#include "AIUtil/SseParser.h"

// ---------------- SseParser 实现 ----------------

// 在 s 中找事件分隔符（\n\n 或 \r\n\r\n），返回 {位置,长度}；找不到位置为 npos
std::pair<size_t, size_t> SseParser::findSep(const std::string& s) {
    const size_t npos = std::string::npos;
    size_t p1 = s.find("\n\n");
    size_t p2 = s.find("\r\n\r\n");
    if (p1 == npos && p2 == npos) return {npos, 0};
    if (p2 == npos || (p1 != npos && p1 < p2)) return {p1, 2};
    return {p2, 4};
}

// 喂入一段新数据；判定格式后按事件切分解析
bool SseParser::feed(const std::string& chunk, std::vector<std::string>& deltas) {
    pending += chunk;

    // 首块数据判定格式：跳过前导空白后是否以 "data:" 开头
    if (!formatChecked) {
        size_t i = 0;
        while (i < pending.size() &&
               (pending[i] == '\r' || pending[i] == '\n' || pending[i] == ' ')) {
            ++i;
        }
        if (i >= pending.size()) return true;      // 还没有效字符，继续等下一块
        formatChecked = true;
        if (pending.compare(i, 5, "data:") != 0) {
            plainJson = true;                       // 全量 JSON（服务端忽略 stream）
            return false;
        }
    }
    if (plainJson) return false;

    // 按 "\n\n" / "\r\n\r\n" 切事件；剩余不足一个完整事件的留在 pending
    auto [pos, len] = findSep(pending);
    while (pos != std::string::npos) {
        std::string event = pending.substr(0, pos);
        pending.erase(0, pos + len);
        handleEvent(event, deltas);
        std::tie(pos, len) = findSep(pending);
    }
    return true;
}

// 流结束后处理 pending 中残留的最后一个事件（无结尾分隔符的尾巴）
void SseParser::flush(std::vector<std::string>& deltas) {
    if (plainJson || pending.empty()) { pending.clear(); return; }
    std::string event = std::move(pending);
    pending.clear();
    handleEvent(event, deltas);
}

// 解析单条事件：聚合多行 data，[DONE] 置完成标志，其余 json 取增量与 usage
void SseParser::handleEvent(const std::string& event, std::vector<std::string>& deltas) {
    // 逐行提取 "data: xxx"（SSE 规范多行 data 以 \n 拼接）
    std::string data;
    size_t start = 0;
    while (start < event.size()) {
        size_t nl = event.find('\n', start);
        std::string line = event.substr(start,
            nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) == 0) {
            size_t d = 5;
            while (d < line.size() && line[d] == ' ') ++d;
            if (!data.empty()) data += '\n';
            data += line.substr(d);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (data.empty()) return;

    if (data == "[DONE]") { done = true; return; }

    // OpenAI 兼容增量：choices[0].delta.content（可能缺失/为空，容错）
    try {
        json j = json::parse(data);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& c = j["choices"][0];
            if (c.contains("delta") && c["delta"].contains("content")) {
                const auto& content = c["delta"]["content"];
                if (content.is_string()) {
                    std::string s = content.get<std::string>();
                    if (!s.empty()) deltas.push_back(std::move(s));
                }
            }
        }
        // 部分服务在最后一帧带 usage（容错累加）
        if (j.contains("usage")) {
            usagePrompt += j["usage"].value("prompt_tokens", 0);
            usageCompletion += j["usage"].value("completion_tokens", 0);
        }
    } catch (...) {
        // 无法解析的事件（心跳注释等）直接忽略
    }
}
