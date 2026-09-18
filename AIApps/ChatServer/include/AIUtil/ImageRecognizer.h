#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>

// 一次分类的完整结果。
// 原先接口只返回类别名，置信度在 Handler 里硬编码成 0.95 回给前端（带着 todo 注释），
// 等于对用户撒谎——模型再不确定，前端看到的也永远是 95%。
struct ImagePrediction {
    std::string label;        // 类别名（取不到标签时为 "Unknown"）
    float       confidence{}; // softmax 归一化后的真实概率，0~1
    int         classId{-1};  // 类别下标，便于前端做映射或埋点
};

class ImageRecognizer {
public:
    // 模型与标签加载。标签路径缺省时读环境变量 IMAGE_LABEL_PATH，再退到内置默认值。
    explicit ImageRecognizer(const std::string& model_path,
        const std::string& label_path = "");

    // ---- 带置信度的接口（推荐使用）----
    ImagePrediction PredictDetailFromFile(const std::string& image_path);
    ImagePrediction PredictDetailFromBuffer(const std::vector<unsigned char>& image_data);
    ImagePrediction PredictDetailFromMat(const cv::Mat& img);

    // ---- 只要类别名的旧接口，保留兼容，内部转调上面三个 ----
    std::string PredictFromFile(const std::string& image_path);
    std::string PredictFromBuffer(const std::vector<unsigned char>& image_data);
    std::string PredictFromMat(const cv::Mat& img);

private:
    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;

    std::string input_name;
    std::string output_name;
    std::vector<int64_t> input_shape;
    int input_height{}, input_width{};

    std::vector<std::string> labels; // 类别标签表

    void LoadLabels(const std::string& label_path);
};
