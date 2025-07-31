/**
 * @file distributed_training_integration.hpp
 * @brief Integration of cswift with zero-copy gradient updates for distributed training
 * 
 * This demonstrates how to combine the zero-copy gradient pattern with
 * Metal Performance Shaders and SwiftNIO for distributed neural core training
 * across multiple Apple devices via Thunderbolt.
 */

#pragma once

#include "cswift.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <thread>
#include <future>
#include <chrono>
#include <iostream>
#include <atomic>
#include <mutex>

namespace bufferalligator {

/**
 * @brief Zero-copy gradient message for distributed training
 * 
 * @tparam T Gradient data type (typically float)
 */
template<typename T>
class GradientMessage {
private:
    std::unique_ptr<cswift::CSMTLBuffer> buffer_;
    size_t capacity_;
    std::atomic<size_t> writeIndex_{0};
    
public:
    /**
     * @brief Construct gradient message with Metal buffer
     * 
     * @param buffer Metal buffer for zero-copy operations
     * @param capacity Maximum number of gradient elements
     */
    GradientMessage(std::unique_ptr<cswift::CSMTLBuffer> buffer, size_t capacity)
        : buffer_(std::move(buffer)), capacity_(capacity) {}
    
    /**
     * @brief Write gradient value using zero-copy operation
     * 
     * @param gradient Gradient value to write
     */
    void write_gradient(T gradient) {
        size_t index = writeIndex_.fetch_add(1);
        if (index < capacity_) {
            T* data = buffer_->contents<T>();
            data[index] = gradient;
            buffer_->didModifyRange(index * sizeof(T), sizeof(T));
        }
    }
    
    /**
     * @brief Send gradient message to a channel
     * 
     * @param channel Communication channel
     */
    template<typename Channel>
    void send_to(Channel& channel) {
        channel.write(buffer_->contents(), writeIndex_.load() * sizeof(T));
    }
    
    /**
     * @brief Reset message for reuse
     */
    void reset() {
        writeIndex_.store(0);
    }
    
    size_t size() const { return writeIndex_.load(); }
    size_t capacity() const { return capacity_; }
};

/**
 * @brief Zero-copy gradient substrate for distributed training
 * 
 * @tparam T Gradient data type (typically float)
 */
template<typename T>
class GradientSubstrate {
private:
    std::unique_ptr<cswift::CSMTLDevice> device_;
    size_t capacity_;
    mutable std::mutex bufferMutex_;
    std::vector<std::unique_ptr<cswift::CSMTLBuffer>> availableBuffers_;
    
public:
    /**
     * @brief Initialize gradient substrate
     * 
     * @param capacity Number of gradient elements per message
     */
    explicit GradientSubstrate(size_t capacity) : capacity_(capacity) {
        if (cswift::CSMTLDevice::isAvailable()) {
            device_ = std::make_unique<cswift::CSMTLDevice>();
            
            // Pre-allocate some buffers for zero-copy operations
            for (int i = 0; i < 10; ++i) {
                auto buffer = device_->createBuffer(capacity * sizeof(T));
                availableBuffers_.push_back(std::move(buffer));
            }
        }
    }
    
    /**
     * @brief Create a new gradient message
     * 
     * @return Zero-copy gradient message
     */
    GradientMessage<T> create_gradient_message() {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        
        std::unique_ptr<cswift::CSMTLBuffer> buffer;
        if (!availableBuffers_.empty()) {
            buffer = std::move(availableBuffers_.back());
            availableBuffers_.pop_back();
        } else if (device_) {
            buffer = device_->createBuffer(capacity_ * sizeof(T));
        } else {
            throw std::runtime_error("No Metal device available for gradient substrate");
        }
        
        return GradientMessage<T>(std::move(buffer), capacity_);
    }
    
    /**
     * @brief Return buffer to the pool for reuse
     * 
     * @param buffer Buffer to return
     */
    void return_buffer(std::unique_ptr<cswift::CSMTLBuffer> buffer) {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        availableBuffers_.push_back(std::move(buffer));
    }
    
