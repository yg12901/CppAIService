#include"../include/AIUtil/AISessionIdGenerator.h"



std::string AISessionIdGenerator::generate(){
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    long long randVal = std::rand() % 100000; // 0~99999
    long long rawId = now ^ randVal;
    return std::to_string(rawId);
}
