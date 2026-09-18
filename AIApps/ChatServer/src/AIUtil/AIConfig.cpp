#include"../include/AIUtil/AIConfig.h"
#include <mutex>
#include <algorithm>
#include <cstdlib>

/**
 * AIConfig - MCP Prompt 模板管理 + 工具调用解析
 * 职责：
 *   1) 从 config.json 加载提示词模板和工具清单
 *   2) buildPrompt        —— 把用户问题和工具清单塞进模板
 *   3) parseAIResponse    —— 四层递进解析 + 格式校验，把模型输出翻译成工具调用意图
 *   4) buildToolResultPrompt —— 把工具执行结果拼成第二段推理的提示词
 */

namespace {

// 配置路径：优先 setConfigPath 显式指定，其次环境变量，最后内置默认值
std::string g_configPath;
std::once_flag g_loadOnce;

const char* kDefaultConfigPath = "../AIApps/ChatServer/resource/config.json";

} // namespace

void AIConfig::setConfigPath(const std::string& path) {
    g_configPath = path;
}

AIConfig& AIConfig::instance() {
    static AIConfig cfg;
    // call_once 保证多线程首次并发访问时只加载一次，且后来者会等加载完成再返回
    std::call_once(g_loadOnce, [&] {
        if (g_configPath.empty()) {
            const char* env = std::getenv("AI_CONFIG_PATH");
            g_configPath = (env && *env) ? env : kDefaultConfigPath;
        }
        if (!cfg.loadFromFile(g_configPath)) {
            std::cerr << "[AIConfig] FATAL: failed to load " << g_configPath
                      << ", tool calling will be disabled" << std::endl;
        }
    });
    return cfg;
}

bool AIConfig::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[AIConfig] Unable to open configuration file: " << path << std::endl;
        return false;
    }

    json j;
    try {
        file >> j;
    }
    catch (const std::exception& e) {
        // 原实现没兜住这里：配置文件写坏会直接抛到调用栈顶把请求打挂
        std::cerr << "[AIConfig] malformed json in " << path << ": " << e.what() << std::endl;
        return false;
    }

    // Parsing templates
    if (!j.contains("prompt_template") || !j["prompt_template"].is_string()) {
        std::cerr << "[AIConfig] prompt_template is missing" << std::endl;
        return false;
    }
    promptTemplate_ = j["prompt_template"].get<std::string>();

    // List of parsing tools
    tools_.clear();
    if (j.contains("tools") && j["tools"].is_array()) {
        for (auto& tool : j["tools"]) {
            AITool t;
            t.name = tool.value("name", "");
            t.desc = tool.value("desc", "");
            if (t.name.empty()) {
                std::cerr << "[AIConfig] skip a tool with empty name" << std::endl;
                continue;
            }
            if (tool.contains("params") && tool["params"].is_object()) {
                for (auto& [key, val] : tool["params"].items()) {
                    // 示例值允许写成非字符串（数字等），统一转成字符串存
                    t.params[key] = val.is_string() ? val.get<std::string>() : val.dump();
                }
            }
            tools_.push_back(std::move(t));
        }
    }
    std::cout << "[AIConfig] loaded " << tools_.size() << " tool(s) from " << path << std::endl;
    return true;
}

