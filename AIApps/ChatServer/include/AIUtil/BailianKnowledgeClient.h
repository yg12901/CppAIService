#pragma once

#include <string>
#include "../../../../HttpServer/include/utils/JsonUtil.h"

/**
 * BailianKnowledgeClient - 把本地文件灌进阿里百炼「共享」知识库
 *
 * 问的链路（AliyunRAGStrategy）不用改。这里只补灌库：
 *   ApplyFileUploadLease → PUT 二进制到租约 URL → AddFile → SubmitIndexAddDocumentsJob
 * 索引是异步的，ingest() 在提交任务后就返回 jobId，不要在请求里干等到 COMPLETED。
 *
 * 环境变量：
 *   DASHSCOPE_API_KEY      已有
 *   BAILIAN_WORKSPACE_ID   业务空间 ID（控制台左上角）
 *   BAILIAN_INDEX_ID       知识库 IndexId（不是应用 ID / Knowledge_Base_ID）
 *   BAILIAN_CATEGORY_ID    可选，默认 default
 *   BAILIAN_API_BASE       可选，默认 https://dashscope.aliyuncs.com/api/v1
 */
class BailianKnowledgeClient {
public:
    struct IngestResult {
        std::string filename;
        std::string fileId;
        std::string jobId;
    };

    struct JobStatus {
        bool        ok = false;
        std::string status;    // COMPLETED / FAILED / RUNNING / PENDING
        std::string message;
        json        documents = json::array();
    };

    static BailianKnowledgeClient& instance();

    IngestResult ingest(const std::string& filename, const std::string& bytes);
    JobStatus    queryJob(const std::string& jobId) const;

private:
    BailianKnowledgeClient() = default;

    struct Env {
        std::string apiKey;
        std::string workspaceId;
        std::string indexId;
        std::string categoryId;
        std::string apiBase;
    };
    static Env loadEnv();

    static std::string md5Hex(const std::string& data);
    static std::string sanitizeFilename(const std::string& raw);
    static bool isAllowedExt(const std::string& filename);
    static bool aliSuccess(const json& j);
    static std::string aliError(const json& j, const std::string& fallback);

    static json httpJson(const std::string& method, const std::string& url,
        const std::string& bearer, const std::string& body,
        long timeoutSec = 30);

    static void httpPutBinary(const std::string& url, const json& extraHeaders,
        const std::string& bytes, long timeoutSec = 120);
};
