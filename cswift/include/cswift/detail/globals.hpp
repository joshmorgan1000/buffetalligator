#ifndef CSWIFT_CORE_GLOBALS_HPP
#define CSWIFT_CORE_GLOBALS_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <array>
#include <numeric>
#include <algorithm>
#include <future>
#include <thread>
#include <chrono>

// C++20 span support
#if __cplusplus >= 202002L
#include <span>
#endif

/**
 * @brief Opaque handle type for Swift objects
 */
typedef void* CSHandle;

/**
 * @brief Error codes
 */
typedef enum {
    CS_SUCCESS = 0,
    CS_ERROR_OUT_OF_MEMORY = -1,
    CS_ERROR_INVALID_ARGUMENT = -2,
    CS_ERROR_BUFFER_OVERFLOW = -3,
    CS_ERROR_OPERATION_FAILED = -4,
    CS_ERROR_NOT_IMPLEMENTED = -5,
    CS_ERROR_CONNECTION_FAILED = -6,
    CS_ERROR_WRITE_FAILED = -7,
    CS_ERROR_READ_FAILED = -8,
    CS_ERROR_TIMEOUT = -9,
    CS_ERROR_CANCELLED = -10,
    CS_ERROR_NOT_FOUND = -11,
    CS_ERROR_INDEX_OUT_OF_BOUNDS = -12,
    CS_ERROR_PROCESSING_FAILED = -13,
    CS_ERROR_NULL_POINTER = -14,
    CS_ERROR_NOT_SUPPORTED = -15
} CSError;

/**
 * @brief Error information structure
 */
typedef struct {
    CSError code;
    char message[256];
} CSErrorInfo;

namespace cswift {

/**
 * @brief Exception class for cswift errors
 */
class CSException : public std::runtime_error {
private:
    CSError code_;
    
public:
    CSException(CSError code, const std::string& message) 
        : std::runtime_error(message), code_(code) {}
    
    CSError code() const noexcept { return code_; }
};

} // namespace cswift

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Neural Engine C bridge functions
 */
CSHandle cswift_neural_engine_model_create(const char* modelPath, int32_t optimization, int32_t precision, int32_t maxBatchSize, void** error);
void cswift_neural_engine_model_destroy(CSHandle modelHandle);
int32_t cswift_neural_engine_infer(CSHandle modelHandle, const float* inputData, const int32_t* inputShape, int32_t inputDims, const char* inputName, float* outputData, size_t outputSize);
int32_t cswift_neural_engine_batch_infer(CSHandle modelHandle, const float* batchInputData, const int32_t* inputShape, int32_t inputDims, int32_t batchSize, const char* inputName, float* batchOutputData, size_t outputSizePerSample);
void cswift_neural_engine_get_metrics(CSHandle modelHandle, float* inferencesPerSecond, float* averageLatencyMs, float* neuralEngineUtilization);
int32_t cswift_neural_engine_compile_model(const char* inputPath, const char* outputPath, int32_t optimization, int32_t precision);

#ifdef __cplusplus
}
#endif

#endif // CSWIFT_CORE_GLOBALS_HPP