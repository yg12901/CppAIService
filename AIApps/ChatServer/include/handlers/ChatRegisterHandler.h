#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../../../HttpServer/include/utils/MysqlUtil.h"
#include "../ChatServer.h"


class ChatRegisterHandler : public http::router::RouterHandler
{
public:
    explicit ChatRegisterHandler(ChatServer* server) : server_(server) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;
private:
    int insertUser(const std::string& username, const std::string& password);
    bool isUserExist(const std::string& username);
private:
    ChatServer* server_;
    http::MysqlUtil     mysqlUtil_;
};