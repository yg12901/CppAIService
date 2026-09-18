#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include <fstream>
#include <sstream>
#include <iostream>
#include "../../../../HttpServer/include/utils/JsonUtil.h"  


struct AITool {
    std::string name;
    // key = 参数名，value = config.json 里给的示例值（用来在提示词里示范格式）
    // 约定：出现在这里的参数一律视为必填
    std::unordered_map<std::string, std::string> params;
    std::string desc;
};


struct AIToolCall {
    std::string toolName;
    json args;
    bool isToolCall = false;

    // ---- 可观测字段：线上排查"为什么这次没调工具"全靠它俩 ----
    // 本次结果由哪一层解析得出：direct / code_fence / brace_scan / regex_field / none
    std::string parseStage = "none";
    // 格式校验未通过的原因；为空表示模型压根没打算调工具（正常走文本回答）
    std::string rejectReason;
};


class AIConfig {
public:
    // 进程内单例：提示词模板和工具清单加载一次就不再变，
    // 之前每次对话都重新开文件 + 解析 JSON，是白白压在热路径上的磁盘 IO。
    // 加载完之后所有成员只读，多线程并发读无需加锁。
    static AIConfig& instance();
    // 启动时可覆盖配置路径；不调用则依次取环境变量 AI_CONFIG_PATH、内置默认相对路径
    static void setConfigPath(const std::string& path);

    bool loadFromFile(const std::string& path);
    std::string buildPrompt(const std::string& userInput) const;

    // 把模型返回的那坨文本翻译成"要不要调工具、调哪个、参数是什么"。
    // 四层递进解析 + 一道格式校验，详见 .cpp 中的实现说明。
    AIToolCall parseAIResponse(const std::string& response) const;

    std::string buildToolResultPrompt(const std::string& userInput,const std::string& toolName,const json& toolArgs,const json& toolResult) const;

    // 工具清单查询：校验模型报出来的工具名是不是真存在（防幻觉）
    const AITool* findTool(const std::string& name) const;
    bool hasTool(const std::string& name) const { return findTool(name) != nullptr; }
    bool empty() const { return tools_.empty(); }

private:
    std::string promptTemplate_;
    std::vector<AITool> tools_;

    std::string buildToolList() const;

    // ---- 解析流水线的各层实现 ----
    // 不抛异常地尝试把一段文本解析成 JSON 对象
    static bool tryParseObject(const std::string& text, json& out);
    // 第 1 层兜底：正则剥掉 markdown 代码围栏 ```json ... ```
    static std::string stripCodeFence(const std::string& text);
    // 第 2 层兜底：括号配对扫描，从夹带闲聊的文本里抠出第一个完整的 {...}
    static bool extractBalancedObject(const std::string& text, std::string& out);
    // 第 3 层兜底：连 JSON 语法都坏了时，纯正则直接抽 tool 名和 args 键值对
    static bool regexExtractToolCall(const std::string& text, json& out);
    // 格式校验：工具名白名单、args 归一化、必填参数齐全性、多余参数剔除
    AIToolCall validateToolCall(const json& obj) const;

    static std::string trimCopy(const std::string& s);
};
