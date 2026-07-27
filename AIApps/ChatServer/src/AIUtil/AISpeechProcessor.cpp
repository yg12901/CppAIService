 #include"../include/AIUtil/AISpeechProcessor.h"

// #include"AISpeechProcessor.h"

 
static size_t onWriteData(void* buffer, size_t size, size_t nmemb, void* userp) {
    std::string* str = static_cast<std::string*>(userp);
    str->append((char*)buffer, size * nmemb);
    return size * nmemb;
}

std::string AISpeechProcessor::getAccessToken() {
    std::string result;
    CURL *curl = curl_easy_init();
    if (!curl) return "";

    curl_easy_setopt(curl, CURLOPT_URL, "https://aip.baidubce.com/oauth/2.0/token");
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::string data = "grant_type=client_credentials&client_id=" + client_id_ + "&client_secret=" + client_secret_;
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

    CURLcode res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    if (res != CURLE_OK) return "";

    // 解析为 nlohmann::json
    try {
        auto j = json::parse(result);
        if (j.contains("access_token") && j["access_token"].is_string()) {
            return j["access_token"].get<std::string>();
        }
    } catch (...) {
        // parse error
    }
    return "";
}


// 语音识别
std::string AISpeechProcessor::recognize(const std::string& speechData,
                                         const std::string& format,
                                         int rate,
                                         int channel) 
{
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string result;

    curl_easy_setopt(curl, CURLOPT_URL, "https://vop.baidu.com/server_api");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "undefined");

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // 构造 JSON
    json body;
    body["format"] = format;
    body["rate"] = rate;
    body["channel"] = channel;
    body["cuid"] = cuid_;
    body["token"] = token_;
    body["len"] = static_cast<int>(speechData.size()); // 原始 PCM/WAV 字节长度
    body["speech"] = speechData;                       // base64 已编码

    // 注意：直接 dump 会对 base64 中的 + / = 等转义
    // 为了安全，可以使用 dump(4, ' ', false, nlohmann::json::error_handler_t::ignore)
    std::string data = body.dump(); 

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

    CURLcode res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    if (res != CURLE_OK) return "";

    // 解析返回 JSON
    try {
        json root = json::parse(result);
        if (root.contains("result") && root["result"].is_array() && !root["result"].empty()) {
            if (root["result"][0].is_string()) {
                return root["result"][0].get<std::string>();
            }
        }
    } catch (...) {
        std::cout << "Parse error in recognize response: " << result << std::endl;
    }

    std::cout << "Recognize failed, response: " << result << std::endl;
    return "";
}



// 语音合成（创建任务 -> 轮询 -> 返回 speech_url）
std::string AISpeechProcessor::synthesize(const std::string& text,
                                          const std::string& format,
                                          const std::string& lang,
                                          int speed,
                                          int pitch,
                                          int volume) 
{
    CURL* curl = nullptr;
    CURLcode res;
    std::string response;

    // ----------- 第一步：创建合成任务 -----------
    curl = curl_easy_init();
    if (!curl) return "";

    std::string create_url = "https://aip.baidubce.com/rpc/2.0/tts/v1/create?access_token=" + token_;

    curl_easy_setopt(curl, CURLOPT_URL, create_url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // 按你示例使用 undefined（和官方 demo 一致）
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "undefined");

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    json body = {
        {"text", text},
        {"format", format},
        {"lang", lang},
        {"speed", speed},
        {"pitch", pitch},
        {"volume", volume},
        {"enable_subtitle", 0}
    };

    std::string data = body.dump();

    response.clear();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        if (headers) curl_slist_free_all(headers);
        return "";
    }

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);

    // 解析 task_id
    std::string task_id;
    try {
        json result_json = json::parse(response);
        // 有些示例中 task_id 在根节点或者在 tasks_info[0] 中都可能出现，优先取根 task_id，再尝试 tasks_info
        if (result_json.contains("task_id") && result_json["task_id"].is_string()) {
            task_id = result_json["task_id"].get<std::string>();
        } else if (result_json.contains("tasks_info") && result_json["tasks_info"].is_array()
                   && !result_json["tasks_info"].empty() && result_json["tasks_info"][0].contains("task_id")) {
            task_id = result_json["tasks_info"][0]["task_id"].get<std::string>();
        }
    } catch (...) {
        return "";
    }

    if (task_id.empty()) return "";

    // ----------- 第二步：轮询查询任务状态 -----------
    std::string speech_url;
    json query;
    query["task_ids"] = json::array({task_id});

    // 轮询，上限可按需调整（例如超时 30 次）
    const int max_loops = 60; // 最多等 60 秒（sleep 1s）
    int loops = 0;
    while (loops++ < max_loops) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        curl = curl_easy_init();
        if (!curl) break;

        std::string query_url = "https://aip.baidubce.com/rpc/2.0/tts/v1/query?access_token=" + token_;
        curl_easy_setopt(curl, CURLOPT_URL, query_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "undefined");

        headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        data = query.dump();
        response.clear();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (headers) curl_slist_free_all(headers);
        if (res != CURLE_OK) break;

        // 解析轮询结果
        try {
            json queryResult = json::parse(response);
            if (queryResult.contains("tasks_info") && queryResult["tasks_info"].is_array()
                && !queryResult["tasks_info"].empty()) {
                json task = queryResult["tasks_info"][0];
                if (task.contains("task_status") && task["task_status"].is_string()) {
                    std::string status = task["task_status"].get<std::string>();
                    if (status == "Success" && task.contains("task_result") && task["task_result"].contains("speech_url")) {
                        speech_url = task["task_result"]["speech_url"].get<std::string>();
                        break;
                    }
                }
            }
        } catch (...) {
            break;
        }
    }

    return speech_url;
}