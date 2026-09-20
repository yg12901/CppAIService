#include "../include/AIUtil/BailianKnowledgeClient.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <iomanip>
#include <vector>

namespace {

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string getenvOr(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

bool endsWithIgnoreCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(
            s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

} // namespace

BailianKnowledgeClient& BailianKnowledgeClient::instance() {
    static BailianKnowledgeClient c;
    return c;
}

BailianKnowledgeClient::Env BailianKnowledgeClient::loadEnv() {
    Env e;
    e.apiKey      = getenvOr("DASHSCOPE_API_KEY");
    e.workspaceId = getenvOr("BAILIAN_WORKSPACE_ID");
    e.indexId     = getenvOr("BAILIAN_INDEX_ID");
    e.categoryId  = getenvOr("BAILIAN_CATEGORY_ID", "default");
    e.apiBase     = getenvOr("BAILIAN_API_BASE", "https://dashscope.aliyuncs.com/api/v1");
    while (!e.apiBase.empty() && e.apiBase.back() == '/') e.apiBase.pop_back();
    if (e.apiKey.empty())      throw std::runtime_error("DASHSCOPE_API_KEY not found");
    if (e.workspaceId.empty()) throw std::runtime_error("BAILIAN_WORKSPACE_ID not found（百炼控制台左上角业务空间 ID）");
    if (e.indexId.empty())     throw std::runtime_error("BAILIAN_INDEX_ID not found（知识库 IndexId，不是应用 ID / Knowledge_Base_ID）");
    if (e.categoryId.empty())  e.categoryId = "default";
    return e;
}

std::string BailianKnowledgeClient::md5Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("MD5 digest failed");
    }
    EVP_MD_CTX_free(ctx);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i) oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

std::string BailianKnowledgeClient::sanitizeFilename(const std::string& raw) {
    std::string name = raw;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '\0' || c == '\r' || c == '\n') continue;
        out.push_back(static_cast<char>(c));
    }
    if (out.size() < 4) out = "doc_" + out;
    if (out.size() > 128) {
        auto dot = out.find_last_of('.');
        std::string ext = (dot != std::string::npos && dot + 1 < out.size()) ? out.substr(dot) : std::string();
        out = out.substr(0, 128 - ext.size()) + ext;
    }
    return out;
}

bool BailianKnowledgeClient::isAllowedExt(const std::string& filename) {
    static const char* kExts[] = {
        ".pdf", ".doc", ".docx", ".wps", ".ppt", ".pptx",
        ".xls", ".xlsx", ".md", ".txt", ".epub", ".mobi"
    };
    for (const char* e : kExts) {
        if (endsWithIgnoreCase(filename, e)) return true;
    }
    return false;
}

bool BailianKnowledgeClient::aliSuccess(const json& j) {
    if (!j.is_object()) return false;
    if (j.contains("Success")) {
        const auto& s = j["Success"];
        if (s.is_boolean()) return s.get<bool>();
        if (s.is_string()) {
            std::string v = s.get<std::string>();
            return v == "true" || v == "True" || v == "TRUE";
        }
    }
    if (j.contains("Status")) {
        if (j["Status"].is_string()) return j["Status"].get<std::string>() == "200";
        if (j["Status"].is_number_integer()) return j["Status"].get<int>() == 200;
    }
    return false;
}

std::string BailianKnowledgeClient::aliError(const json& j, const std::string& fallback) {
    if (!j.is_object()) return fallback;
    std::string msg = j.value("Message", std::string());
    std::string code = j.value("Code", std::string());
    if (msg.empty() && code.empty()) return fallback;
    if (code.empty()) return msg;
    if (msg.empty()) return code;
    return code + ": " + msg;
}

