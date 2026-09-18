// AIConfig 工具调用解析单元测试
// 覆盖四层递进解析（直接 / 代码围栏 / 括号配对 / 纯正则兜底）与格式校验各条规则。
// 编译：g++ -std=c++17 -I ../include -I ../../../HttpServer/include \
//        -o test_tool_parse test_tool_parse.cpp ../src/AIUtil/AIConfig.cpp && ./test_tool_parse
#include "../include/AIUtil/AIConfig.h"
#include <cstdio>
#include <iostream>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::cout << "FAIL line " << __LINE__ << ": " #cond << std::endl; } \
} while (0)

// 就地造一份和 resource/config.json 同构的配置，测试不依赖外部文件
static const char* kConfigJson = R"JSON({
  "prompt_template": "问题：{user_input}\n工具：\n{tool_list}\n",
  "tools": [
    { "name": "get_weather", "params": { "city": "北京" }, "desc": "查询天气" },
    { "name": "get_time",    "params": {},                 "desc": "获取时间" }
  ]
})JSON";

int main() {
    const std::string path = "/tmp/_ai_config_test.json";
    {
        FILE* f = std::fopen(path.c_str(), "w");
        std::fputs(kConfigJson, f);
        std::fclose(f);
    }

    AIConfig cfg;
    CHECK(cfg.loadFromFile(path));
    CHECK(cfg.hasTool("get_weather"));
    CHECK(!cfg.hasTool("get_stock"));

    // ---- L0：模型严格照做，整段就是一个 JSON ----
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_weather","args":{"city":"北京"}})");
        CHECK(c.isToolCall);
        CHECK(c.parseStage == "direct");
        CHECK(c.toolName == "get_weather");
        CHECK(c.args["city"] == "北京");
    }

    // ---- L1：被 markdown 代码围栏包住（提示词明令禁止，模型照样会干） ----
    {
        auto c = cfg.parseAIResponse("```json\n{\"tool\":\"get_time\",\"args\":{}}\n```");
        CHECK(c.isToolCall);
        CHECK(c.parseStage == "code_fence");
        CHECK(c.toolName == "get_time");
    }
    {
        // 不带语言标注的围栏
        auto c = cfg.parseAIResponse("```\n{\"tool\":\"get_weather\",\"args\":{\"city\":\"上海\"}}\n```");
        CHECK(c.isToolCall);
        CHECK(c.args["city"] == "上海");
    }

    // ---- L2：JSON 被闲聊夹在中间 ----
    {
        auto c = cfg.parseAIResponse(
            "好的，我帮你查一下：{\"tool\":\"get_weather\",\"args\":{\"city\":\"深圳\"}} 请稍等。");
        CHECK(c.isToolCall);
        CHECK(c.parseStage == "brace_scan");
        CHECK(c.args["city"] == "深圳");
    }
    {
        // 参数值里带花括号，括号配对必须识别字符串字面量，不能提前截断
        auto c = cfg.parseAIResponse(
            "结果如下 {\"tool\":\"get_weather\",\"args\":{\"city\":\"北}京\"}} 完毕");
        CHECK(c.isToolCall);
        CHECK(c.args["city"] == "北}京");
    }

    // ---- L3：JSON 语法本身坏了（单引号 / 键没加引号 / 中文冒号） ----
    {
        auto c = cfg.parseAIResponse("{'tool': 'get_weather', 'args': {'city': '杭州'}}");
        CHECK(c.isToolCall);
        CHECK(c.parseStage == "regex_field");
        CHECK(c.args["city"] == "杭州");
    }
    {
        auto c = cfg.parseAIResponse("{tool：\"get_time\", args：{}}");
        CHECK(c.isToolCall);
        CHECK(c.toolName == "get_time");
    }

    // ---- 正常文本回答：不能被误判成工具调用 ----
    {
        auto c = cfg.parseAIResponse("今天北京天气不错，气温 20 度左右。");
        CHECK(!c.isToolCall);
        CHECK(c.rejectReason.empty());   // 压根没想调工具，静默走文本
    }
    {
        // 模型正经返回了一段 JSON 数据，但不是工具调用
        auto c = cfg.parseAIResponse(R"({"answer":"北京今天晴","source":"记忆"})");
        CHECK(!c.isToolCall);
        CHECK(c.rejectReason.empty());
    }

    // ---- 格式校验：工具名白名单（防模型幻觉） ----
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_stock_price","args":{"code":"600519"}})");
        CHECK(!c.isToolCall);
        CHECK(c.rejectReason.find("未注册的工具") != std::string::npos);
    }

    // ---- 格式校验：必填参数缺失 ----
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_weather","args":{}})");
        CHECK(!c.isToolCall);
        CHECK(c.rejectReason.find("缺少必填参数") != std::string::npos);
    }
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_weather","args":{"city":"   "}})");
        CHECK(!c.isToolCall);
        CHECK(c.rejectReason.find("参数为空") != std::string::npos);
    }

    // ---- 格式校验：args 被套了一层字符串 ----
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_weather","args":"{\"city\":\"成都\"}"})");
        CHECK(c.isToolCall);
        CHECK(c.args["city"] == "成都");
    }

    // ---- 格式校验：参数值不是字符串时归一化 ----
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_weather","args":{"city":12345}})");
        CHECK(c.isToolCall);
        CHECK(c.args["city"] == "12345");
    }

    // ---- 格式校验：多余的幻觉参数被剔除，不透传给工具 ----
    {
        auto c = cfg.parseAIResponse(
            R"({"tool":"get_weather","args":{"city":"武汉","unit":"celsius","days":7}})");
        CHECK(c.isToolCall);
        CHECK(c.args.size() == 1);
        CHECK(c.args.contains("city"));
        CHECK(!c.args.contains("unit"));
    }

    // ---- 无参工具：args 缺省也应通过 ----
    {
        auto c = cfg.parseAIResponse(R"({"tool":"get_time"})");
        CHECK(c.isToolCall);
        CHECK(c.args.empty());
    }

    // ---- 边界：空响应 / 纯空白 ----
    {
        CHECK(!cfg.parseAIResponse("").isToolCall);
        CHECK(!cfg.parseAIResponse("   \n\t ").isToolCall);
    }

    // ---- 边界：花括号没配平（输出被截断） ----
    {
        auto c = cfg.parseAIResponse("{\"tool\":\"get_weather\",\"args\":{\"city\":\"南京\"");
        CHECK(!c.isToolCall);
    }

    // ---- buildPrompt：$ 不能被当成正则反向引用展开（提示词注入） ----
    {
        std::string p = cfg.buildPrompt("帮我查 $& 和 $1 的天气");
        CHECK(p.find("$& 和 $1") != std::string::npos);
        CHECK(p.find("{user_input}") == std::string::npos);
        CHECK(p.find("get_weather") != std::string::npos);   // 工具清单已渲染
    }

    std::cout << "passed=" << g_pass << " failed=" << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
