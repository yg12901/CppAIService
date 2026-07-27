#pragma once
#include <chrono>
#include <random>
#include <cstdlib>
#include <ctime>
#include <string>


class AISessionIdGenerator {
public:
    AISessionIdGenerator() {
        
        std::srand(static_cast<unsigned>(std::time(nullptr)));
    }
    
    std::string generate();
};