    size_t capacity() const { return capacity_; }
};

/**
 * @brief Communication channel for gradient updates
 * 
 * @tparam T Gradient data type
 */
template<typename T>
class GradientChannel {
private:
    std::unique_ptr<cswift::CSNIOByteBuffer> buffer_;
    std::string deviceId_;
    
public:
    /**
     * @brief Initialize gradient channel
     * 
     * @param deviceId Device identifier
     * @param bufferSize Buffer size in bytes
     */
    GradientChannel(const std::string& deviceId, size_t bufferSize = 64 * 1024)
        : deviceId_(deviceId), buffer_(std::make_unique<cswift::CSNIOByteBuffer>(static_cast<int32_t>(bufferSize))) {}
    
    /**
     * @brief Write data to channel
     * 
     * @param data Data pointer
     * @param size Data size in bytes
     */
    void write(const void* data, size_t size) {
        buffer_->writeBytes(data, size);
    }
    
    /**
     * @brief Read data from channel
     * 
     * @param data Destination buffer
     * @param size Maximum bytes to read
     * @return Number of bytes read
     */
    size_t read(void* data, size_t size) {
        size_t available = std::min(size, static_cast<size_t>(buffer_->readableBytes()));
        if (available > 0) {
            buffer_->readBytes(data, available);
        }
        return available;
    }
    
    const std::string& deviceId() const { return deviceId_; }
    size_t available() const { return buffer_->readableBytes(); }
};

/**
 * @brief Orchestrator for distributed gradient updates
 * 
 * @tparam T Gradient data type (typically float)
 */
template<typename T>
class GradientUpdateOrchestrator {
private:
    std::shared_ptr<GradientSubstrate<T>> substrate_;
    float learningRate_;
    std::vector<std::unique_ptr<GradientChannel<T>>> channels_;
    std::atomic<size_t> epochCounter_{0};
    
public:
    /**
     * @brief Initialize gradient update orchestrator
     * 
     * @param substrate Shared gradient substrate
     */
    explicit GradientUpdateOrchestrator(std::shared_ptr<GradientSubstrate<T>> substrate)
        : substrate_(substrate), learningRate_(0.001f) {}
    
    /**
     * @brief Set learning rate for optimizer
     * 
     * @param learningRate Learning rate value
     */
    void set_learning_rate(float learningRate) {
        learningRate_ = learningRate;
    }
    
    /**
     * @brief Reset epoch counter and prepare for new epoch
     */
    void reset_epoch() {
        epochCounter_.fetch_add(1);
        for (auto& channel : channels_) {
            // Clear any pending data in channels
        }
    }
    
    /**
     * @brief Execute gradient update step
     */
    void update_step() {
        // In a full implementation, this would:
        // 1. Collect gradients from all channels
        // 2. Apply gradient aggregation (average, etc.)
        // 3. Update model parameters
        // 4. Synchronize updated parameters back to devices
        
        // For now, this is a placeholder that simulates the update
        for (auto& channel : channels_) {
            // Process gradients from each channel
            size_t available = channel->available();
            if (available > 0) {
                std::vector<T> gradientData(available / sizeof(T));
                channel->read(gradientData.data(), available);
                // Apply gradients (placeholder)
            }
        }
    }
    
    /**
     * @brief Create a new communication channel
     * 
     * @return Reference to new gradient channel
     */
    GradientChannel<T>& create_channel() {
        std::string channelId = "channel_" + std::to_string(channels_.size());
        auto channel = std::make_unique<GradientChannel<T>>(channelId);
        auto& channelRef = *channel;
        channels_.push_back(std::move(channel));
        return channelRef;
    }
    
    float learning_rate() const { return learningRate_; }
    size_t epoch() const { return epochCounter_.load(); }
    size_t channel_count() const { return channels_.size(); }
};

/**
 * @brief Distributed neural core coordinator
 * 
 * Manages training across multiple Apple devices using Thunderbolt networking
 * and Metal Performance Shaders for neural core acceleration.
 */
class DistributedNeuralCoreCoordinator {
private:
    struct DeviceNode {
        std::string hostname;
        uint16_t port;
        std::unique_ptr<cswift::CSNIOTSConnectionChannel> channel;
        std::unique_ptr<cswift::CSMPSDevice> mpsDevice;
        std::unique_ptr<cswift::CSMTLDevice> mtlDevice;
        bool isActive = false;
        
