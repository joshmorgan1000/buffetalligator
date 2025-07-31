#include <cswift/cswift.hpp>
#include <iostream>
#include <vector>
#include <string>

using namespace cswift;

/**
 * @brief Example demonstrating ChannelPipeline for data transformation
 * 
 * @details This example shows how to use ChannelPipeline to add
 * compression, decompression, and TLS handlers for data processing.
 */

int main() {
    std::cout << "ChannelPipeline Example\n" << std::endl;
    
    try {
        // Example 1: Basic pipeline with compression
        std::cout << "=== Example 1: Compression Pipeline ===" << std::endl;
        {
            CSNIOChannelPipeline pipeline;
            
            // Add compression handler
            pipeline.addCompression("deflate");
            std::cout << "Added deflate compression to pipeline" << std::endl;
            
            // In a real application, data would flow through the pipeline
            // The compression would be applied automatically
            std::cout << "Pipeline configured for compression" << std::endl;
        }
        
        // Example 2: Pipeline with compression and decompression
        std::cout << "\n=== Example 2: Compression/Decompression Pipeline ===" << std::endl;
        {
            CSNIOChannelPipeline pipeline;
            
            // Add both compression and decompression
            pipeline.addCompression("deflate");
            pipeline.addDecompression("deflate");
            
            std::cout << "Added deflate compression and decompression handlers" << std::endl;
            std::cout << "Data can now be compressed and decompressed" << std::endl;
        }
        
        // Example 3: TLS pipeline
        std::cout << "\n=== Example 3: TLS Pipeline ===" << std::endl;
        {
            CSNIOChannelPipeline pipeline;
            
            // Add TLS handler for secure communication
            std::string hostname = "example.com";
            pipeline.addTLS(hostname, true);  // true = verify hostname
            
            std::cout << "Added TLS handler for " << hostname << std::endl;
            std::cout << "Hostname verification: enabled" << std::endl;
        }
        
        // Example 4: Complete secure pipeline
        std::cout << "\n=== Example 4: Complete Secure Pipeline ===" << std::endl;
        {
            CSNIOChannelPipeline pipeline;
            
            // Add handlers in order
            // 1. TLS for encryption
            pipeline.addTLS("secure.example.com", true);
            
            // 2. Compression after encryption
            pipeline.addCompression("deflate");
            
            std::cout << "Created secure pipeline with:" << std::endl;
            std::cout << "  1. TLS encryption" << std::endl;
            std::cout << "  2. Deflate compression" << std::endl;
        }
        
        // Example 5: Different compression algorithms
        std::cout << "\n=== Example 5: Different Compression Algorithms ===" << std::endl;
        {
            // Pipeline with gzip compression
            CSNIOChannelPipeline gzipPipeline;
            gzipPipeline.addCompression("gzip");
            std::cout << "Created pipeline with gzip compression" << std::endl;
            
            // Pipeline with zlib compression
            CSNIOChannelPipeline zlibPipeline;
            zlibPipeline.addCompression("zlib");
            std::cout << "Created pipeline with zlib compression" << std::endl;
            
            // Pipeline with custom algorithm (if supported)
            try {
                CSNIOChannelPipeline customPipeline;
                customPipeline.addCompression("lz4");
                std::cout << "Created pipeline with lz4 compression" << std::endl;
            } catch (const CSException& e) {
                std::cout << "LZ4 compression not supported: " << e.what() << std::endl;
            }
        }
        
        // Example 6: Handler removal
        std::cout << "\n=== Example 6: Dynamic Handler Management ===" << std::endl;
        {
            CSNIOChannelPipeline pipeline;
            
            // Add multiple handlers
            pipeline.addCompression("deflate");
            pipeline.addTLS("example.com", false);
            
            std::cout << "Added compression and TLS handlers" << std::endl;
            
            // Remove a handler
            try {
                pipeline.removeHandler("deflate");
                std::cout << "Removed deflate handler" << std::endl;
            } catch (const CSException& e) {
                std::cout << "Failed to remove handler: " << e.what() << std::endl;
            }
        }
        
        // Example 7: Multiple pipelines for different connections
        std::cout << "\n=== Example 7: Multiple Pipeline Configurations ===" << std::endl;
        {
            // Pipeline for internal communication (no TLS, just compression)
            CSNIOChannelPipeline internalPipeline;
            internalPipeline.addCompression("deflate");
            internalPipeline.addDecompression("deflate");
            std::cout << "Internal pipeline: compression only" << std::endl;
            
            // Pipeline for external communication (TLS + compression)
            CSNIOChannelPipeline externalPipeline;
            externalPipeline.addTLS("api.example.com", true);
            externalPipeline.addCompression("gzip");
            std::cout << "External pipeline: TLS + compression" << std::endl;
            
            // Pipeline for legacy system (no compression, just TLS)
            CSNIOChannelPipeline legacyPipeline;
            legacyPipeline.addTLS("legacy.example.com", true);
            std::cout << "Legacy pipeline: TLS only" << std::endl;
        }
        
        // Example 8: Error handling
        std::cout << "\n=== Example 8: Error Handling ===" << std::endl;
        {
            CSNIOChannelPipeline pipeline;
            
            // Try to remove non-existent handler
            try {
                pipeline.removeHandler("non_existent");
            } catch (const CSException& e) {
                std::cout << "Expected error: " << e.what() << std::endl;
            }
            
            // Try to add invalid compression
            try {
                pipeline.addCompression("invalid_algorithm");
            } catch (const CSException& e) {
                std::cout << "Expected error: " << e.what() << std::endl;
            }
        }
        
        std::cout << "\n=== Pipeline Configuration Summary ===" << std::endl;
        std::cout << "ChannelPipeline supports:" << std::endl;
        std::cout << "  - Compression (deflate, gzip, zlib)" << std::endl;
        std::cout << "  - Decompression" << std::endl;
        std::cout << "  - TLS encryption with hostname verification" << std::endl;
        std::cout << "  - Dynamic handler addition/removal" << std::endl;
        std::cout << "\nHandlers process data as it flows through the channel" << std::endl;
        
        std::cout << "\nChannelPipeline example completed successfully!" << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "Pipeline error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}