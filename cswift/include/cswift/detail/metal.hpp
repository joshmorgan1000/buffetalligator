#ifndef CSWIFT_METAL_HPP
#define CSWIFT_METAL_HPP

#include <cswift/detail/globals.hpp>

// ============================================================================
// Metal Performance Shaders (MPS)
// ============================================================================

namespace cswift {

#ifdef __APPLE__

extern "C" {
    // MPSGraph
    CSHandle cswift_mps_graph_create();
    void cswift_mps_graph_destroy(CSHandle handle);
    CSHandle cswift_mps_graph_placeholder(CSHandle graph, const int32_t* shape, size_t shapeCount, 
                                          int32_t dataType, const char* name);
    CSHandle cswift_mps_graph_matmul(CSHandle graph, CSHandle a, CSHandle b, 
                                     int32_t transposeA, int32_t transposeB, const char* name);
    CSHandle cswift_mps_graph_relu(CSHandle graph, CSHandle input, const char* name);
    CSHandle cswift_mps_graph_addition(CSHandle graph, CSHandle a, CSHandle b, const char* name);
    CSHandle cswift_mps_graph_get_tensor(CSHandle graph, const char* name);
    void cswift_mps_graph_tensor_destroy(CSHandle handle);
    
    // Device
    CSHandle cswift_mps_device_create_default();
    void cswift_mps_device_destroy(CSHandle handle);
    
    // Operations
    CSHandle cswift_mps_graph_convolution2d(CSHandle graph, CSHandle input, CSHandle weights,
                                           int32_t strideX, int32_t strideY,
                                           int32_t paddingLeft, int32_t paddingRight,
                                           int32_t paddingTop, int32_t paddingBottom,
                                           int32_t dilationX, int32_t dilationY,
                                           int32_t groups, const char* name);
    CSHandle cswift_mps_graph_maxpool2d(CSHandle graph, CSHandle input,
                                        int32_t kernelWidth, int32_t kernelHeight,
                                        int32_t strideX, int32_t strideY,
                                        int32_t paddingLeft, int32_t paddingRight,
                                        int32_t paddingTop, int32_t paddingBottom,
                                        const char* name);
    CSHandle cswift_mps_graph_avgpool2d(CSHandle graph, CSHandle input,
                                        int32_t kernelWidth, int32_t kernelHeight,
                                        int32_t strideX, int32_t strideY,
                                        int32_t paddingLeft, int32_t paddingRight,
                                        int32_t paddingTop, int32_t paddingBottom,
                                        const char* name);
    CSHandle cswift_mps_graph_sigmoid(CSHandle graph, CSHandle input, const char* name);
    CSHandle cswift_mps_graph_tanh(CSHandle graph, CSHandle input, const char* name);
    CSHandle cswift_mps_graph_leaky_relu(CSHandle graph, CSHandle input, float alpha, const char* name);
    CSHandle cswift_mps_graph_softmax(CSHandle graph, CSHandle input, int32_t axis, const char* name);
    CSHandle cswift_mps_graph_batch_norm(CSHandle graph, CSHandle input,
                                        CSHandle mean, CSHandle variance,
                                        CSHandle gamma, CSHandle beta,
                                        float epsilon, const char* name);
    CSHandle cswift_mps_graph_reshape(CSHandle graph, CSHandle input,
                                     const int32_t* shape, size_t shapeCount,
                                     const char* name);
    CSHandle cswift_mps_graph_dropout(CSHandle graph, CSHandle input,
                                     float rate, int32_t training,
                                     const char* name);
    
    // Gradients
    CSHandle cswift_mps_graph_gradients(CSHandle graph, CSHandle loss,
                                       const CSHandle* sourceTensors, size_t sourceTensorCount);
    CSHandle cswift_mps_gradients_get(CSHandle gradients, int32_t index);
    int32_t cswift_mps_gradients_count(CSHandle gradients);
    void cswift_mps_gradients_destroy(CSHandle gradients);
    
    // Loss functions
    CSHandle cswift_mps_graph_cross_entropy_loss(CSHandle graph, CSHandle predictions,
                                                 CSHandle labels, int32_t axis,
                                                 int32_t reduction, const char* name);
    CSHandle cswift_mps_graph_mse_loss(CSHandle graph, CSHandle predictions,
                                       CSHandle targets, int32_t reduction,
                                       const char* name);
    
    // Optimizer updates
    CSHandle cswift_mps_graph_sgd_update(CSHandle graph, CSHandle weights,
                                         CSHandle gradients, float learningRate,
                                         float momentum, const char* name);
    CSHandle cswift_mps_graph_adam_update(CSHandle graph, CSHandle weights,
                                          CSHandle gradients, CSHandle momentum,
                                          CSHandle velocity, float learningRate,
                                          float beta1, float beta2, float epsilon,
                                          int32_t timestep, const char* name);
}

// Forward declarations
class CSMPSGraphTensor;
class CSMPSDevice;

/**
 * @brief Data types supported by MPS
 */
enum class CSMPSDataType {
    Float32 = 0,
    Float16 = 1,
    Int32 = 2,
    Int64 = 3,
    Bool = 4
};

/**
 * @brief Loss reduction types
 */
enum class CSMPSLossReduction {
    None = 0,
    Sum = 1,
    Mean = 2
};

/**
 * @brief Metal Performance Shaders computation graph
 */
class CSMPSGraph {
private:
    CSHandle handle;
    std::unordered_map<std::string, std::shared_ptr<CSMPSGraphTensor>> tensors;
    
public:
    CSMPSGraph() : handle(cswift_mps_graph_create()) {
        if (!handle) {
            throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create MPSGraph");
        }
    }
    