        DeviceNode(const std::string& host, uint16_t p) 
            : hostname(host), port(p) {}
    };
    
    std::vector<std::unique_ptr<DeviceNode>> devices_;
    std::unique_ptr<cswift::CSNIOEventLoopGroup> eventLoopGroup_;
    std::shared_ptr<GradientSubstrate<float>> gradientSubstrate_;
    std::unique_ptr<GradientUpdateOrchestrator<float>> orchestrator_;
    
    // MPS computation graph shared across devices
    std::unique_ptr<cswift::CSMPSGraph> sharedGraph_;
    
    // Parameter synchronization
    std::unordered_map<std::string, std::shared_ptr<cswift::CSMTLBuffer>> parameterBuffers_;
    std::unordered_map<std::string, std::shared_ptr<cswift::CSMTLBuffer>> gradientBuffers_;
    
    // Training state
    float learningRate_ = 0.001f;
    size_t currentEpoch_ = 0;
    bool isTraining_ = false;
    
public:
    /**
     * @brief Initialize distributed coordinator
     * 
     * @param gradientCapacity Number of gradient messages to support
     * @param eventLoopThreads Number of network threads
     */
    DistributedNeuralCoreCoordinator(size_t gradientCapacity = 10000, 
                                    int32_t eventLoopThreads = 4) 
        : gradientSubstrate_(std::make_shared<GradientSubstrate<float>>(gradientCapacity)),
          orchestrator_(std::make_unique<GradientUpdateOrchestrator<float>>(gradientSubstrate_)),
          eventLoopGroup_(std::make_unique<cswift::CSNIOEventLoopGroup>(eventLoopThreads)) {
        
        // Initialize shared MPS graph
        sharedGraph_ = std::make_unique<cswift::CSMPSGraph>();
    }
    
    /**
     * @brief Add a device to the cluster
     * 
     * @param hostname Device hostname (e.g., "m3-macbook.local")
     * @param port Network port for gradient communication
     * @param enableTLS Whether to use TLS for secure communication
     */
    void addDevice(const std::string& hostname, uint16_t port, bool enableTLS = true) {
        auto device = std::make_unique<DeviceNode>(hostname, port);
        
        try {
            // Create network channel
            device->channel = std::make_unique<cswift::CSNIOTSConnectionChannel>(
                hostname, port, enableTLS
            );
            
            // Initialize Metal devices if available
            if (cswift::CSMPSDevice::isAvailable()) {
                device->mpsDevice = std::make_unique<cswift::CSMPSDevice>();
            }
            
            if (cswift::CSMTLDevice::isAvailable()) {
                device->mtlDevice = std::make_unique<cswift::CSMTLDevice>();
            }
            
            devices_.push_back(std::move(device));
            
        } catch (const cswift::CSException& e) {
            throw std::runtime_error("Failed to add device " + hostname + ": " + e.what());
        }
    }
    
    /**
     * @brief Connect to all devices in the cluster
     */
    void connectCluster() {
        std::vector<std::future<void>> connections;
        
        for (auto& device : devices_) {
            auto connectionFuture = eventLoopGroup_->submit([&device]() {
                device->channel->connect();
                device->isActive = device->channel->isActive();
            });
            connections.push_back(std::move(connectionFuture));
        }
        
        // Wait for all connections
        for (auto& future : connections) {
            future.wait();
        }
        
        // Verify cluster connectivity
        size_t activeDevices = 0;
        for (const auto& device : devices_) {
            if (device->isActive) {
                activeDevices++;
            }
        }
        
        if (activeDevices == 0) {
            throw std::runtime_error("No devices connected to cluster");
        }
        
        std::cout << "Cluster connected: " << activeDevices << " active devices" << std::endl;
    }
    
