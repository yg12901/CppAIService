#pragma once
#include <iostream>
#include <string>
#include <curl/curl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <chrono>


#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include"base64.h"



class AISpeechProcessor {
public:
    AISpeechProcessor(const std::string& clientId,
                      const std::string& clientSecret,
                      const std::string& cuid = "RZjSQGzNaA8EFWf6rvuHEKDh9i4XJIV9") //用户唯一标识，需要更改成自身标识
        : client_id_(clientId), client_secret_(clientSecret), cuid_(cuid) 
    {
        token_ = getAccessToken();
    }

    // 语音识别
    std::string recognize(const std::string& speechData,const std::string& format = "pcm",int rate = 16000,int channel = 1);

    // 语音合成
    std::string synthesize(const std::string& text,const std::string& format = "mp3-16k",const std::string& lang = "zh",int speed = 5,int pitch = 5,int volume = 5); 


private:
    std::string client_id_;
    std::string client_secret_;
    std::string cuid_;
    std::string token_;

    // 获取 Access Token
    std::string getAccessToken();

};
