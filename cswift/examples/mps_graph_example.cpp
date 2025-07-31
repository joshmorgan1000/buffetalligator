#include <cswift/cswift.hpp>
#include <iostream>
#include <vector>

using namespace cswift;

/**
 * @brief Example demonstrating MPSGraph for machine learning operations
 * 
 * @details This example shows how to use MPSGraph to build and execute
 * neural network operations on Apple's GPU hardware.
 * 
 * @note This example requires macOS with Metal support
 */

int main() {
    std::cout << "MPSGraph Example\n" << std::endl;
    
    try {
        // Check if Metal device is available
        if (!CSMPSDevice::isAvailable()) {
            std::cout << "Metal device not available on this platform" << std::endl;
            std::cout << "MPSGraph requires macOS with Metal support" << std::endl;
            return 0;
        }
        
        // Example 1: Basic graph construction
        std::cout << "=== Example 1: Basic Graph Construction ===" << std::endl;
        {
            CSMPSGraph graph;
            CSMPSDevice device;
            
            std::cout << "Created MPSGraph and Metal device" << std::endl;
            
            // Create placeholders
            auto input = graph.placeholder({-1, 784}, CSMPSDataType::Float32, "input");
            auto weights = graph.placeholder({784, 10}, CSMPSDataType::Float32, "weights");
            auto bias = graph.placeholder({10}, CSMPSDataType::Float32, "bias");
            
            std::cout << "Created placeholders:" << std::endl;
            std::cout << "  Input shape: [-1, 784]" << std::endl;
            std::cout << "  Weights shape: [784, 10]" << std::endl;
            std::cout << "  Bias shape: [10]" << std::endl;
            
            // Build a simple linear layer
            auto matmul_output = graph.matmul(*input, *weights, false, false, "matmul");
            auto linear_output = graph.add(*matmul_output, *bias, "linear");
            auto output = graph.relu(*linear_output, "output");
            
            std::cout << "Built computation graph: input -> matmul -> add -> relu -> output" << std::endl;
            
            // Retrieve tensors by name
            auto retrieved_output = graph.getTensor("output");
            if (retrieved_output) {
                std::cout << "Successfully retrieved output tensor by name" << std::endl;
            }
        }
        
        // Example 2: Multi-layer neural network
        std::cout << "\n=== Example 2: Multi-Layer Neural Network ===" << std::endl;
        {
            CSMPSGraph graph;
            
            // Input layer
            auto input = graph.placeholder({-1, 28, 28, 1}, CSMPSDataType::Float32, "input");
            
            // Flatten input (28x28x1 -> 784)
            // In real implementation, we'd have a reshape operation
            auto flat_input = graph.placeholder({-1, 784}, CSMPSDataType::Float32, "flat_input");
            
            // First hidden layer (784 -> 128)
            auto w1 = graph.placeholder({784, 128}, CSMPSDataType::Float32, "w1");
            auto b1 = graph.placeholder({128}, CSMPSDataType::Float32, "b1");
            auto h1 = graph.matmul(*flat_input, *w1);
            h1 = graph.add(*h1, *b1);
            h1 = graph.relu(*h1, "hidden1");
            
            // Second hidden layer (128 -> 64)
            auto w2 = graph.placeholder({128, 64}, CSMPSDataType::Float32, "w2");
            auto b2 = graph.placeholder({64}, CSMPSDataType::Float32, "b2");
            auto h2 = graph.matmul(*h1, *w2);
            h2 = graph.add(*h2, *b2);
            h2 = graph.relu(*h2, "hidden2");
            
            // Output layer (64 -> 10)
            auto w3 = graph.placeholder({64, 10}, CSMPSDataType::Float32, "w3");
            auto b3 = graph.placeholder({10}, CSMPSDataType::Float32, "b3");
            auto output = graph.matmul(*h2, *w3);
            output = graph.add(*output, *b3, "logits");
            
            std::cout << "Built 3-layer neural network:" << std::endl;
            std::cout << "  Input: [-1, 784]" << std::endl;
            std::cout << "  Hidden 1: [-1, 128] (ReLU)" << std::endl;
            std::cout << "  Hidden 2: [-1, 64] (ReLU)" << std::endl;
            std::cout << "  Output: [-1, 10]" << std::endl;
        }
        
        // Example 3: Matrix operations
        std::cout << "\n=== Example 3: Matrix Operations ===" << std::endl;
        {
            CSMPSGraph graph;
            
            // Create matrices for various operations
            auto A = graph.placeholder({3, 4}, CSMPSDataType::Float32, "A");
            auto B = graph.placeholder({4, 5}, CSMPSDataType::Float32, "B");
            auto C = graph.placeholder({5, 3}, CSMPSDataType::Float32, "C");
            
            // Standard matrix multiplication: A @ B
            auto AB = graph.matmul(*A, *B, false, false, "A_times_B");
            std::cout << "A @ B: [3,4] @ [4,5] = [3,5]" << std::endl;
            
            // Matrix multiplication with transpose: A @ B^T
            auto B_transposed = graph.placeholder({5, 4}, CSMPSDataType::Float32, "B_T");
            auto AB_T = graph.matmul(*A, *B_transposed, false, true, "A_times_B_transpose");
            std::cout << "A @ B^T: [3,4] @ [5,4]^T = [3,5]" << std::endl;
            
            // Chain multiplication: (A @ B) @ C
            auto ABC = graph.matmul(*AB, *C, false, false, "ABC");
            std::cout << "(A @ B) @ C: [3,5] @ [5,3] = [3,3]" << std::endl;
        }
        
        // Example 4: Building blocks for common layers
        std::cout << "\n=== Example 4: Common Neural Network Layers ===" << std::endl;
        {
            CSMPSGraph graph;
            
            // Helper lambda for linear layer
            auto linearLayer = [&graph](const CSMPSGraphTensor& input, 
                                       int inputSize, int outputSize, 
                                       const std::string& name) {
                auto w = graph.placeholder({inputSize, outputSize}, 
                                         CSMPSDataType::Float32, name + "_weight");
                auto b = graph.placeholder({outputSize}, 
                                         CSMPSDataType::Float32, name + "_bias");
                
                auto z = graph.matmul(input, *w);
                return graph.add(*z, *b, name + "_output");
            };
            
            // Build a simple feedforward network using the helper
            auto input = graph.placeholder({-1, 100}, CSMPSDataType::Float32, "input");
            
            auto layer1 = linearLayer(*input, 100, 50, "layer1");
            auto act1 = graph.relu(*layer1, "relu1");
            
            auto layer2 = linearLayer(*act1, 50, 25, "layer2");
            auto act2 = graph.relu(*layer2, "relu2");
            
            auto output = linearLayer(*act2, 25, 10, "output_layer");
            
            std::cout << "Built feedforward network using helper function:" << std::endl;
            std::cout << "  100 -> 50 (ReLU) -> 25 (ReLU) -> 10" << std::endl;
        }
        
        std::cout << "\nMPSGraph example completed successfully!" << std::endl;
        std::cout << "\nNote: This example demonstrates graph construction." << std::endl;
        std::cout << "Actual execution would require MPSGraphExecutable and data tensors." << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "MPS error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}