const AITool* AIConfig::findTool(const std::string& name) const {
    for (const auto& t : tools_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

std::string AIConfig::trimCopy(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// 工具清单渲染进提示词。带上示例值，模型照着抄格式的成功率明显高于只给参数名。
std::string AIConfig::buildToolList() const {
    std::ostringstream oss;
    for (const auto& t : tools_) {
        oss << t.name << "(";
        bool first = true;
        for (const auto& [key, sample] : t.params) {
            if (!first) oss << ", ";
            oss << key;
            if (!sample.empty()) oss << "=\"" << sample << "\"";
            first = false;
        }
        oss << ") -> " << t.desc << "\n";
    }
    return oss.str();
}

std::string AIConfig::buildPrompt(const std::string& userInput) const {
    // 这里刻意不用 regex_replace：它的替换串里 $ 是特殊字符，
    // 用户只要输入 "$&" 或 "$1"，就会被当成反向引用展开，属于提示词注入。
    // 普通字符串查找替换既没有这个坑，也省掉每次请求编译两个正则的开销。
    auto replaceAll = [](std::string text, const std::string& from, const std::string& to) {
        if (from.empty()) return text;
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();   // 从替换结果之后继续找，避免 to 里含 from 时无限循环
        }
        return text;
    };

    std::string result = promptTemplate_;
    result = replaceAll(result, "{user_input}", userInput);
    result = replaceAll(result, "{tool_list}", buildToolList());
    return result;
}

// ---------------- 解析流水线 ----------------

bool AIConfig::tryParseObject(const std::string& text, json& out) {
    if (text.empty()) return false;
    // allow_exceptions=false：模型大多数时候返回的是自然语言，解析失败是常态而非异常。
    // 用抛异常来表达"这不是 JSON"，在高频路径上是笔纯亏的开销。
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return false;
    out = std::move(parsed);
    return true;
}

// 第 1 层：模型很爱把 JSON 包在 markdown 代码块里，哪怕提示词里明令禁止。
std::string AIConfig::stripCodeFence(const std::string& text) {
    // 匹配 ```json\n ... ``` 或 ``` ... ```，捕获中间内容（非贪婪，取第一段）
    static const std::regex kFence(R"(```[ \t]*[A-Za-z0-9_+-]*[ \t]*\r?\n?([\s\S]*?)```)");
    std::smatch m;
    if (std::regex_search(text, m, kFence)) return trimCopy(m[1].str());
    return {};
}

// 第 2 层：JSON 被闲聊夹在中间，例如
//   好的，我来帮你查天气：{"tool":"get_weather","args":{"city":"北京"}} 稍等
// 从第一个 '{' 起做括号配对扫描，取出第一个完整对象。
// 必须识别字符串字面量和转义，否则参数值里出现 '}' 就会提前截断。
bool AIConfig::extractBalancedObject(const std::string& text, std::string& out) {
    size_t start = text.find('{');
    if (start == std::string::npos) return false;

    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = start; i < text.size(); ++i) {
        char c = text[i];
        if (esc) { esc = false; continue; }          // 上一个字符是反斜杠，本字符无条件跳过
        if (c == '\\') { if (inStr) esc = true; continue; }
        if (c == '"') { inStr = !inStr; continue; }
        if (inStr) continue;                          // 字符串内部的花括号不参与配对
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                out = text.substr(start, i - start + 1);
                return true;
            }
        }
    }
    return false;   // 花括号没配平，说明输出被截断了
}

// 第 3 层：最后一道兜底。此时 JSON 本身语法就是坏的——单引号、键没加引号、
// 尾随逗号、中文冒号之类，任何解析器都救不回来，只能用正则硬抠字段。
bool AIConfig::regexExtractToolCall(const std::string& text, json& out) {
    // 冒号写成 (?::|：) 而不是字符类 [:：]：std::regex 是按字节匹配的，
    // 全角冒号 U+FF1A 在 UTF-8 下是 EF BC 9A 三个字节，塞进字符类只会各匹配一个字节，
    // 结果是消耗掉 0xEF 后就对不上了。用分支才能把三字节当成一个整体去匹配。
    // 兼容双引号、单引号、无引号三种键写法，值兼容双引号和单引号
    static const std::regex kTool(R"(["']?tool["']?\s*(?::|：)\s*["']([^"']+)["'])");
    std::smatch m;
    if (!std::regex_search(text, m, kTool)) return false;

    out = json::object();
    out["tool"] = trimCopy(m[1].str());

    json args = json::object();
    // args 后面那一对花括号里的内容（不含嵌套，兜底层不追求处理复杂结构）
    static const std::regex kArgsBlock(R"(["']?args["']?\s*(?::|：)\s*\{([^{}]*)\})");
    std::smatch am;
    if (std::regex_search(text, am, kArgsBlock)) {
        const std::string body = am[1].str();
        static const std::regex kPair(R"(["']?([A-Za-z_][A-Za-z0-9_]*)["']?\s*(?::|：)\s*["']([^"']*)["'])");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), kPair);
             it != std::sregex_iterator(); ++it) {
            args[(*it)[1].str()] = (*it)[2].str();
        }
    }
    out["args"] = args;
    return true;
}

