#pragma once

#include <alligator/buffer/chain_buffer.hpp>
#include <string>
#include <atomic>

namespace alligator {

/**
 * @class SharedBuffer
 * @brief Shared memory buffer for inter-process communication
 * 
 * This buffer type creates memory that can be shared between multiple
 * processes on the same system. It uses platform-specific shared memory
 * APIs (POSIX shm on Unix/Linux/macOS, Windows shared memory on Windows).
 * 
 * Features:
 * - Zero-copy IPC between processes
 * - Named shared memory segments
 * - Automatic cleanup on last reference
 * - Cross-process synchronization support
 */
class SharedBuffer : public ChainBuffer {
private:
    std::string shm_name_;           ///< Name of shared memory segment
    int shm_fd_;                     ///< File descriptor for shared memory
    void* mapped_memory_;            ///< Mapped memory address
    bool is_creator_;                ///< True if this process created the segment
    
    // Shared memory header for cross-process coordination
    struct SharedHeader {
        std::atomic<uint32_t> ref_count;    ///< Number of processes attached
        std::atomic<uint64_t> total_size;   ///< Total size of shared segment
        std::atomic<uint32_t> version;      ///< Version for compatibility
        std::atomic<uint64_t> create_time;  ///< Creation timestamp
        char creator_name[256];             ///< Name of creating process
    };
    
    SharedHeader* header_;           ///< Pointer to shared header
    uint8_t* data_start_;           ///< Start of user data (after header)
    
public:
    /**
     * @brief Constructor - creates or attaches to shared memory
     * @param size Size of the buffer in bytes
     * @param name Optional name for the shared memory segment
     */
    explicit SharedBuffer(size_t size, const std::string& name = "");
    
    /**
     * @brief Destructor
     */
    ~SharedBuffer() override;
    
    /**
     * @brief Factory method
     * @param size Size in bytes
     * @return Pointer to created SharedBuffer
     */
    static SharedBuffer* create(size_t size) {
        return new SharedBuffer(size);
    }
    
    /**
     * @brief Create a new buffer of the same type
     * @param size Size in bytes
     * @return Pointer to new SharedBuffer
     */
    ChainBuffer* create_new(size_t size) override;
    
    /**
     * @brief Check if buffer is local (CPU accessible)
     * @return Always true for shared memory
     */
    bool is_local() const override { return true; }
    
    /**
     * @brief Check if buffer is file-backed
     * @return False (uses shared memory, not files)
     */
    bool is_file_backed() const override { return false; }
    
    /**
     * @brief Check if buffer is shared memory
     * @return Always true for SharedBuffer
     */
    bool is_shared() const override { return true; }
    
    /**
     * @brief Clear the buffer
     * @param value Value to clear with
     */
    void clear(uint8_t value = 0) override;
    
    /**
     * @brief Deallocate the shared memory
     */
    void deallocate() override;
    
    /**
     * @brief Get raw data pointer
     * @return Pointer to buffer data
     */
    uint8_t* data() override;
    const uint8_t* data() const override;
    
    /**
     * @brief Get span view of buffer
     */
    std::span<uint8_t> get_span(size_t offset = 0, size_t size = 0) override;
    std::span<const uint8_t> get_span(const size_t& offset = 0, const size_t& size = 0) const override;
    
    /**
     * @brief Get the shared memory name
     * @return Name of the shared memory segment
     */
    const std::string& get_shm_name() const { return shm_name_; }
    
    /**
     * @brief Get number of processes attached
     * @return Reference count from shared header
     */
    uint32_t get_ref_count() const {
        return header_ ? header_->ref_count.load() : 0;
    }
    
    /**
     * @brief Attach to existing shared memory by name
     * @param name Name of shared memory segment
     * @param size Expected size (for validation)
     * @return Pointer to SharedBuffer or nullptr on failure
     */
    static SharedBuffer* attach(const std::string& name, size_t size);
    
private:
    /**
     * @brief Platform-specific shared memory creation
     * @return True on success
     */
    bool create_shared_memory();
    
    /**
     * @brief Platform-specific shared memory attachment
     * @return True on success
     */
    bool attach_shared_memory();
    
    /**
     * @brief Generate unique name if none provided
     * @return Generated name
     */
    std::string generate_shm_name();
};

} // namespace alligator
