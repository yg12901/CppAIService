#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>

class ImageRecognizer {
public:
    // Image model and label loading
    explicit ImageRecognizer(const std::string& model_path,
        const std::string& label_path = "/root/imagenet_classes.txt");

    // Predict from file
    std::string PredictFromFile(const std::string& image_path);

    //PredictFromBuffer
    std::string PredictFromBuffer(const std::vector<unsigned char>& image_data);

    // Predict from OpenCV Mat
    std::string PredictFromMat(const cv::Mat& img);

private:
    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;

    std::string input_name;
    std::string output_name;
    std::vector<int64_t> input_shape;
    int input_height{}, input_width{};

    std::vector<std::string> labels; // ǩ

    void LoadLabels(const std::string& label_path);
};