// 格式校验：解析出来只说明"长得像工具调用"，能不能真执行还得逐项过一遍。
AIToolCall AIConfig::validateToolCall(const json& obj) const {
    AIToolCall call;

    if (!obj.is_object()) {
        call.rejectReason = "顶层不是 JSON 对象";
        return call;
    }

    auto itTool = obj.find("tool");
    if (itTool == obj.end()) {
        // 模型返回了 JSON 但没有 tool 字段，说明它本来就没想调工具，静默走文本回答
        return call;
    }
    if (!itTool->is_string()) {
        call.rejectReason = "tool 字段不是字符串";
        return call;
    }

    std::string name = trimCopy(itTool->get<std::string>());
    if (name.empty()) {
        call.rejectReason = "tool 名为空";
        return call;
    }

    // 白名单校验：模型会凭空编出 get_stock_price 这种根本没注册的工具。
    // 不拦住的话就会一路走到 registry.invoke 抛异常，用户看到的是一句报错。
    const AITool* spec = findTool(name);
    if (!spec) {
        call.rejectReason = "未注册的工具: " + name;
        return call;
    }

    // args 归一化
    json args = json::object();
    auto itArgs = obj.find("args");
    if (itArgs != obj.end() && !itArgs->is_null()) {
        if (itArgs->is_object()) {
            args = *itArgs;
        }
        else if (itArgs->is_string()) {
            // 模型偶尔会把 args 再套一层字符串： "args": "{\"city\":\"北京\"}"
            json inner;
            if (!tryParseObject(itArgs->get<std::string>(), inner)) {
                call.rejectReason = "args 是字符串且无法二次解析";
                return call;
            }
            args = std::move(inner);
        }
        else {
            call.rejectReason = "args 不是对象";
            return call;
        }
    }

    // 必填参数齐全性：config.json 里 params 声明的每个键都必须给到且非空。
    // 提示词里已经交代过"参数对不上就直接文本回答"，这里是代码侧的最后一道闸。
    json cleaned = json::object();
    for (const auto& [key, sample] : spec->params) {
        auto itVal = args.find(key);
        if (itVal == args.end() || itVal->is_null()) {
            call.rejectReason = "缺少必填参数: " + key;
            return call;
        }
        // 值类型归一成字符串：模型可能把城市写成数字、把开关写成 bool，
        // 下游工具函数统一按字符串取，省得每个工具各写一遍类型判断。
        std::string v = itVal->is_string() ? itVal->get<std::string>() : itVal->dump();
        v = trimCopy(v);
        if (v.empty()) {
            call.rejectReason = "参数为空: " + key;
            return call;
        }
        cleaned[key] = v;
    }
    // 注意 cleaned 只装了声明过的参数：模型多塞的幻觉参数（比如凭空加个 "unit"）
    // 到这一步被自然丢弃，不会透传给工具函数。

    call.toolName = name;
    call.args = std::move(cleaned);
    call.isToolCall = true;
    return call;
}

AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;

    const std::string text = trimCopy(response);
    if (text.empty()) {
        result.rejectReason = "空响应";
        return result;
    }

    // 四层候选按可信度从高到低排列，逐个过校验，第一个通过的就是答案。
    // 之所以是"先全部收集再逐个校验"而不是"每层解析完立刻返回"：
    // 某一层可能解析出一个合法 JSON 但它并不是工具调用（比如模型正经回答了一段 JSON 数据），
    // 这时候应该让后面的层继续尝试，而不是就此收工。
    struct Candidate { const char* stage; json obj; };
    std::vector<Candidate> candidates;
    json obj;

    // L0 直接解析：模型严格照做，整段就是一个 JSON
    if (tryParseObject(text, obj)) {
        candidates.push_back({ "direct", std::move(obj) });
    }
    // L1 剥 markdown 代码围栏
    const std::string unfenced = stripCodeFence(text);
    if (!unfenced.empty() && unfenced != text && tryParseObject(unfenced, obj)) {
        candidates.push_back({ "code_fence", std::move(obj) });
    }
    // L2 从闲聊里括号配对抠 JSON
    std::string braced;
    if (extractBalancedObject(text, braced) && braced != text && tryParseObject(braced, obj)) {
        candidates.push_back({ "brace_scan", std::move(obj) });
    }
    // L3 纯正则抽字段（JSON 语法已损坏）。
    // 只在前三层全部落空时才启用：这层匹配得很宽松，用户正常聊天里要是出现
    // "tool: '锤子'" 这种字样也会被它误判成工具调用，因此必须放在最后且带前提。
    if (candidates.empty() && regexExtractToolCall(text, obj)) {
        candidates.push_back({ "regex_field", std::move(obj) });
    }

    for (auto& c : candidates) {
        AIToolCall cur = validateToolCall(c.obj);
        cur.parseStage = c.stage;
        if (cur.isToolCall) {
            if (std::string(c.stage) != "direct") {
                // 埋点：统计兜底层的命中率，能直接反映提示词的约束效果好不好
                std::cout << "[MCP] tool call recovered by fallback, stage=" << c.stage
                          << ", tool=" << cur.toolName << std::endl;
            }
            return cur;
        }
        // 留下第一条"看起来想调工具但没过校验"的原因，便于线上定位
        if (result.rejectReason.empty() && !cur.rejectReason.empty()) {
            result.rejectReason = cur.rejectReason;
            result.parseStage = c.stage;
        }
    }

    if (!result.rejectReason.empty()) {
        std::cout << "[MCP] tool call rejected at stage=" << result.parseStage
                  << ", reason=" << result.rejectReason << std::endl;
    }
    return result;   // isToolCall = false → 上层把模型原文当作最终答案
}

std::string AIConfig::buildToolResultPrompt(
    const std::string& userInput,
    const std::string& toolName,
    const json& toolArgs,
    const json& toolResult) const
{
    std::ostringstream oss;
    oss << "下面是用户说的话：" << userInput << "\n"
        << "我刚才调用了工具 [" << toolName << "] ，参数为："
        << toolArgs.dump() << "\n"
        << "工具返回的结果如下：\n" << toolResult.dump(4) << "\n"
        << "请根据以上信息，用自然语言回答用户。";
    return oss.str();
}