json BailianKnowledgeClient::httpJson(const std::string& method, const std::string& url,
    const std::string& bearer, const std::string& body, long timeoutSec)
{
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string auth = "Authorization: Bearer " + bearer;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl ") + method + " " + url + " failed: " +
            curl_easy_strerror(rc));
    }

    json parsed = json::parse(response, nullptr, false);
    if (parsed.is_discarded()) {
        throw std::runtime_error("百炼返回非 JSON（HTTP " + std::to_string(httpCode) + "）: " +
            response.substr(0, 240));
    }
    if (httpCode >= 400 && !aliSuccess(parsed)) {
        throw std::runtime_error(aliError(parsed, "HTTP " + std::to_string(httpCode)));
    }
    return parsed;
}

void BailianKnowledgeClient::httpPutBinary(const std::string& url, const json& extraHeaders,
    const std::string& bytes, long timeoutSec)
{
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string response;
    struct curl_slist* headers = nullptr;

    if (extraHeaders.is_object()) {
        for (auto it = extraHeaders.begin(); it != extraHeaders.end(); ++it) {
            std::string v = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            std::string line = it.key() + ": " + v;
            headers = curl_slist_append(headers, line.c_str());
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bytes.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("PUT 文件失败: ") + curl_easy_strerror(rc));
    }
    if (httpCode >= 400) {
        throw std::runtime_error("PUT 文件被拒绝 HTTP " + std::to_string(httpCode) + " " +
            response.substr(0, 240));
    }
}