    /**
     * @brief Build neural network model using MPS
     * 
     * Creates a shared model architecture that will be replicated across devices
     */
    void buildModel(const std::vector<int32_t>& inputShape, 
                   const std::vector<int32_t>& outputShape) {
        
        // Create input placeholder
        auto input = sharedGraph_->placeholder(inputShape, cswift::CSMPSDataType::Float32, "input");
        
        // Build a simple CNN architecture
        auto weights1 = sharedGraph_->placeholder({32, inputShape[2], 3, 3}, 
                                                 cswift::CSMPSDataType::Float32, "conv1_weights");
        auto bias1 = sharedGraph_->placeholder({32}, cswift::CSMPSDataType::Float32, "conv1_bias");
        
        // First conv layer
        auto conv1 = cswift::CSMPSGraphOperations::conv2d(
            *sharedGraph_, *input, *weights1, 
            cswift::Conv2DDescriptor().withStride(1, 1).withPadding(1, 1, 1, 1),
            "conv1"
        );
        auto conv1_bias = sharedGraph_->add(*conv1, *bias1, "conv1_biased");
        auto relu1 = sharedGraph_->relu(*conv1_bias, "relu1");
        
        // Max pooling
        auto pool1 = cswift::CSMPSGraphOperations::maxPool2d(
            *sharedGraph_, *relu1,
            cswift::Pool2DDescriptor().withKernel(2, 2).withStride(2, 2),
            "pool1"
        );
        
        // Second conv layer
        auto weights2 = sharedGraph_->placeholder({64, 32, 3, 3}, 
                                                 cswift::CSMPSDataType::Float32, "conv2_weights");
        auto bias2 = sharedGraph_->placeholder({64}, cswift::CSMPSDataType::Float32, "conv2_bias");
        
        auto conv2 = cswift::CSMPSGraphOperations::conv2d(
            *sharedGraph_, *pool1, *weights2,
            cswift::Conv2DDescriptor().withStride(1, 1).withPadding(1, 1, 1, 1),
            "conv2"
        );
        auto conv2_bias = sharedGraph_->add(*conv2, *bias2, "conv2_biased");
        auto relu2 = sharedGraph_->relu(*conv2_bias, "relu2");
        
        // Global average pooling and output
        auto gap = cswift::CSMPSGraphOperations::avgPool2d(
            *sharedGraph_, *relu2,
            cswift::Pool2DDescriptor().withKernel(relu2->shape()[1], relu2->shape()[2]),
            "global_avg_pool"
        );
        
        auto output_weights = sharedGraph_->placeholder({outputShape[0], 64}, 
                                                       cswift::CSMPSDataType::Float32, "output_weights");
        auto output_bias = sharedGraph_->placeholder(outputShape, 
                                                    cswift::CSMPSDataType::Float32, "output_bias");
        
        auto gap_flat = cswift::CSMPSGraphOperations::reshape(*sharedGraph_, *gap, {-1, 64}, "gap_flat");
        auto output = sharedGraph_->matmul(*gap_flat, *output_weights, false, true, "output");
        auto output_biased = sharedGraph_->add(*output, *output_bias, "output_biased");
        
        // Store parameter references for synchronization
        std::vector<std::string> paramNames = {
            "conv1_weights", "conv1_bias", "conv2_weights", "conv2_bias", 
            "output_weights", "output_bias"
        };
        
        // Initialize parameter buffers on each device
        for (const auto& paramName : paramNames) {
            auto tensor = sharedGraph_->getTensor(paramName);
            if (tensor) {
                size_t bufferSize = tensor->numElements() * sizeof(float);
                
                for (auto& device : devices_) {
                    if (device->mtlDevice && device->isActive) {
                        auto buffer = device->mtlDevice->createBuffer(bufferSize);
                        parameterBuffers_[device->hostname + "_" + paramName] = std::move(buffer);
                        
                        auto gradBuffer = device->mtlDevice->createBuffer(bufferSize);
                        gradientBuffers_[device->hostname + "_" + paramName] = std::move(gradBuffer);
                    }
                }
            }
        }
    }
    
