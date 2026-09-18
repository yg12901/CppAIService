#include "../include/AIUtil/ImageRecognizer.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

/**
 * ImageRecognizer - 基于 ONNX Runtime 的图像分类器
 * 部署 MobileNetV2 轻量级模型，端到端推理：
 *   预处理（BGR→RGB + resize + 归一化 + ImageNet 标准化 + NCHW）
 *   → 推理调用 → 后处理（softmax + argmax）
 */

namespace {

// mobilenetv2-7 的官方前处理约定：输入为 RGB，先缩放到 [0,1]，
// 再按 ImageNet 训练集的均值方差做标准化。缺了这一步分类结果会明显跑偏。
constexpr float kMean[3] = { 0.485f, 0.456f, 0.406f };
constexpr float kStd[3]  = { 0.229f, 0.224f, 0.225f };

std::string defaultLabelPath() {
    const char* p = std::getenv("IMAGE_LABEL_PATH");
    return (p && *p) ? std::string(p) : std::string("/root/imagenet_classes.txt");
}

} // namespace

ImageRecognizer::ImageRecognizer(const std::string& model_path,
    const std::string& label_path)
    : env(ORT_LOGGING_LEVEL_WARNING, "ImageRecognizer")
{
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

    session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);
    allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();


    input_name = session->GetInputNameAllocated(0, *allocator).get();
    output_name = session->GetOutputNameAllocated(0, *allocator).get();


    input_shape = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    input_height = static_cast<int>(input_shape[2]);
    input_width = static_cast<int>(input_shape[3]);

    // 有些模型把 N/H/W 标成动态维度（-1），此时取不到具体尺寸，退回 224。
    if (input_height <= 0) input_height = 224;
    if (input_width <= 0)  input_width = 224;

    LoadLabels(label_path.empty() ? defaultLabelPath() : label_path);
}

void ImageRecognizer::LoadLabels(const std::string& label_path) {
    std::ifstream infile(label_path);
    if (!infile.is_open()) {
        throw std::runtime_error("Failed to open label file: " + label_path);
    }

    std::string line;
    while (std::getline(infile, line)) {
        // 标签文件常带 Windows 换行，残留的 \r 会被拼进类别名回给前端
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            labels.push_back(line);
        }
    }
    infile.close();

    if (labels.empty()) {
        throw std::runtime_error("No labels loaded from file: " + label_path);
    }
}

ImagePrediction ImageRecognizer::PredictDetailFromFile(const std::string& image_path) {
    cv::Mat img = cv::imread(image_path);
    if (img.empty()) {
        throw std::runtime_error("Failed to load image: " + image_path);
    }
    return PredictDetailFromMat(img);
}

ImagePrediction ImageRecognizer::PredictDetailFromBuffer(const std::vector<unsigned char>& image_data) {
    cv::Mat img = cv::imdecode(image_data, cv::IMREAD_COLOR);
    if (img.empty()) {
        throw std::runtime_error("Failed to decode image from buffer");
    }
    return PredictDetailFromMat(img);
}

ImagePrediction ImageRecognizer::PredictDetailFromMat(const cv::Mat& img_raw) {
    if (img_raw.empty()) {
        throw std::runtime_error("Input image is empty");
    }

    // ---- 预处理 ----
    // 一次 blobFromImage 同时完成：缩放到 [0,1]、resize 到模型输入尺寸、
    // BGR→RGB（swapRB=true，OpenCV 读图是 BGR 而模型要 RGB）、HWC→NCHW。
    cv::Mat blob;
    cv::dnn::blobFromImage(img_raw, blob, 1.0 / 255.0,
        cv::Size(input_width, input_height),
        cv::Scalar(), /*swapRB=*/true, /*crop=*/false);

    // blobFromImage 的 scalefactor 只能是标量，做不了逐通道除以 std，手动补上。
    // blob 内存布局是 NCHW，三个通道平面首尾相接，逐平面处理即可。
    const int plane = input_height * input_width;
    float* blobData = blob.ptr<float>();
    for (int c = 0; c < 3; ++c) {
        float* p = blobData + c * plane;
        for (int i = 0; i < plane; ++i) {
            p[i] = (p[i] - kMean[c]) / kStd[c];
        }
    }

    std::vector<int64_t> dims = { 1, 3, input_height, input_width };
    const size_t input_tensor_size = static_cast<size_t>(3) * plane;

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    // 注意：这里是"借用"blob 的内存而非拷贝，blob 必须活到 Run 结束
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, blobData, input_tensor_size, dims.data(), dims.size());

    // ---- 推理 ----
    const char* input_names[] = { input_name.c_str() };
    const char* output_names[] = { output_name.c_str() };

    auto output_tensors = session->Run(
        Ort::RunOptions{ nullptr },
        input_names, &input_tensor, 1,
        output_names, 1
    );

    float* output_data = output_tensors.front().GetTensorMutableData<float>();

    // ---- 后处理 ----
    // 类别数必须以输出张量的真实长度为准。
    // 原实现用 labels.size() 当类别数：标签文件一旦比模型输出多一行
    //（ImageNet 标签表带不带 background 类就差这一行），就会读到张量外的内存。
    const size_t num_classes =
        output_tensors.front().GetTensorTypeAndShapeInfo().GetElementCount();
    if (num_classes == 0) {
        throw std::runtime_error("Model produced an empty output tensor");
    }

    const size_t pred_class =
        std::max_element(output_data, output_data + num_classes) - output_data;

    // 模型输出的是未归一化的 logits，直接拿来当概率没有意义，要过一次 softmax。
    // 先减最大值再取指数：logit 偏大时 exp 会溢出成 inf，减完最大值后指数上界是 1，
    // 数值稳定且不改变结果（分子分母同乘一个常数）。
    const float maxLogit = output_data[pred_class];
    double sumExp = 0.0;
    for (size_t i = 0; i < num_classes; ++i) {
        sumExp += std::exp(static_cast<double>(output_data[i]) - maxLogit);
    }

    ImagePrediction result;
    result.classId = static_cast<int>(pred_class);
    // 最大项减去自身最大值后 exp 恒为 1，所以分子就是 1
    result.confidence = sumExp > 0.0 ? static_cast<float>(1.0 / sumExp) : 0.0f;
    result.label = (pred_class < labels.size()) ? labels[pred_class] : "Unknown";
    return result;
}

std::string ImageRecognizer::PredictFromFile(const std::string& image_path) {
    return PredictDetailFromFile(image_path).label;
}

std::string ImageRecognizer::PredictFromBuffer(const std::vector<unsigned char>& image_data) {
    return PredictDetailFromBuffer(image_data).label;
}

std::string ImageRecognizer::PredictFromMat(const cv::Mat& img) {
    return PredictDetailFromMat(img).label;
}
