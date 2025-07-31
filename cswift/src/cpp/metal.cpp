/**
 * @file metal.cpp
 * @brief Implementation of Metal GPU integration classes
 * 
 * Implements Metal GPU operations for macOS/iOS
 */

#include <cswift/detail/metal.hpp>
#include <cswift/cswift.hpp>

#ifdef __APPLE__

namespace cswift {

// CSMPSGraphOperations static method implementations

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::conv2d(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const CSMPSGraphTensor& weights,
    const Conv2DDescriptor& descriptor,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_convolution2d(
        graph.getHandle(),
        input.getHandle(),
        weights.getHandle(),
        descriptor.stride[0], descriptor.stride[1],
        descriptor.padding[0], descriptor.padding[1],
        descriptor.padding[2], descriptor.padding[3],
        descriptor.dilation[0], descriptor.dilation[1],
        descriptor.groups,
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create convolution operation");
    }
    
    std::vector<int32_t> outputShape = input.shape();
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, outputShape, input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::maxPool2d(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const Pool2DDescriptor& descriptor,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_maxpool2d(
        graph.getHandle(),
        input.getHandle(),
        descriptor.kernel[0], descriptor.kernel[1],
        descriptor.stride[0], descriptor.stride[1],
        descriptor.padding[0], descriptor.padding[1],
        descriptor.padding[2], descriptor.padding[3],
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create max pooling operation");
    }
    
    std::vector<int32_t> outputShape = input.shape();
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, outputShape, input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::avgPool2d(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const Pool2DDescriptor& descriptor,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_avgpool2d(
        graph.getHandle(),
        input.getHandle(),
        descriptor.kernel[0], descriptor.kernel[1],
        descriptor.stride[0], descriptor.stride[1],
        descriptor.padding[0], descriptor.padding[1],
        descriptor.padding[2], descriptor.padding[3],
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create average pooling operation");
    }
    
    std::vector<int32_t> outputShape = input.shape();
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, outputShape, input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::sigmoid(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_sigmoid(
        graph.getHandle(),
        input.getHandle(),
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create sigmoid operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::tanh(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_tanh(
        graph.getHandle(),
        input.getHandle(),
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create tanh operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::leakyRelu(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    float alpha,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_leaky_relu(
        graph.getHandle(),
        input.getHandle(),
        alpha,
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create leaky ReLU operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::softmax(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    int32_t axis,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_softmax(
        graph.getHandle(),
        input.getHandle(),
        axis,
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create softmax operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::batchNorm(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const CSMPSGraphTensor& mean,
    const CSMPSGraphTensor& variance,
    const CSMPSGraphTensor* gamma,
    const CSMPSGraphTensor* beta,
    float epsilon,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_batch_norm(
        graph.getHandle(),
        input.getHandle(),
        mean.getHandle(),
        variance.getHandle(),
        gamma ? gamma->getHandle() : nullptr,
        beta ? beta->getHandle() : nullptr,
        epsilon,
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create batch normalization operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::reshape(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    const std::vector<int32_t>& shape,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_reshape(
        graph.getHandle(),
        input.getHandle(),
        shape.data(),
        static_cast<int32_t>(shape.size()),
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create reshape operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, shape, input.dataType(), name
    );
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraphOperations::dropout(
    CSMPSGraph& graph,
    const CSMPSGraphTensor& input,
    float rate,
    bool training,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_dropout(
        graph.getHandle(),
        input.getHandle(),
        rate,
        training ? 1 : 0,
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create dropout operation");
    }
    
    return std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
}

// CSMPSGraph method implementations

std::shared_ptr<CSMPSGraphTensor> CSMPSGraph::getTensor(const std::string& name) const {
    auto it = tensors.find(name);
    return (it != tensors.end()) ? it->second : nullptr;
}

CSHandle CSMPSGraph::getHandle() const { 
    return handle; 
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraph::placeholder(
    const std::vector<int32_t>& shape,
    CSMPSDataType dataType,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_placeholder(
        handle,
        shape.data(),
        static_cast<int32_t>(shape.size()),
        static_cast<int32_t>(dataType),
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create placeholder tensor");
    }
    
    auto tensor = std::make_shared<CSMPSGraphTensor>(tensorHandle, shape, dataType, name);
    
    if (!name.empty()) {
        tensors[name] = tensor;
    }
    
    return tensor;
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraph::matmul(
    const CSMPSGraphTensor& a,
    const CSMPSGraphTensor& b,
    bool transposeA,
    bool transposeB,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_matmul(
        handle,
        a.getHandle(),
        b.getHandle(),
        transposeA ? 1 : 0,
        transposeB ? 1 : 0,
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create matrix multiplication operation");
    }
    
    // Calculate output shape
    auto shapeA = a.shape();
    auto shapeB = b.shape();
    std::vector<int32_t> outputShape;
    
    if (shapeA.size() == 2 && shapeB.size() == 2) {
        int32_t m = transposeA ? shapeA[1] : shapeA[0];
        int32_t n = transposeB ? shapeB[0] : shapeB[1];
        outputShape = {m, n};
    } else {
        outputShape = shapeA; // Simplified for batch matmul
    }
    
    auto result = std::make_shared<CSMPSGraphTensor>(tensorHandle, outputShape, a.dataType(), name);
    
    if (!name.empty()) {
        tensors[name] = result;
    }
    
    return result;
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraph::relu(
    const CSMPSGraphTensor& input,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_relu(
        handle,
        input.getHandle(),
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create ReLU operation");
    }
    
    auto result = std::make_shared<CSMPSGraphTensor>(
        tensorHandle, input.shape(), input.dataType(), name
    );
    
    if (!name.empty()) {
        tensors[name] = result;
    }
    
    return result;
}

std::shared_ptr<CSMPSGraphTensor> CSMPSGraph::add(
    const CSMPSGraphTensor& a,
    const CSMPSGraphTensor& b,
    const std::string& name) {
    
    CSHandle tensorHandle = cswift_mps_graph_addition(
        handle,
        a.getHandle(),
        b.getHandle(),
        name.empty() ? nullptr : name.c_str()
    );
    
    if (!tensorHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create addition operation");
    }
    
    auto result = std::make_shared<CSMPSGraphTensor>(
        tensorHandle, a.shape(), a.dataType(), name
    );
    
    if (!name.empty()) {
        tensors[name] = result;
    }
    
    return result;
}

// CSMPSTrainer method implementations

std::vector<std::shared_ptr<CSMPSGraphTensor>> CSMPSTrainer::step(
    const CSMPSGraphTensor& loss,
    const std::vector<std::shared_ptr<CSMPSGraphTensor>>& parameters) {
    
    auto gradients = CSMPSGradientOps::computeGradients(graph, loss, parameters);
    
    std::vector<std::shared_ptr<CSMPSGraphTensor>> updatedParams;
    
    for (size_t i = 0; i < parameters.size(); ++i) {
        auto param = parameters[i];
        auto grad = gradients[i];
        
        if (optimizerType == "sgd") {
            auto updated = CSMPSOptimizer::sgdUpdate(
                graph, *param, *grad, learningRate
            );
            updatedParams.push_back(updated);
        } else if (optimizerType == "adam") {
            timestep++;
            
            std::string paramName = param->name();
            if (momentumStates.find(paramName) == momentumStates.end()) {
                momentumStates[paramName] = graph.placeholder(
                    param->shape(), param->dataType(), paramName + "_momentum"
                );
                velocityStates[paramName] = graph.placeholder(
                    param->shape(), param->dataType(), paramName + "_velocity"
                );
            }
            
            auto updated = CSMPSOptimizer::adamUpdate(
                graph, *param, *grad,
                *momentumStates[paramName],
                *velocityStates[paramName],
                learningRate, beta1, beta2, epsilon, timestep
            );
            updatedParams.push_back(updated);
        }
    }
    
    return updatedParams;
}

void CSMPSTrainer::setLearningRate(float lr) { 
    learningRate = lr; 
}

float CSMPSTrainer::getLearningRate() const { 
    return learningRate; 
}

// CSMPSGraph method implementations already defined above

// CSMTLDevice static method implementations

bool CSMTLDevice::isAvailable() {
    CSHandle device = cswift_mps_device_create_default();
    if (device) {
        cswift_mps_device_destroy(device);
        return true;
    }
    return false;
}

// CSMTLDevice method implementations

CSHandle CSMTLDevice::getHandle() const { 
    return handle; 
}

std::unique_ptr<CSMTLBuffer> CSMTLDevice::createBuffer(size_t size, CSMTLStorageMode mode) {
    CSHandle bufferHandle = cswift_mtl_buffer_create_unified(
        handle, size, static_cast<uint32_t>(mode)
    );
    
    if (!bufferHandle) {
        throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create Metal buffer");
    }
    
    return std::unique_ptr<CSMTLBuffer>(new CSMTLBuffer(bufferHandle));
}

std::unique_ptr<CSMTLBuffer> CSMTLDevice::createBuffer(const void* data, size_t size, 
                                                       CSMTLStorageMode mode) {
    CSHandle bufferHandle = cswift_mtl_buffer_create_with_data(
        handle, data, size, static_cast<uint32_t>(mode)
    );
    
    if (!bufferHandle) {
        throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create Metal buffer with data");
    }
    
    return std::unique_ptr<CSMTLBuffer>(new CSMTLBuffer(bufferHandle));
}

std::unique_ptr<CSMTLCommandQueue> CSMTLDevice::createCommandQueue() {
    CSHandle queueHandle = cswift_mtl_command_queue_create(handle);
    
    if (!queueHandle) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to create command queue");
    }
    
    return std::unique_ptr<CSMTLCommandQueue>(new CSMTLCommandQueue(queueHandle));
}

} // namespace cswift

#endif // __APPLE__