    /**
     * @brief Synchronize parameters across all devices
     * 
     * Implements parameter server pattern for distributed training
     */
    void synchronizeParameters() {
        std::vector<std::future<void>> syncTasks;
        
        for (const auto& [bufferName, buffer] : parameterBuffers_) {
            auto syncTask = eventLoopGroup_->submit([this, &bufferName, &buffer]() {
                // Extract device hostname from buffer name
                size_t pos = bufferName.find('_');
                if (pos != std::string::npos) {
                    std::string hostname = bufferName.substr(0, pos);
                    std::string paramName = bufferName.substr(pos + 1);
                    
                    // Find the corresponding device
                    for (auto& device : devices_) {
                        if (device->hostname == hostname && device->isActive) {
                            // Serialize parameter data
                            cswift::CSNIOByteBuffer sendBuffer(buffer->size());
                            sendBuffer.writeBytes(buffer->contents(), buffer->size());
                            
                            // Send to device
                            try {
                                auto [ptr, available] = sendBuffer.readPointer();
                                device->channel->write(ptr, available);
                            } catch (const cswift::CSException& e) {
                                std::cerr << "Failed to sync to " << hostname << ": " << e.what() << std::endl;
                            }
                            break;
                        }
                    }
                }
            });
            syncTasks.push_back(std::move(syncTask));
        }
        
        // Wait for all synchronization tasks
        for (auto& task : syncTasks) {
            task.wait();
        }
    }
    
    /**
     * @brief Execute distributed training step
     * 
     * Coordinates forward pass, gradient computation, and parameter updates
     * across all devices in the cluster.
     */
    void distributedTrainingStep() {
        if (!isTraining_) return;
        
        std::vector<std::future<void>> trainingTasks;
        
        // Launch training on each device
        for (auto& device : devices_) {
            if (!device->isActive) continue;
            
            auto trainingTask = eventLoopGroup_->submit([this, &device]() {
                try {
                    // Create gradient message bound to our substrate
                    auto gradientMsg = gradientSubstrate_->create_gradient_message();
                    
                    // Execute forward pass (this would be device-specific)
                    // For now, simulate gradient computation
                    float simulatedGradient = 0.01f * (rand() / float(RAND_MAX) - 0.5f);
                    gradientMsg.write_gradient(simulatedGradient);
                    
                    // Send gradient through zero-copy channel
                    auto& channel = orchestrator_->create_channel();
                    gradientMsg.send_to(channel);
                    
                    // Notify device of gradient completion
                    std::string notification = "gradient_ready";
                    device->channel->write(notification);
                    
                } catch (const std::exception& e) {
                    std::cerr << "Training error on " << device->hostname << ": " << e.what() << std::endl;
                }
            });
            
            trainingTasks.push_back(std::move(trainingTask));
        }
        
        // Wait for all devices to complete forward/backward pass
        for (auto& task : trainingTasks) {
            task.wait();
        }
        
        // Apply gradient updates using zero-copy orchestrator
        orchestrator_->update_step();
        
        // Synchronize updated parameters back to devices
        synchronizeParameters();
    }
    
    /**
     * @brief Start distributed training
     * 
     * @param epochs Number of training epochs
     * @param learningRate Learning rate for optimizer
     */
    void startTraining(size_t epochs, float learningRate = 0.001f) {
        learningRate_ = learningRate;
        orchestrator_->set_learning_rate(learningRate);
        isTraining_ = true;
        
        for (size_t epoch = 0; epoch < epochs; ++epoch) {
            currentEpoch_ = epoch;
            
            auto epochStart = std::chrono::high_resolution_clock::now();
            
            // Reset gradients for new epoch
            orchestrator_->reset_epoch();
            
            // Execute training step
            distributedTrainingStep();
            
            auto epochEnd = std::chrono::high_resolution_clock::now();
            auto epochTime = std::chrono::duration_cast<std::chrono::milliseconds>(epochEnd - epochStart);
            
            std::cout << "Epoch " << epoch + 1 << "/" << epochs 
                      << " completed in " << epochTime.count() << "ms" << std::endl;
        }
        
        isTraining_ = false;
    }
    
    /**
     * @brief Get cluster status
     */
    void printClusterStatus() const {
        std::cout << "=== Cluster Status ===" << std::endl;
        std::cout << "Total devices: " << devices_.size() << std::endl;
        
        size_t activeDevices = 0;
        for (const auto& device : devices_) {
            std::cout << "Device: " << device->hostname << ":" << device->port
                      << " - " << (device->isActive ? "ACTIVE" : "INACTIVE");
            
            if (device->mpsDevice) {
                std::cout << " [MPS]";
            }
            if (device->mtlDevice) {
                std::cout << " [Metal]";
            }
            std::cout << std::endl;
            
            if (device->isActive) activeDevices++;
        }
        
        std::cout << "Active devices: " << activeDevices << std::endl;
        std::cout << "Current epoch: " << currentEpoch_ << std::endl;
        std::cout << "Learning rate: " << learningRate_ << std::endl;
        std::cout << "Training: " << (isTraining_ ? "YES" : "NO") << std::endl;
    }
    