BailianKnowledgeClient::IngestResult
BailianKnowledgeClient::ingest(const std::string& filename, const std::string& bytes)
{
    const Env env = loadEnv();
    const std::string safeName = sanitizeFilename(filename);
    if (!isAllowedExt(safeName)) {
        throw std::runtime_error("不支持的文件类型，请上传 pdf/doc/docx/ppt/pptx/xls/xlsx/md/txt");
    }
    if (bytes.empty()) throw std::runtime_error("空文件");
    if (bytes.size() > 12ull * 1024ull * 1024ull) {
        throw std::runtime_error("文件超过 12MB，请缩小后再传");
    }

    // 1) 申请上传租约
    json leaseReq;
    leaseReq["FileName"]     = safeName;
    leaseReq["Md5"]          = md5Hex(bytes);
    leaseReq["SizeInBytes"]  = std::to_string(bytes.size());
    leaseReq["CategoryType"] = "UNSTRUCTURED";

    const std::string leaseUrl = env.apiBase + "/" + env.workspaceId +
        "/datacenter/category/" + env.categoryId;
    json leaseResp = httpJson("POST", leaseUrl, env.apiKey, leaseReq.dump());
    if (!aliSuccess(leaseResp)) {
        throw std::runtime_error(aliError(leaseResp, "申请上传租约失败"));
    }
    const json& leaseData = leaseResp.contains("Data") ? leaseResp["Data"] : leaseResp;
    const std::string leaseId = leaseData.value("FileUploadLeaseId", std::string());
    if (leaseId.empty()) throw std::runtime_error("租约响应缺少 FileUploadLeaseId");

    json param = leaseData.value("Param", json::object());
    std::string putUrl = param.value("Url", std::string());
    if (putUrl.empty()) throw std::runtime_error("租约响应缺少上传 URL");
    json putHeaders = json::object();
    if (param.contains("Headers")) {
        if (param["Headers"].is_object()) putHeaders = param["Headers"];
        else if (param["Headers"].is_string()) {
            json parsed = json::parse(param["Headers"].get<std::string>(), nullptr, false);
            if (parsed.is_object()) putHeaders = std::move(parsed);
        }
    }

    // 2) 按租约把二进制 PUT 到 OSS（不要用 FormData）
    std::string method = param.value("Method", std::string("PUT"));
    if (method != "PUT" && method != "put") {
        // 文档里偶发 POST，仍走自定义 method
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");
        std::string response;
        struct curl_slist* headers = nullptr;
        if (putHeaders.is_object()) {
            for (auto it = putHeaders.begin(); it != putHeaders.end(); ++it) {
                std::string v = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                std::string line = it.key() + ": " + v;
                headers = curl_slist_append(headers, line.c_str());
            }
        }
        curl_easy_setopt(curl, CURLOPT_URL, putUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bytes.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK || httpCode >= 400) {
            throw std::runtime_error("上传文件二进制失败 HTTP " + std::to_string(httpCode));
        }
    } else {
        httpPutBinary(putUrl, putHeaders, bytes);
    }

    // 3) AddFile：临时存储 → 应用数据
    json addReq;
    addReq["LeaseId"]      = leaseId;
    addReq["Parser"]       = "AUTO_SELECT";
    addReq["CategoryId"]   = env.categoryId;
    addReq["CategoryType"] = "UNSTRUCTURED";
    const std::string addUrl = env.apiBase + "/" + env.workspaceId + "/datacenter/file";
    json addResp = httpJson("PUT", addUrl, env.apiKey, addReq.dump());
    if (!aliSuccess(addResp)) {
        throw std::runtime_error(aliError(addResp, "AddFile 失败"));
    }
    const json& addData = addResp.contains("Data") ? addResp["Data"] : addResp;
    const std::string fileId = addData.value("FileId", std::string());
    if (fileId.empty()) throw std::runtime_error("AddFile 响应缺少 FileId");

    // 4) 追加进知识库。解析可能还没完成，失败就隔 2 秒重试几次。
    json submitReq;
    submitReq["IndexId"]     = env.indexId;
    submitReq["SourceType"]  = "DATA_CENTER_FILE";
    submitReq["DocumentIds"] = json::array({ fileId });
    const std::string submitUrl = env.apiBase + "/" + env.workspaceId + "/index/add_documents_to_index";

    json submitResp;
    std::string lastErr;
    for (int i = 0; i < 8; ++i) {
        try {
            submitResp = httpJson("POST", submitUrl, env.apiKey, submitReq.dump());
            if (aliSuccess(submitResp)) break;
            lastErr = aliError(submitResp, "SubmitIndexAddDocumentsJob 失败");
        } catch (const std::exception& e) {
            lastErr = e.what();
        }
        if (i == 7) throw std::runtime_error(lastErr);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    const json& jobData = submitResp.contains("Data") ? submitResp["Data"] : submitResp;
    const std::string jobId = jobData.value("Id", std::string());
    if (jobId.empty()) throw std::runtime_error("提交索引任务成功但缺少 JobId");

    IngestResult r;
    r.filename = safeName;
    r.fileId = fileId;
    r.jobId = jobId;
    return r;
}

BailianKnowledgeClient::JobStatus
BailianKnowledgeClient::queryJob(const std::string& jobId) const
{
    if (jobId.empty()) throw std::runtime_error("jobId 为空");
    const Env env = loadEnv();

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    char* escJob = curl_easy_escape(curl, jobId.c_str(), static_cast<int>(jobId.size()));
    char* escIdx = curl_easy_escape(curl, env.indexId.c_str(), static_cast<int>(env.indexId.size()));
    std::string url = env.apiBase + "/" + env.workspaceId + "/index/job/status?JobId=" +
        (escJob ? escJob : jobId) + "&IndexId=" + (escIdx ? escIdx : env.indexId);
    if (escJob) curl_free(escJob);
    if (escIdx) curl_free(escIdx);
    curl_easy_cleanup(curl);

    json resp = httpJson("GET", url, env.apiKey, "");
    JobStatus st;
    st.ok = aliSuccess(resp);
    const json& data = resp.contains("Data") ? resp["Data"] : resp;
    st.status = data.value("Status", std::string());
    st.message = resp.value("Message", std::string());
    if (data.contains("Documents") && data["Documents"].is_array()) {
        st.documents = data["Documents"];
    }
    if (!st.ok && st.message.empty()) st.message = aliError(resp, "查询任务失败");
    return st;
}
