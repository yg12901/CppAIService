#pragma once
#include <string>
#include <vector>
#include <cstdint>

/**
 * UserAuthDao - 用户功能权限的数据库访问层
 * 权限为功能级布尔字段（can_chat / can_image / can_tts），存于 users 表
 * 所有访问均使用参数化查询（预编译），杜绝 SQL 注入
 */
class UserAuthDao {
public:
    // 查用户功能权限；查不到（用户不存在）返回 false，三个权限均置 false（最保守）
    static bool GetUserPermissions(int userId, bool& canChat, bool& canImage, bool& canTts);
};
