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
        // 百炼应用 SSE 常以 id: / event: / :HTTP_STATUS 开头，不一定第一行就是 data:
        // 全量 JSON 以 '{' 起头。其余按 SSE 继续切事件。
        if (pending[i] == '{') {
            plainJson = true;
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

    try {
        json j = json::parse(data);
        bool gotOpenAiDelta = false;
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& c = j["choices"][0];
            if (c.contains("delta") && c["delta"].contains("content")) {
                const auto& content = c["delta"]["content"];
                if (content.is_string()) {
                    std::string s = content.get<std::string>();
                    if (!s.empty()) {
                        deltas.push_back(std::move(s));
                        gotOpenAiDelta = true;
                    }
                }
            }
        }
        // 百炼应用 API：output.text。incremental_output=true 时是增量；
        // false 时是累积全文，用 lastOutputText 算出新增后缀，避免重复推。
        if (!gotOpenAiDelta && j.contains("output") && j["output"].contains("text")
            && j["output"]["text"].is_string()) {
            std::string text = j["output"]["text"].get<std::string>();
            if (!text.empty()) {
                if (lastOutputText.empty()) {
                    deltas.push_back(text);
                    lastOutputText = text;
                } else if (text.size() > lastOutputText.size()
                           && text.compare(0, lastOutputText.size(), lastOutputText) == 0) {
                    deltas.push_back(text.substr(lastOutputText.size()));
                    lastOutputText = std::move(text);
                } else if (text != lastOutputText) {
                    deltas.push_back(text);
                    lastOutputText += text;
                }
            }
            if (j["output"].contains("finish_reason") && j["output"]["finish_reason"].is_string()) {
                const std::string reason = j["output"]["finish_reason"].get<std::string>();
                if (!reason.empty() && reason != "null") done = true;
            }
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            const auto& u = j["usage"];
            if (u.contains("models") && u["models"].is_array()) {
                int in = 0, out = 0;
                for (const auto& m : u["models"]) {
                    in += m.value("input_tokens", 0);
                    out += m.value("output_tokens", 0);
                }
                usagePrompt = in;
                usageCompletion = out;
            } else {
                usagePrompt += u.value("prompt_tokens", 0);
                usageCompletion += u.value("completion_tokens", 0);
            }
        }
    } catch (...) {
        // 无法解析的事件（心跳注释等）直接忽略
    }
}