    /**
     * @brief Shutdown cluster gracefully
     */
    void shutdown() {
        isTraining_ = false;
        
        // Close all device connections
        for (auto& device : devices_) {
            if (device->channel && device->isActive) {
                try {
                    device->channel->close();
                } catch (const cswift::CSException& e) {
                    std::cerr << "Error closing " << device->hostname << ": " << e.what() << std::endl;
                }
            }
        }
        
        // Shutdown event loop
        eventLoopGroup_->shutdownGracefully();
    }
};

/**
 * @brief Neural core performance monitor
 * 
 * Tracks training metrics across the distributed cluster
 */
class NeuralCorePerformanceMonitor {
private:
    struct DeviceMetrics {
        std::string hostname;
        std::chrono::high_resolution_clock::time_point lastUpdate;
        float throughput = 0.0f;  // samples/second
        float memoryUsage = 0.0f; // GB
        float temperature = 0.0f; // Celsius
        size_t gradientUpdates = 0;
    };
    
    std::unordered_map<std::string, DeviceMetrics> metrics_;
    std::thread monitorThread_;
    std::atomic<bool> monitoring_{false};
    
public:
    void startMonitoring() {
        monitoring_ = true;
        monitorThread_ = std::thread([this]() {
            while (monitoring_) {
                updateMetrics();
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        });
    }
    
    void stopMonitoring() {
        monitoring_ = false;
        if (monitorThread_.joinable()) {
            monitorThread_.join();
        }
    }
    
    void updateMetrics() {
        for (auto& [hostname, metrics] : metrics_) {
            metrics.lastUpdate = std::chrono::high_resolution_clock::now();
            // In a real implementation, this would query actual device metrics
            // For now, simulate some values
            metrics.throughput = 1000.0f + (rand() % 500);
            metrics.memoryUsage = 8.0f + (rand() % 4);
            metrics.temperature = 45.0f + (rand() % 20);
        }
    }
    
    void printMetrics() const {
        std::cout << "=== Neural Core Performance ===" << std::endl;
        for (const auto& [hostname, metrics] : metrics_) {
            std::cout << hostname << ":" << std::endl;
            std::cout << "  Throughput: " << metrics.throughput << " samples/sec" << std::endl;
            std::cout << "  Memory: " << metrics.memoryUsage << " GB" << std::endl;
            std::cout << "  Temperature: " << metrics.temperature << "°C" << std::endl;
            std::cout << "  Gradient Updates: " << metrics.gradientUpdates << std::endl;
            std::cout << std::endl;
        }
    }
    
    void addDevice(const std::string& hostname) {
        metrics_[hostname] = DeviceMetrics{hostname, std::chrono::high_resolution_clock::now()};
    }
};

} // namespace bufferalligator

// Usage example:
/*
int main() {
    try {
        // Create distributed coordinator
        bufferalligator::DistributedNeuralCoreCoordinator coordinator(10000, 4);
        
        // Add Apple devices to cluster (connected via Thunderbolt)
        coordinator.addDevice("m2-macbook.local", 8080, true);
        coordinator.addDevice("m3-macbook.local", 8080, true);
        coordinator.addDevice("m4-macbook.local", 8080, true);
        
        // Connect cluster
        coordinator.connectCluster();
        
        // Build neural network model
        coordinator.buildModel({1, 28, 28, 1}, {10}); // MNIST-like
        
        // Start performance monitoring
        bufferalligator::NeuralCorePerformanceMonitor monitor;
        monitor.addDevice("m2-macbook.local");
        monitor.addDevice("m3-macbook.local");
        monitor.addDevice("m4-macbook.local");
        monitor.startMonitoring();
        
        // Start distributed training
        coordinator.startTraining(100, 0.001f);
        
        // Print final status
        coordinator.printClusterStatus();
        monitor.printMetrics();
        
        // Cleanup
        monitor.stopMonitoring();
        coordinator.shutdown();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
*/