    ~CSMPSGraph() {
        if (handle) {
            cswift_mps_graph_destroy(handle);
        }
    }
    
    CSMPSGraph(CSMPSGraph&& other) noexcept 
        : handle(other.handle), tensors(std::move(other.tensors)) {
        other.handle = nullptr;
    }
    
    CSMPSGraph& operator=(CSMPSGraph&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_mps_graph_destroy(handle);
            }
            handle = other.handle;
            tensors = std::move(other.tensors);
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMPSGraph(const CSMPSGraph&) = delete;
    CSMPSGraph& operator=(const CSMPSGraph&) = delete;
    
    std::shared_ptr<CSMPSGraphTensor> placeholder(const std::vector<int32_t>& shape,
                                                  CSMPSDataType dataType = CSMPSDataType::Float32,
                                                  const std::string& name = "");
    
    std::shared_ptr<CSMPSGraphTensor> matmul(const CSMPSGraphTensor& a,
                                             const CSMPSGraphTensor& b,
                                             bool transposeA = false,
                                             bool transposeB = false,
                                             const std::string& name = "");
    
    std::shared_ptr<CSMPSGraphTensor> relu(const CSMPSGraphTensor& input,
                                           const std::string& name = "");
    
    std::shared_ptr<CSMPSGraphTensor> add(const CSMPSGraphTensor& a,
                                          const CSMPSGraphTensor& b,
                                          const std::string& name = "");
    
    std::shared_ptr<CSMPSGraphTensor> getTensor(const std::string& name) const;
    CSHandle getHandle() const;
};

/**
 * @brief Tensor in an MPS computation graph
 */
class CSMPSGraphTensor {
private:
    CSHandle handle;
    std::vector<int32_t> shape_;
    CSMPSDataType dataType_;
    std::string name_;
    bool owned;
    
    friend class CSMPSGraph;
    
public:
    CSMPSGraphTensor(CSHandle handle, const std::vector<int32_t>& shape,
                     CSMPSDataType dataType, const std::string& name, bool owned = true)
        : handle(handle), shape_(shape), dataType_(dataType), name_(name), owned(owned) {}
    
    ~CSMPSGraphTensor() {
        if (handle && owned) {
            cswift_mps_graph_tensor_destroy(handle);
        }
    }
    
    CSMPSGraphTensor(CSMPSGraphTensor&& other) noexcept 
        : handle(other.handle), shape_(std::move(other.shape_)), 
          dataType_(other.dataType_), name_(std::move(other.name_)), owned(other.owned) {
        other.handle = nullptr;
        other.owned = false;
    }
    
    CSMPSGraphTensor& operator=(CSMPSGraphTensor&& other) noexcept {
        if (this != &other) {
            if (handle && owned) {
                cswift_mps_graph_tensor_destroy(handle);
            }
            handle = other.handle;
            shape_ = std::move(other.shape_);
            dataType_ = other.dataType_;
            name_ = std::move(other.name_);
            owned = other.owned;
            other.handle = nullptr;
            other.owned = false;
        }
        return *this;
    }
    
    CSMPSGraphTensor(const CSMPSGraphTensor&) = delete;
    CSMPSGraphTensor& operator=(const CSMPSGraphTensor&) = delete;
    
    const std::vector<int32_t>& shape() const { return shape_; }
    CSMPSDataType dataType() const { return dataType_; }
    const std::string& name() const { return name_; }
    
    size_t numElements() const {
        size_t total = 1;
        for (auto dim : shape_) {
            total *= dim;
        }
        return total;
    }
    
