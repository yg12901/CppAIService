#include "AIUtil/UserAuthDao.h"

// MysqlUtil 是 header-only 模板（http 命名空间）
#include "../../../../HttpServer/include/utils/MysqlUtil.h"

#include <iostream>

namespace {
// RAII 工具：确保 ResultSet 释放，避免连接池连接被占用泄漏
class ResultSetGuard {
public:
    explicit ResultSetGuard(sql::ResultSet* rs) : rs_(rs) {}
    ~ResultSetGuard() { delete rs_; }
    sql::ResultSet* get() const { return rs_; }
private:
    sql::ResultSet* rs_;
};
} // namespace

// 查用户功能权限（can_chat / can_image / can_tts）；查不到时全部置 false 并返回 false
bool UserAuthDao::GetUserPermissions(int userId, bool& canChat, bool& canImage, bool& canTts) {
    canChat = canImage = canTts = false;
    try {
        http::MysqlUtil mysql;
        ResultSetGuard rs(mysql.executeQuery(
            "SELECT can_chat, can_image, can_tts FROM users WHERE id = ?", userId));
        if (rs.get() && rs.get()->next()) {
            canChat  = rs.get()->getInt("can_chat")  != 0;
            canImage = rs.get()->getInt("can_image") != 0;
            canTts   = rs.get()->getInt("can_tts")   != 0;
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "[UserAuthDao] GetUserPermissions failed: " << e.what() << std::endl;
    }
    return false;
}
