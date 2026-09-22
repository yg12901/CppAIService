// SseParser 单元测试：覆盖跨回调切分、[DONE]、非 SSE 回退、\r\n 分隔、容错、flush 尾巴
// 编译：g++ -std=c++17 -I ../../include -I ../../../HttpServer/include \
//        -o test_sse test_sse.cpp ../src/AIUtil/SseParser.cpp && ./test_sse
#include "../include/AIUtil/SseParser.h"
#include <cassert>
#include <iostream>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::cout << "FAIL line " << __LINE__ << ": " #cond << std::endl; } \
} while (0)

int main() {
    // ---- 1. 基本流：两个事件一次到达 ----
    {
        SseParser p;
        std::vector<std::string> d;
        CHECK(p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n"
                     "data: {\"choices\":[{\"delta\":{\"content\":\"，世界\"}}]}\n\n", d));
        CHECK(!p.plainJson);
        CHECK(d.size() == 2);
        CHECK(d[0] == "你好" && d[1] == "，世界");
        CHECK(!p.done);
    }

    // ---- 2. 事件跨多次 feed 到达（半截 JSON 也不能崩） ----
    {
        SseParser p;
        std::vector<std::string> d;
        // 半截事件
        CHECK(p.feed("data: {\"choices\":[{\"delta\":{\"cont", d));
        CHECK(d.empty());                          // 没凑齐一个事件，不出 delta
        // 补齐剩余
        CHECK(p.feed("ent\":\"北京天气\"}}]}\n\n", d));
        CHECK(d.size() == 1);
        CHECK(d[0] == "北京天气");
    }

    // ---- 3. [DONE] 结束标记 ----
    {
        SseParser p;
        std::vector<std::string> d;
        p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"A\"}}]}\n\ndata: [DONE]\n\n", d);
        CHECK(d.size() == 1);
        CHECK(p.done);
    }

    // ---- 4. 非 SSE（全量 JSON）检测 → feed 返回 false ----
    {
        SseParser p;
        std::vector<std::string> d;
        CHECK(!p.feed("{\"choices\":[{\"message\":{\"content\":\"full\"}}]}", d));
        CHECK(p.plainJson);
        CHECK(d.empty());
        // 后续继续喂也保持 false
        CHECK(!p.feed(" more", d));
    }

    // ---- 5. \r\n\r\n 分隔符（Windows 风格） ----
    {
        SseParser p;
        std::vector<std::string> d;
        CHECK(p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"CRLF\"}}]}\r\n\r\n", d));
        CHECK(d.size() == 1);
        CHECK(d[0] == "CRLF");
    }

    // ---- 6. 容错：delta 缺 content / content 为空 / 非法 JSON ----
    {
        SseParser p;
        std::vector<std::string> d;
        p.feed("data: {\"choices\":[{\"delta\":{}}]}\n\n", d);          // 缺 content
        p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"\"}}]}\n\n", d); // 空串
        p.feed("data: not-a-json\n\n", d);                                // 非法 JSON
        p.feed(": this is a comment\n\n", d);                             // SSE 注释行
        p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n", d);
        CHECK(d.size() == 1);
        CHECK(d[0] == "ok");
    }

    // ---- 7. 最后一帧携带 usage ----
    {
        SseParser p;
        std::vector<std::string> d;
        p.feed("data: {\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":31,\"completion_tokens\":7}}\n\n", d);
        CHECK(p.usagePrompt == 31 && p.usageCompletion == 7);
    }

    // ---- 8. flush：流结束时 pending 里没有结尾分隔符的尾巴 ----
    {
        SseParser p;
        std::vector<std::string> d;
        p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"头\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"尾巴\"}}]}", d);  // 无结尾分隔符
        CHECK(d.size() == 1);
        p.flush(d);   // 收尾
        CHECK(d.size() == 2);
        CHECK(d[1] == "尾巴");
    }

    // ---- 9. 首块只有空白/换行：不判定格式，等待后续 ----
    {
        SseParser p;
        std::vector<std::string> d;
        CHECK(p.feed("\r\n", d));     // 无有效字符
        CHECK(!p.formatChecked);
        CHECK(p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"X\"}}]}\n\n", d));
        CHECK(d.size() == 1 && d[0] == "X");
    }

    // ---- 10. 特殊字符（引号/换行转义后的 JSON 值） ----
    {
        SseParser p;
        std::vector<std::string> d;
        p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"a\\\"b\\\\c\"}}]}\n\n", d);
        CHECK(d.size() == 1);
        CHECK(d[0] == "a\"b\\c");
    }

    // ---- 11. 百炼应用 SSE：id/event 开头 + output.text 增量 ----
    {
        SseParser p;
        std::vector<std::string> d;
        CHECK(p.feed("id:1\nevent:result\n:HTTP_STATUS/200\n"
                     "data: {\"output\":{\"text\":\"你\",\"finish_reason\":\"null\"}}\n\n", d));
        CHECK(!p.plainJson);
        CHECK(d.size() == 1 && d[0] == "你");
        d.clear();
        p.feed("data: {\"output\":{\"text\":\"好\",\"finish_reason\":\"stop\"},"
               "\"usage\":{\"models\":[{\"input_tokens\":12,\"output_tokens\":2}]}}\n\n", d);
        CHECK(d.size() == 1 && d[0] == "好");
        CHECK(p.done);
        CHECK(p.usagePrompt == 12 && p.usageCompletion == 2);
        CHECK(p.lastOutputText == "你好");
    }

    // ---- 12. 百炼应用 SSE：output.text 累积全文，只推新增后缀 ----
    {
        SseParser p;
        std::vector<std::string> d;
        p.feed("data: {\"output\":{\"text\":\"北\"}}\n\n", d);
        p.feed("data: {\"output\":{\"text\":\"北京\"}}\n\n", d);
        p.feed("data: {\"output\":{\"text\":\"北京天气\"}}\n\n", d);
        CHECK(d.size() == 3);
        CHECK(d[0] == "北" && d[1] == "京" && d[2] == "天气");
    }

    std::cout << (g_fail == 0 ? "ALL PASS" : "HAS FAILURES")
              << " (pass=" << g_pass << " fail=" << g_fail << ")" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
