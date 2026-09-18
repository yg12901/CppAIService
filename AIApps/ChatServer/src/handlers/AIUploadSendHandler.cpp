#include "../include/handlers/AIUploadSendHandler.h"


// 图像识别接口 POST /upload/send
// base64 解码图片 → 按 userId 查找或创建 ImageRecognizer → PredictFromBuffer 推理
void AIUploadSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {

        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
        if (session->getValue("isLoggedIn") != "true")
        {

            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        int userId = std::stoi(session->getValue("userId"));

        // 功能权限校验：canImage=false 的用户禁止使用图像识别（fail-closed）
        if (session->getValue("canImage") != "true") {
            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "image recognition not permitted for your account";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k403Forbidden,
                "Forbidden", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        // 访问器内部：shared_lock 快路径命中已加载的模型；首次加载时在锁外构造，
        // 不会让一个用户的冷启动把全服的图像请求挡住。模型路径已外置到环境变量。
        std::shared_ptr<ImageRecognizer> ImageRecognizerPtr = server_->getOrCreateRecognizer(userId);

        auto body = req.getBody();
        std::string filename;
        std::string imageBase64;
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("filename")) filename = j["filename"];
            if (j.contains("image")) imageBase64 = j["image"];
        }
        if (imageBase64.empty())
        {
            throw std::runtime_error("No image data provided");
        }

        std::string decodedData = base64_decode(imageBase64);
        std::vector<uchar> imgData(decodedData.begin(), decodedData.end());

        ImagePrediction prediction = ImageRecognizerPtr->PredictDetailFromBuffer(imgData);


        json successResp;
        successResp["success"] = "ok";
        successResp["filename"] = filename;
        successResp["class_name"] = prediction.label;
        // 真实的 softmax 概率，不再是写死的 0.95
        successResp["confidence"] = prediction.confidence;
        successResp["class_id"] = prediction.classId;


        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
        return;

    }
    catch (const std::exception& e)
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}



