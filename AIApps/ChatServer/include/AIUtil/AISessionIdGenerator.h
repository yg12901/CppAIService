#pragma once
#include <chrono>
#include <random>
#include <cstdlib>
#include <ctime>
#include <string>


// 会话 ID 生成器：系统时间纳秒计数 XOR 随机数，生成唯一 sessionId
class AISessionIdGenerator {
public:
    AISessionIdGenerator() {
        
        std::srand(static_cast<unsigned>(std::time(nullptr)));
    }
    
    std::string generate();
};
