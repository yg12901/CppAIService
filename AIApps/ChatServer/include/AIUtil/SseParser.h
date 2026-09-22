#pragma once
#include <string>
#include <vector>
#include <utility>

#include "../../../../HttpServer/include/utils/JsonUtil.h"

/**
 * SseParser - LLM SSE 增量流解析器（独立可单测）
 * 输入：WriteCallback 多次喂入的原始字节（事件可能跨回调到达）
 * 输出：每次 feed 解析出 0..n 条 delta 文本
 * 状态保留在成员 pending 中，直到收到 "\n\n" 分隔的完整事件才处理
 */
struct SseParser {
    std::string pending;        // 跨回调的未完成事件尾巴
    bool done = false;          // 已收到 [DONE] 结束标记
    bool formatChecked = false; // 是否已判定响应格式
    bool plainJson = false;     // true = 服务端返回全量 JSON（未走流式）
    int  usagePrompt = 0;       // 最后事件可能携带的 usage（容错解析）
    int  usageCompletion = 0;
    std::string lastOutputText; // RAG 应用 API：已吐出的 output.text（兼容累积/增量）

    // 喂入一段新数据；deltas 输出本次解析出的增量文本
    // 返回 false 表示检测到非 SSE 响应（全量 JSON），调用方应停止流式处理
    bool feed(const std::string& chunk, std::vector<std::string>& deltas);

    // 处理流结束后仍残留在 pending 中的最后一个事件（无结尾分隔符的尾巴）
    void flush(std::vector<std::string>& deltas);

private:
    // 在 s 中找事件分隔符（\n\n 或 \r\n\r\n），返回 {位置,长度}；找不到位置为 npos
    static std::pair<size_t, size_t> findSep(const std::string& s);
    // 解析单条事件文本（多行），提取 data 行并解析出 delta
    void handleEvent(const std::string& event, std::vector<std::string>& deltas);
};
