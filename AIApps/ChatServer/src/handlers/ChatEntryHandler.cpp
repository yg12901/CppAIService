#include "../include/handlers/ChatEntryHandler.h"



// 入口页面 GET / 和 GET /entry
// 读取 entry.html 静态文件，设置 Content-Type:text/html 直接返回
void ChatEntryHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{

    std::string reqFile;
    reqFile.append("../AIApps/ChatServer/resource/entry.html");
    FileUtil fileOperater(reqFile);
    if (!fileOperater.isValid())
    {
        LOG_WARN << reqFile << " not exist";
        fileOperater.resetDefaultFile(); // 404 NOT FOUND
    }

    std::vector<char> buffer(fileOperater.size());
    fileOperater.readFile(buffer); 
    std::string bufStr = std::string(buffer.data(), buffer.size());

    resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    resp->setCloseConnection(false);
    resp->setContentType("text/html");
    resp->setContentLength(bufStr.size());
    resp->setBody(bufStr);
}
