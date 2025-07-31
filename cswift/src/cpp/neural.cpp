#include <cswift/detail/neural.hpp>

namespace cswift {

// CSNeuralEngineModel implementations
CSNeuralEngineModel::CSNeuralEngineModel(const std::string& modelPath, 
                                       CSNeuralEngineOptimization optimization,
                                       CSNeuralEnginePrecision precision,
                                       int32_t maxBatchSize) 
    : modelPath_(modelPath)
    , optimization_(optimization)
    , precision_(precision)
    , maxBatchSize_(maxBatchSize) {
    
    void* error = nullptr;
    handle_ = cswift_neural_engine_model_create(
        modelPath_.c_str(),
        static_cast<int32_t>(optimization_),
        static_cast<int32_t>(precision_),
        maxBatchSize_,
        &error
    );
    
    if (!handle_) {
        std::string errorMsg = "Failed to create Neural Engine model";
        if (error) {
            errorMsg += ": " + std::string(static_cast<char*>(error));
            free(error);
        }
        throw CSException(CS_ERROR_OPERATION_FAILED, errorMsg);
    }
}

CSNeuralEngineModel::~CSNeuralEngineModel() {
    if (handle_) {
        cswift_neural_engine_model_destroy(handle_);
    }
}

CSNeuralEngineModel::CSNeuralEngineModel(CSNeuralEngineModel&& other) noexcept 
    : handle_(other.handle_)
    , modelPath_(std::move(other.modelPath_))
    , optimization_(other.optimization_)
    , precision_(other.precision_)
    , maxBatchSize_(other.maxBatchSize_) {
    other.handle_ = nullptr;
}

CSNeuralEngineModel& CSNeuralEngineModel::operator=(CSNeuralEngineModel&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            cswift_neural_engine_model_destroy(handle_);
        }
        handle_ = other.handle_;
        modelPath_ = std::move(other.modelPath_);
        optimization_ = other.optimization_;
        precision_ = other.precision_;
        maxBatchSize_ = other.maxBatchSize_;
        other.handle_ = nullptr;
    }
    return *this;
}

CSError CSNeuralEngineModel::infer(const float* inputData, 
                                 const int32_t* inputShape, 
                                 int32_t inputDims, 
                                 const char* inputName,
                                 float* outputData, 
                                 size_t outputSize) {
    return static_cast<CSError>(cswift_neural_engine_infer(
        handle_,
        inputData,
        inputShape,
        inputDims,
        inputName,
        outputData,
        outputSize
    ));
}

CSError CSNeuralEngineModel::batchInfer(const float* batchInputData, 
                                      const int32_t* inputShape, 
                                      int32_t inputDims, 
                                      int32_t batchSize,
                                      const char* inputName,
                                      float* batchOutputData, 
                                      size_t outputSizePerSample) {
    return static_cast<CSError>(cswift_neural_engine_batch_infer(
        handle_,
        batchInputData,
        inputShape,
        inputDims,
        batchSize,
        inputName,
        batchOutputData,
        outputSizePerSample
    ));
}

CSError CSNeuralEngineModel::compileModelForNeuralEngine(const std::string& inputPath,
                                                        const std::string& outputPath,
                                                        CSNeuralEngineOptimization optimization,
                                                        CSNeuralEnginePrecision precision) {
    return static_cast<CSError>(cswift_neural_engine_compile_model(
        inputPath.c_str(),
        outputPath.c_str(),
        static_cast<int32_t>(optimization),
        static_cast<int32_t>(precision)
    ));
}

const std::string& CSNeuralEngineModel::getModelPath() const { 
    return modelPath_; 
}

CSNeuralEngineOptimization CSNeuralEngineModel::getOptimization() const { 
    return optimization_; 
}

CSNeuralEnginePrecision CSNeuralEngineModel::getPrecision() const { 
    return precision_; 
}

int32_t CSNeuralEngineModel::getMaxBatchSize() const { 
    return maxBatchSize_; 
}

CSNeuralEngineModel::PerformanceMetrics CSNeuralEngineModel::getPerformanceMetrics() const {
    PerformanceMetrics metrics{};
    
    cswift_neural_engine_get_metrics(
        handle_,
        &metrics.inferencesPerSecond,
        &metrics.averageLatencyMs,
        &metrics.neuralEngineUtilization
    );
    
    return metrics;
}

} // namespace cswift