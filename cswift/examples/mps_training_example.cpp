#include <cswift/cswift.hpp>
#include <iostream>
#include <vector>
#include <random>
#include <iomanip>

using namespace cswift;

/**
 * @brief Example demonstrating neural network training with MPSGraph
 * 
 * @details This example shows how to:
 * - Build a neural network
 * - Compute loss and gradients
 * - Update weights using optimizers
 * - Train on synthetic data
 * 
 * @note This example requires macOS with Metal support
 */

int main() {
    std::cout << "MPSGraph Training Example\n" << std::endl;
    
    try {
        // Check if Metal device is available
        if (!CSMPSDevice::isAvailable()) {
            std::cout << "Metal device not available on this platform" << std::endl;
            std::cout << "MPSGraph requires macOS with Metal support" << std::endl;
            return 0;
        }
        
        // Example 1: Simple linear regression
        std::cout << "=== Example 1: Linear Regression ===\n" << std::endl;
        {
            CSMPSGraph graph;
            CSMPSDevice device;
            
            // Create model: y = wx + b
            auto x = graph.placeholder({-1, 1}, CSMPSDataType::Float32, "x");
            auto y_true = graph.placeholder({-1, 1}, CSMPSDataType::Float32, "y_true");
            
            auto w = graph.placeholder({1, 1}, CSMPSDataType::Float32, "weight");
            auto b = graph.placeholder({1}, CSMPSDataType::Float32, "bias");
            
            // Forward pass
            auto y_pred = graph.matmul(*x, *w);
            y_pred = graph.add(*y_pred, *b, "prediction");
            
            // Compute MSE loss
            auto loss = CSMPSGradientOps::mseLoss(
                graph, *y_pred, *y_true, CSMPSLossReduction::Mean, "mse_loss"
            );
            
            std::cout << "Built linear regression model: y = wx + b" << std::endl;
            std::cout << "Loss function: MSE" << std::endl;
            
            // Compute gradients
            std::vector<std::shared_ptr<CSMPSGraphTensor>> parameters = {w, b};
            auto gradients = CSMPSGradientOps::computeGradients(graph, *loss, parameters);
            
            std::cout << "Computed gradients for weight and bias" << std::endl;
            
            // Update parameters using SGD
            float learningRate = 0.01f;
            auto w_updated = CSMPSOptimizer::sgdUpdate(
                graph, *w, *gradients[0], learningRate, 0.0f, "w_updated"
            );
            auto b_updated = CSMPSOptimizer::sgdUpdate(
                graph, *b, *gradients[1], learningRate, 0.0f, "b_updated"
            );
            
            std::cout << "Updated parameters using SGD with learning rate " << learningRate << std::endl;
        }
        
        // Example 2: Multi-layer neural network with cross-entropy loss
        std::cout << "\n=== Example 2: Classification Network ===\n" << std::endl;
        {
            CSMPSGraph graph;
            
            // Input and labels
            auto input = graph.placeholder({-1, 784}, CSMPSDataType::Float32, "input");
            auto labels = graph.placeholder({-1, 10}, CSMPSDataType::Float32, "labels");
            
            // Layer 1: 784 -> 128
            auto w1 = graph.placeholder({784, 128}, CSMPSDataType::Float32, "w1");
            auto b1 = graph.placeholder({128}, CSMPSDataType::Float32, "b1");
            
            auto h1 = graph.matmul(*input, *w1);
            h1 = graph.add(*h1, *b1);
            h1 = graph.relu(*h1, "hidden1");
            
            // Layer 2: 128 -> 10
            auto w2 = graph.placeholder({128, 10}, CSMPSDataType::Float32, "w2");
            auto b2 = graph.placeholder({10}, CSMPSDataType::Float32, "b2");
            
            auto logits = graph.matmul(*h1, *w2);
            logits = graph.add(*logits, *b2, "logits");
            
            // Softmax + Cross-entropy loss
            auto loss = CSMPSGradientOps::crossEntropyLoss(
                graph, *logits, *labels, -1, CSMPSLossReduction::Mean, "ce_loss"
            );
            
            std::cout << "Built 2-layer classification network:" << std::endl;
            std::cout << "  784 -> 128 (ReLU) -> 10" << std::endl;
            std::cout << "  Loss: Cross-entropy" << std::endl;
            
            // Compute gradients for all parameters
            std::vector<std::shared_ptr<CSMPSGraphTensor>> parameters = {w1, b1, w2, b2};
            auto gradients = CSMPSGradientOps::computeGradients(graph, *loss, parameters);
            
            std::cout << "Computed gradients for all " << parameters.size() << " parameters" << std::endl;
        }
        
        // Example 3: Training with Adam optimizer
        std::cout << "\n=== Example 3: Training with Adam Optimizer ===\n" << std::endl;
        {
            CSMPSGraph graph;
            
            // Simple model for demonstration
            auto x = graph.placeholder({-1, 10}, CSMPSDataType::Float32, "x");
            auto y = graph.placeholder({-1, 5}, CSMPSDataType::Float32, "y");
            
            auto w = graph.placeholder({10, 5}, CSMPSDataType::Float32, "weights");
            auto b = graph.placeholder({5}, CSMPSDataType::Float32, "bias");
            
            // Forward pass
            auto pred = graph.matmul(*x, *w);
            pred = graph.add(*pred, *b, "predictions");
            
            // Loss
            auto loss = CSMPSGradientOps::mseLoss(
                graph, *pred, *y, CSMPSLossReduction::Mean, "loss"
            );
            
            // Initialize trainer
            CSMPSTrainer trainer(graph, 0.001f, "adam");
            
            std::cout << "Created Adam optimizer with learning rate 0.001" << std::endl;
            
            // Simulate training steps
            std::vector<std::shared_ptr<CSMPSGraphTensor>> parameters = {w, b};
            
            std::cout << "\nSimulating training steps:" << std::endl;
            for (int step = 0; step < 5; ++step) {
                // In real training, we would:
                // 1. Load batch data
                // 2. Execute forward pass
                // 3. Compute loss
                // 4. Update parameters
                
                auto updated_params = trainer.step(*loss, parameters);
                
                std::cout << "  Step " << (step + 1) << ": Updated " 
                          << updated_params.size() << " parameters" << std::endl;
                
                // Update parameters for next iteration
                parameters = updated_params;
            }
            
            std::cout << "\nTraining simulation complete!" << std::endl;
        }
        
        // Example 4: Building a convolutional neural network with gradients
        std::cout << "\n=== Example 4: CNN with Gradient Computation ===\n" << std::endl;
        {
            CSMPSGraph graph;
            
            // Input: batch of 28x28 grayscale images
            auto input = graph.placeholder({-1, 28, 28, 1}, CSMPSDataType::Float32, "input");
            auto labels = graph.placeholder({-1, 10}, CSMPSDataType::Float32, "labels");
            
            // Conv layer 1: 1 -> 32 channels
            auto conv1_w = graph.placeholder({5, 5, 1, 32}, CSMPSDataType::Float32, "conv1_weights");
            auto conv1_b = graph.placeholder({32}, CSMPSDataType::Float32, "conv1_bias");
            
            auto conv1 = CSMPSGraphOperations::conv2d(
                graph, *input, *conv1_w,
                Conv2DDescriptor().withStride(1, 1).withPadding(2, 2, 2, 2),
                "conv1"
            );
            conv1 = graph.add(*conv1, *conv1_b);
            conv1 = graph.relu(*conv1, "conv1_relu");
            
            // Max pooling
            auto pool1 = CSMPSGraphOperations::maxPool2d(
                graph, *conv1,
                Pool2DDescriptor().withKernel(2, 2).withStride(2, 2),
                "pool1"
            );
            
            // Conv layer 2: 32 -> 64 channels
            auto conv2_w = graph.placeholder({3, 3, 32, 64}, CSMPSDataType::Float32, "conv2_weights");
            auto conv2_b = graph.placeholder({64}, CSMPSDataType::Float32, "conv2_bias");
            
            auto conv2 = CSMPSGraphOperations::conv2d(
                graph, *pool1, *conv2_w,
                Conv2DDescriptor().withStride(1, 1).withPadding(1, 1, 1, 1),
                "conv2"
            );
            conv2 = graph.add(*conv2, *conv2_b);
            conv2 = graph.relu(*conv2, "conv2_relu");
            
            // Global average pooling (simplified as reshape + dense for this example)
            auto flattened = CSMPSGraphOperations::reshape(
                graph, *conv2, {-1, 64 * 14 * 14}, "flatten"
            );
            
            // Dense layer
            auto fc_w = graph.placeholder({64 * 14 * 14, 10}, CSMPSDataType::Float32, "fc_weights");
            auto fc_b = graph.placeholder({10}, CSMPSDataType::Float32, "fc_bias");
            
            auto logits = graph.matmul(*flattened, *fc_w);
            logits = graph.add(*logits, *fc_b, "logits");
            
            // Loss
            auto loss = CSMPSGradientOps::crossEntropyLoss(
                graph, *logits, *labels, -1, CSMPSLossReduction::Mean, "loss"
            );
            
            std::cout << "Built CNN architecture:" << std::endl;
            std::cout << "  Conv(5x5, 32) -> ReLU -> MaxPool(2x2)" << std::endl;
            std::cout << "  Conv(3x3, 64) -> ReLU" << std::endl;
            std::cout << "  Flatten -> Dense(10)" << std::endl;
            std::cout << "  Loss: Cross-entropy" << std::endl;
            
            // Collect all trainable parameters
            std::vector<std::shared_ptr<CSMPSGraphTensor>> parameters = {
                conv1_w, conv1_b, conv2_w, conv2_b, fc_w, fc_b
            };
            
            std::cout << "\nTotal trainable parameters: " << parameters.size() << std::endl;
            
            // Compute gradients
            auto gradients = CSMPSGradientOps::computeGradients(graph, *loss, parameters);
            std::cout << "Computed gradients for all parameters" << std::endl;
            
            // Initialize trainer
            CSMPSTrainer trainer(graph, 0.001f, "adam");
            std::cout << "Ready for training with Adam optimizer" << std::endl;
        }
        
        std::cout << "\n=== Summary ===" << std::endl;
        std::cout << "This example demonstrated:" << std::endl;
        std::cout << "1. Building neural networks with MPSGraph" << std::endl;
        std::cout << "2. Computing losses (MSE, Cross-entropy)" << std::endl;
        std::cout << "3. Automatic differentiation for gradients" << std::endl;
        std::cout << "4. Parameter updates with SGD and Adam optimizers" << std::endl;
        std::cout << "5. Building CNNs with conv and pooling layers" << std::endl;
        
        std::cout << "\nNote: This example shows graph construction and gradient computation." << std::endl;
        std::cout << "Actual training would require data loading and graph execution." << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "MPS error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}