    CSHandle getHandle() const { return handle; }
};

/**
 * @brief Metal device for MPS operations
 */
class CSMPSDevice {
private:
    CSHandle handle;
    
public:
    CSMPSDevice() : handle(cswift_mps_device_create_default()) {
        if (!handle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "No Metal device available");
        }
    }
    
    ~CSMPSDevice() {
        if (handle) {
            cswift_mps_device_destroy(handle);
        }
    }
    
    CSMPSDevice(CSMPSDevice&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSMPSDevice& operator=(CSMPSDevice&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_mps_device_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMPSDevice(const CSMPSDevice&) = delete;
    CSMPSDevice& operator=(const CSMPSDevice&) = delete;
    
    static bool isAvailable() {
        CSHandle device = cswift_mps_device_create_default();
        if (device) {
            cswift_mps_device_destroy(device);
            return true;
        }
        return false;
    }
    
    CSHandle getHandle() const { return handle; }
};

/**
 * @brief Convolution 2D descriptor
 */
struct Conv2DDescriptor {
    std::array<int32_t, 2> stride = {1, 1};
    std::array<int32_t, 4> padding = {0, 0, 0, 0}; // left, right, top, bottom
    std::array<int32_t, 2> dilation = {1, 1};
    int32_t groups = 1;
    
    Conv2DDescriptor() = default;
    
    Conv2DDescriptor& withStride(int32_t sx, int32_t sy) {
        stride = {sx, sy};
        return *this;
    }
    
    Conv2DDescriptor& withPadding(int32_t left, int32_t right, int32_t top, int32_t bottom) {
        padding = {left, right, top, bottom};
        return *this;
    }
    
    Conv2DDescriptor& withDilation(int32_t dx, int32_t dy) {
        dilation = {dx, dy};
        return *this;
    }
    
    Conv2DDescriptor& withGroups(int32_t g) {
        groups = g;
        return *this;
    }
};

/**
 * @brief Pooling 2D descriptor
 */
struct Pool2DDescriptor {
    std::array<int32_t, 2> kernel = {2, 2};
    std::array<int32_t, 2> stride = {2, 2};
    std::array<int32_t, 4> padding = {0, 0, 0, 0}; // left, right, top, bottom
    
    Pool2DDescriptor() = default;
    
    Pool2DDescriptor& withKernel(int32_t width, int32_t height) {
        kernel = {width, height};
        return *this;
    }
    
    Pool2DDescriptor& withStride(int32_t sx, int32_t sy) {
        stride = {sx, sy};
        return *this;
    }
    
    Pool2DDescriptor& withPadding(int32_t left, int32_t right, int32_t top, int32_t bottom) {
        padding = {left, right, top, bottom};
        return *this;
    }
};

/**
 * @brief Extension to CSMPSGraph for layer operations
 */
class CSMPSGraphOperations {
public:
    static std::shared_ptr<CSMPSGraphTensor> conv2d(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const CSMPSGraphTensor& weights,
        const Conv2DDescriptor& descriptor,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> maxPool2d(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const Pool2DDescriptor& descriptor,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> avgPool2d(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const Pool2DDescriptor& descriptor,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> sigmoid(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> tanh(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> leakyRelu(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        float alpha = 0.01f,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> softmax(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        int32_t axis = -1,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> batchNorm(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const CSMPSGraphTensor& mean,
        const CSMPSGraphTensor& variance,
        const CSMPSGraphTensor* gamma = nullptr,
        const CSMPSGraphTensor* beta = nullptr,
        float epsilon = 1e-5f,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> reshape(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        const std::vector<int32_t>& shape,
        const std::string& name = "");
    
    static std::shared_ptr<CSMPSGraphTensor> dropout(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& input,
        float rate = 0.5f,
        bool training = true,
        const std::string& name = "");
};

/**
 * @brief Gradient array result from automatic differentiation
 */
class CSMPSGradients {
private:
    CSHandle handle;
    size_t count_;
    
public:
    CSMPSGradients(CSHandle handle, size_t count) 
        : handle(handle), count_(count) {}
    
    ~CSMPSGradients() {
        if (handle) {
            cswift_mps_gradients_destroy(handle);
        }
    }
    
    CSMPSGradients(CSMPSGradients&& other) noexcept 
        : handle(other.handle), count_(other.count_) {
        other.handle = nullptr;
        other.count_ = 0;
    }
    
    CSMPSGradients& operator=(CSMPSGradients&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_mps_gradients_destroy(handle);
            }
            handle = other.handle;
            count_ = other.count_;
            other.handle = nullptr;
            other.count_ = 0;
        }
        return *this;
    }
    
    CSMPSGradients(const CSMPSGradients&) = delete;
    CSMPSGradients& operator=(const CSMPSGradients&) = delete;
    
    std::shared_ptr<CSMPSGraphTensor> operator[](size_t index) const {
        if (index >= count_) {
            throw CSException(CS_ERROR_INVALID_ARGUMENT, "Gradient index out of bounds");
        }
        
        CSHandle tensorHandle = cswift_mps_gradients_get(handle, static_cast<int32_t>(index));
        if (!tensorHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to get gradient");
        }
        
        return std::make_shared<CSMPSGraphTensor>(
            tensorHandle, std::vector<int32_t>{}, CSMPSDataType::Float32, "", false
        );
    }
    
    size_t size() const { return count_; }
};

/**
 * @brief Gradient operations for automatic differentiation
 */
class CSMPSGradientOps {
public:
    static CSMPSGradients computeGradients(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& loss,
        const std::vector<std::shared_ptr<CSMPSGraphTensor>>& sources) {
        
        std::vector<CSHandle> sourceHandles;
        for (const auto& source : sources) {
            sourceHandles.push_back(source->getHandle());
        }
        
        CSHandle gradientsHandle = cswift_mps_graph_gradients(
            graph.getHandle(),
            loss.getHandle(),
            sourceHandles.data(),
            sourceHandles.size()
        );
        
        if (!gradientsHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to compute gradients");
        }
        
        return CSMPSGradients(gradientsHandle, sources.size());
    }
    
    static std::shared_ptr<CSMPSGraphTensor> crossEntropyLoss(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& predictions,
        const CSMPSGraphTensor& labels,
        int32_t axis = -1,
        CSMPSLossReduction reduction = CSMPSLossReduction::Mean,
        const std::string& name = "") {
        
        CSHandle tensorHandle = cswift_mps_graph_cross_entropy_loss(
            graph.getHandle(),
            predictions.getHandle(),
            labels.getHandle(),
            axis,
            static_cast<int32_t>(reduction),
            name.empty() ? nullptr : name.c_str()
        );
        
        if (!tensorHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create cross-entropy loss");
        }
        
        std::vector<int32_t> outputShape = (reduction == CSMPSLossReduction::None) 
            ? predictions.shape() : std::vector<int32_t>{1};
        
        return std::make_shared<CSMPSGraphTensor>(
            tensorHandle, outputShape, predictions.dataType(), name
        );
    }
    
    static std::shared_ptr<CSMPSGraphTensor> mseLoss(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& predictions,
        const CSMPSGraphTensor& targets,
        CSMPSLossReduction reduction = CSMPSLossReduction::Mean,
        const std::string& name = "") {
        
        CSHandle tensorHandle = cswift_mps_graph_mse_loss(
            graph.getHandle(),
            predictions.getHandle(),
            targets.getHandle(),
            static_cast<int32_t>(reduction),
            name.empty() ? nullptr : name.c_str()
        );
        
        if (!tensorHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create MSE loss");
        }
        
        std::vector<int32_t> outputShape = (reduction == CSMPSLossReduction::None) 
            ? predictions.shape() : std::vector<int32_t>{1};
        
        return std::make_shared<CSMPSGraphTensor>(
            tensorHandle, outputShape, predictions.dataType(), name
        );
    }
};

/**
 * @brief Optimizer operations
 */
class CSMPSOptimizer {
public:
    static std::shared_ptr<CSMPSGraphTensor> sgdUpdate(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& weights,
        const CSMPSGraphTensor& gradients,
        float learningRate,
        float momentum = 0.0f,
        const std::string& name = "") {
        
        CSHandle tensorHandle = cswift_mps_graph_sgd_update(
            graph.getHandle(),
            weights.getHandle(),
            gradients.getHandle(),
            learningRate,
            momentum,
            name.empty() ? nullptr : name.c_str()
        );
        
        if (!tensorHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create SGD update");
        }
        
        return std::make_shared<CSMPSGraphTensor>(
            tensorHandle, weights.shape(), weights.dataType(), name
        );
    }
    
    static std::shared_ptr<CSMPSGraphTensor> adamUpdate(
        CSMPSGraph& graph,
        const CSMPSGraphTensor& weights,
        const CSMPSGraphTensor& gradients,
        const CSMPSGraphTensor& momentum,
        const CSMPSGraphTensor& velocity,
        float learningRate,
        float beta1 = 0.9f,
        float beta2 = 0.999f,
        float epsilon = 1e-8f,
        int32_t timestep = 1,
        const std::string& name = "") {
        
        CSHandle tensorHandle = cswift_mps_graph_adam_update(
            graph.getHandle(),
            weights.getHandle(),
            gradients.getHandle(),
            momentum.getHandle(),
            velocity.getHandle(),
            learningRate,
            beta1,
            beta2,
            epsilon,
            timestep,
            name.empty() ? nullptr : name.c_str()
        );
        
        if (!tensorHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create Adam update");
        }
        
        return std::make_shared<CSMPSGraphTensor>(
            tensorHandle, weights.shape(), weights.dataType(), name
        );
    }
};

/**
 * @brief Helper class for building training loops
 */
class CSMPSTrainer {
private:
    CSMPSGraph& graph;
    float learningRate;
    std::string optimizerType;
    
    // Adam specific state
    float beta1 = 0.9f;
    float beta2 = 0.999f;
    float epsilon = 1e-8f;
    int32_t timestep = 0;
    std::unordered_map<std::string, std::shared_ptr<CSMPSGraphTensor>> momentumStates;
    std::unordered_map<std::string, std::shared_ptr<CSMPSGraphTensor>> velocityStates;
    
public:
    CSMPSTrainer(CSMPSGraph& graph, float learningRate, 
                 const std::string& optimizer = "adam")
        : graph(graph), learningRate(learningRate), optimizerType(optimizer) {}
    
    std::vector<std::shared_ptr<CSMPSGraphTensor>> step(
        const CSMPSGraphTensor& loss,
        const std::vector<std::shared_ptr<CSMPSGraphTensor>>& parameters);
    
    void setLearningRate(float lr);
    float getLearningRate() const;
};

// Implementation of CSMPSGraph methods moved to metal.cpp


// ============================================================================
// Metal Buffer (MTL)
// ============================================================================

extern "C" {
    CSHandle cswift_mtl_device_create_default();
    void cswift_mtl_device_destroy(CSHandle handle);
    
    CSHandle cswift_mtl_buffer_create_unified(CSHandle device, size_t size, uint32_t options);
    CSHandle cswift_mtl_buffer_create_with_data(CSHandle device, const void* data, 
                                                 size_t size, uint32_t options);
    void* cswift_mtl_buffer_contents(CSHandle buffer);
    size_t cswift_mtl_buffer_size(CSHandle buffer);
    void cswift_mtl_buffer_did_modify_range(CSHandle buffer, size_t location, size_t length);
    void cswift_mtl_buffer_destroy(CSHandle buffer);
    
    CSHandle cswift_mtl_command_queue_create(CSHandle device);
    void cswift_mtl_command_queue_destroy(CSHandle queue);
    
    // Command buffer functions
    CSHandle cswift_mtl_command_buffer_create(CSHandle queue);
    void cswift_mtl_command_buffer_commit(CSHandle commandBuffer);
    void cswift_mtl_command_buffer_wait_until_completed(CSHandle commandBuffer);
    void cswift_mtl_command_buffer_destroy(CSHandle commandBuffer);
    
    // GPU buffer copy function
    int32_t cswift_gpu_copy_buffer(CSHandle managerHandle, const char* fromName, const char* toName, int32_t size);
    
    // Additional Metal device functions used in metal.cpp
    CSHandle cswift_mtl_device_create_buffer(CSHandle device, size_t size, int32_t mode);
    CSHandle cswift_mtl_device_create_buffer_with_data(CSHandle device, const void* data, 
                                                        size_t size, int32_t mode);
    CSHandle cswift_mtl_device_create_command_queue(CSHandle device);
}

/**
 * @brief Metal storage mode options
 */
enum class CSMTLStorageMode : uint32_t {
    Shared = 0,      // CPU and GPU accessible (unified memory)
    Private = 1,     // GPU only
    Managed = 2,     // Requires explicit synchronization
    Memoryless = 3   // iOS only, temporary render targets
};

// Forward declarations
class CSMTLBuffer;
class CSMTLCommandQueue;

/**
 * @brief Metal device for GPU operations
 */
class CSMTLDevice {
private:
    CSHandle handle;
    
public:
    CSMTLDevice() : handle(cswift_mtl_device_create_default()) {
        if (!handle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "No Metal device available");
        }
    }
    
    ~CSMTLDevice() {
        if (handle) {
            cswift_mtl_device_destroy(handle);
        }
    }
    
    CSMTLDevice(CSMTLDevice&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSMTLDevice& operator=(CSMTLDevice&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_mtl_device_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMTLDevice(const CSMTLDevice&) = delete;
    CSMTLDevice& operator=(const CSMTLDevice&) = delete;
    
    std::unique_ptr<CSMTLBuffer> createBuffer(size_t size, 
                                              CSMTLStorageMode mode = CSMTLStorageMode::Shared);
    
    std::unique_ptr<CSMTLBuffer> createBuffer(const void* data, size_t size,
                                              CSMTLStorageMode mode = CSMTLStorageMode::Shared);
    
    std::unique_ptr<CSMTLCommandQueue> createCommandQueue();
    
    static bool isAvailable();
    CSHandle getHandle() const;
};

/**
 * @brief Metal buffer with unified memory support
 */
class CSMTLBuffer {
private:
    CSHandle handle;
    void* contents_;
    size_t size_;
    
    friend class CSMTLDevice;
    
    CSMTLBuffer(CSHandle handle) 
        : handle(handle), 
          contents_(cswift_mtl_buffer_contents(handle)),
          size_(cswift_mtl_buffer_size(handle)) {}
    
public:
    ~CSMTLBuffer() {
        if (handle) {
            cswift_mtl_buffer_destroy(handle);
        }
    }
    
    CSMTLBuffer(CSMTLBuffer&& other) noexcept 
        : handle(other.handle), contents_(other.contents_), size_(other.size_) {
        other.handle = nullptr;
        other.contents_ = nullptr;
        other.size_ = 0;
    }
    
    CSMTLBuffer& operator=(CSMTLBuffer&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_mtl_buffer_destroy(handle);
            }
            handle = other.handle;
            contents_ = other.contents_;
            size_ = other.size_;
            other.handle = nullptr;
            other.contents_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    
    CSMTLBuffer(const CSMTLBuffer&) = delete;
    CSMTLBuffer& operator=(const CSMTLBuffer&) = delete;
    
    void* contents() { return contents_; }
    const void* contents() const { return contents_; }
    
    template<typename T>
    T* contents() { return static_cast<T*>(contents_); }
    
    template<typename T>
    const T* contents() const { return static_cast<const T*>(contents_); }
    
    size_t size() const { return size_; }
    
    template<typename T>
    size_t count() const { return size_ / sizeof(T); }
    
    void didModifyRange(size_t offset, size_t length) {
        cswift_mtl_buffer_did_modify_range(handle, offset, length);
    }
    
    void didModify() {
        didModifyRange(0, size_);
    }
    
    void write(const void* data, size_t size, size_t offset = 0) {
        if (offset + size > size_) {
            throw CSException(CS_ERROR_INVALID_ARGUMENT, "Write exceeds buffer size");
        }
        // ZERO_COPY_ALLOWED - GPU buffer writes require memcpy for unified memory
        std::memcpy(static_cast<uint8_t*>(contents_) + offset, data, size);
        didModifyRange(offset, size);
    }
    
    void read(void* data, size_t size, size_t offset = 0) const {
        if (offset + size > size_) {
            throw CSException(CS_ERROR_INVALID_ARGUMENT, "Read exceeds buffer size");
        }
        // ZERO_COPY_ALLOWED - GPU buffer reads require memcpy from unified memory
        std::memcpy(data, static_cast<const uint8_t*>(contents_) + offset, size);
    }
    
    CSHandle getHandle() const { return handle; }
};

/**
 * @brief Metal command queue for GPU execution
 */
class CSMTLCommandQueue {
private:
    CSHandle handle;
    
    friend class CSMTLDevice;
    
    CSMTLCommandQueue(CSHandle handle) : handle(handle) {}
    
public:
    ~CSMTLCommandQueue() {
        if (handle) {
            cswift_mtl_command_queue_destroy(handle);
        }
    }
    
    CSMTLCommandQueue(CSMTLCommandQueue&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSMTLCommandQueue& operator=(CSMTLCommandQueue&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_mtl_command_queue_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMTLCommandQueue(const CSMTLCommandQueue&) = delete;
    CSMTLCommandQueue& operator=(const CSMTLCommandQueue&) = delete;
    
    CSHandle getHandle() const { return handle; }
};

// Implementation of CSMTLDevice methods moved to metal.cpp

#endif // __APPLE__

// ============================================================================
// CoreML Integration (Apple platforms)
// ============================================================================

#ifdef __APPLE__

/**
 * @brief CoreML compute units preference
 */
enum class CSMLComputeUnits : int32_t {
    All = 0,        ///< Use all available compute units (CPU, GPU, Neural Engine)
    CPUOnly = 1,    ///< Use CPU only
    CPUAndGPU = 2   ///< Use CPU and GPU, but not Neural Engine
};

/**
 * @brief CoreML data types for MLMultiArray
 */
enum class CSMLDataType : int32_t {
    Float32 = 0,    ///< 32-bit floating point
    Float64 = 1,    ///< 64-bit floating point
    Int32 = 2       ///< 32-bit signed integer
};

// Forward declarations
extern "C" {
    CSHandle cswift_ml_model_load(const char* path, int32_t computeUnits, void** error);
    CSHandle cswift_ml_model_load_from_data(const void* data, size_t size, int32_t computeUnits, void** error);
    CSHandle cswift_ml_model_predict(CSHandle modelHandle, CSHandle inputHandle, void** error);
    int32_t cswift_ml_model_input_names(CSHandle modelHandle, char** outNames, int maxNames);
    int32_t cswift_ml_model_output_names(CSHandle modelHandle, char** outNames, int maxNames);
    void cswift_ml_model_destroy(CSHandle modelHandle);
    
    CSHandle cswift_ml_feature_provider_create(void);
    int32_t cswift_ml_feature_provider_set_multiarray(CSHandle providerHandle, const char* name, CSHandle multiArrayHandle);
    CSHandle cswift_ml_feature_provider_get_multiarray(CSHandle providerHandle, const char* name);
    int32_t cswift_ml_feature_provider_count(CSHandle providerHandle);
    void cswift_ml_feature_provider_destroy(CSHandle providerHandle);
    
    CSHandle cswift_ml_multiarray_create(const int32_t* shape, int32_t shapeCount, int32_t dataType, void** error);
    void* cswift_ml_multiarray_data_pointer(CSHandle multiArrayHandle);
    int32_t cswift_ml_multiarray_shape(CSHandle multiArrayHandle, int32_t* outShape, int32_t maxDims);
    size_t cswift_ml_multiarray_count(CSHandle multiArrayHandle);
    void cswift_ml_multiarray_destroy(CSHandle multiArrayHandle);
}

/**
 * @brief CoreML multi-dimensional array for tensor data
 */
class CSMLMultiArray {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Create multi-array with specified shape and data type
     * 
     * @param shape Array of dimensions
     * @param dataType Data type for elements
     */
    CSMLMultiArray(const std::vector<int32_t>& shape, CSMLDataType dataType = CSMLDataType::Float32) {
        void* error = nullptr;
        handle = cswift_ml_multiarray_create(shape.data(), static_cast<int32_t>(shape.size()),
                                           static_cast<int32_t>(dataType), &error);
        if (!handle) {
            throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create MLMultiArray");
        }
    }
    
    ~CSMLMultiArray() {
        if (handle) {
            cswift_ml_multiarray_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSMLMultiArray(CSMLMultiArray&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSMLMultiArray& operator=(CSMLMultiArray&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_ml_multiarray_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMLMultiArray(const CSMLMultiArray&) = delete;
    CSMLMultiArray& operator=(const CSMLMultiArray&) = delete;
    
    /**
     * @brief Get direct pointer to underlying data for zero-copy access
     * 
     * @return Pointer to data buffer
     */
    template<typename T>
    T* data() {
        return static_cast<T*>(cswift_ml_multiarray_data_pointer(handle));
    }
    
    template<typename T>
    const T* data() const {
        return static_cast<const T*>(cswift_ml_multiarray_data_pointer(handle));
    }
    
    /**
     * @brief Get array shape (dimensions)
     * 
     * @return Vector containing dimension sizes
     */
    std::vector<int32_t> shape() const {
        constexpr int32_t maxDims = 8;
        int32_t shapeBuffer[maxDims];
        int32_t numDims = cswift_ml_multiarray_shape(handle, shapeBuffer, maxDims);
        return std::vector<int32_t>(shapeBuffer, shapeBuffer + numDims);
    }
    
    /**
     * @brief Get total number of elements
     * 
     * @return Element count
     */
    size_t count() const {
        return cswift_ml_multiarray_count(handle);
    }
    
    CSHandle getHandle() const { return handle; }
};

/**
 * @brief CoreML feature provider for model input/output
 */
class CSMLFeatureProvider {
private:
    CSHandle handle;
    
public:
    CSMLFeatureProvider() {
        handle = cswift_ml_feature_provider_create();
        if (!handle) {
            throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create MLFeatureProvider");
        }
    }
    
    ~CSMLFeatureProvider() {
        if (handle) {
            cswift_ml_feature_provider_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSMLFeatureProvider(CSMLFeatureProvider&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSMLFeatureProvider& operator=(CSMLFeatureProvider&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_ml_feature_provider_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMLFeatureProvider(const CSMLFeatureProvider&) = delete;
    CSMLFeatureProvider& operator=(const CSMLFeatureProvider&) = delete;
    
    /**
     * @brief Set multi-array feature by name
     * 
     * @param name Feature name
     * @param multiArray Multi-array to set
     */
    void setMultiArray(const std::string& name, const CSMLMultiArray& multiArray) {
        int32_t result = cswift_ml_feature_provider_set_multiarray(handle, name.c_str(), 
                                                                  multiArray.getHandle());
        if (result != CS_SUCCESS) {
            throw CSException(static_cast<CSError>(result), "Failed to set multi-array feature");
        }
    }
    
    /**
     * @brief Get multi-array feature by name
     * 
     * @param name Feature name
     * @return Multi-array (throws if not found)
     */
    std::unique_ptr<CSMLMultiArray> getMultiArray(const std::string& name) const {
        CSHandle arrayHandle = cswift_ml_feature_provider_get_multiarray(handle, name.c_str());
        if (!arrayHandle) {
            throw CSException(CS_ERROR_NOT_FOUND, "Feature not found: " + name);
        }
        
        // Note: This creates a wrapper around existing handle - careful with ownership
        return std::make_unique<CSMLMultiArray>(std::vector<int32_t>{1}, CSMLDataType::Float32);
    }
    
    /**
     * @brief Get number of features
     * 
     * @return Feature count
     */
    int32_t count() const {
        return cswift_ml_feature_provider_count(handle);
    }
    
    CSHandle getHandle() const { return handle; }
    
private:
    friend class CSMLModel;
    
    // Private constructor for internal use
    explicit CSMLFeatureProvider(CSHandle h) : handle(h) {}
};

/**
 * @brief CoreML model for machine learning inference
 */
class CSMLModel {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Load model from file path
     * 
     * @param path Path to .mlmodel or .mlpackage file
     * @param computeUnits Compute units preference
     */
    explicit CSMLModel(const std::string& path, 
                      CSMLComputeUnits computeUnits = CSMLComputeUnits::All) {
        void* error = nullptr;
        handle = cswift_ml_model_load(path.c_str(), static_cast<int32_t>(computeUnits), &error);
        if (!handle) {
            throw CSException(CS_ERROR_INVALID_ARGUMENT, "Failed to load model from: " + path);
        }
    }
    
    /**
     * @brief Load model from memory data
     * 
     * @param data Model data
     * @param size Data size
     * @param computeUnits Compute units preference
     */
    CSMLModel(const void* data, size_t size, 
             CSMLComputeUnits computeUnits = CSMLComputeUnits::All) {
        void* error = nullptr;
        handle = cswift_ml_model_load_from_data(data, size, static_cast<int32_t>(computeUnits), &error);
        if (!handle) {
            throw CSException(CS_ERROR_INVALID_ARGUMENT, "Failed to load model from data");
        }
    }
    
    ~CSMLModel() {
        if (handle) {
            cswift_ml_model_destroy(handle);
        }
    }
    
    // Move-only semantics
    CSMLModel(CSMLModel&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    CSMLModel& operator=(CSMLModel&& other) noexcept {
        if (this != &other) {
            if (handle) {
                cswift_ml_model_destroy(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    
    CSMLModel(const CSMLModel&) = delete;
    CSMLModel& operator=(const CSMLModel&) = delete;
    
    /**
     * @brief Perform prediction
     * 
     * @param input Input feature provider
     * @return Output feature provider
     */
    std::unique_ptr<CSMLFeatureProvider> predict(const CSMLFeatureProvider& input) {
        void* error = nullptr;
        CSHandle outputHandle = cswift_ml_model_predict(handle, input.getHandle(), &error);
        if (!outputHandle) {
            throw CSException(CS_ERROR_OPERATION_FAILED, "Prediction failed");
        }
        
        return std::unique_ptr<CSMLFeatureProvider>(new CSMLFeatureProvider(outputHandle));
    }
    
    /**
     * @brief Get input feature names
     * 
     * @return Vector of input feature names
     */
    std::vector<std::string> inputFeatureNames() const {
        constexpr int maxNames = 32;
        char* nameBuffers[maxNames];
        int32_t count = cswift_ml_model_input_names(handle, nameBuffers, maxNames);
        
        std::vector<std::string> names;
        names.reserve(count);
        
        for (int32_t i = 0; i < count; ++i) {
            names.emplace_back(nameBuffers[i]);
            free(nameBuffers[i]);  // Free the strdup'd string
        }
        
        return names;
    }
    
    /**
     * @brief Get output feature names
     * 
     * @return Vector of output feature names
     */
    std::vector<std::string> outputFeatureNames() const {
        constexpr int maxNames = 32;
        char* nameBuffers[maxNames];
        int32_t count = cswift_ml_model_output_names(handle, nameBuffers, maxNames);
        
        std::vector<std::string> names;
        names.reserve(count);
        
        for (int32_t i = 0; i < count; ++i) {
            names.emplace_back(nameBuffers[i]);
            free(nameBuffers[i]);  // Free the strdup'd string
        }
        
        return names;
    }
    
    CSHandle getHandle() const { return handle; }
};

#endif // __APPLE__

} // namespace cswift

#endif // CSWIFT_METAL_HPP
