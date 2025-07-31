#pragma once

#include <cstdint>
#include <atomic>
#include <span>
#include <optional>
#include <string>
#include <memory>
#include <alligator/buffer/chain_buffer.hpp>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/**
 * @file file_backed_buffer.hpp
 * @brief Memory-mapped file buffer implementation with chaining support
 * 
 * This buffer uses memory-mapped files for zero-copy file I/O operations.
 * It provides virtual memory backed by a file, allowing for efficient
 * handling of large data sets that may exceed available RAM.
 */

namespace alligator {

/**
 * @brief File-backed buffer implementation using memory-mapped files
 * 
 * This buffer creates or maps an existing file into virtual memory,
 * providing zero-copy access to file contents. Changes are automatically
 * synchronized to disk based on the selected sync mode.
 * 
 * Key features:
 * - Memory-mapped file access: Zero-copy file I/O
 * - Large file support: Can handle files larger than RAM
 * - Automatic persistence: Changes are saved to disk
 * - Cross-platform: Works on Windows, Linux, and macOS
 * - Sparse file support: Efficient for partially-filled large files
 */
class FileBackedBuffer : public ChainBuffer {
public:
    /**
     * @brief File mapping modes
     */
    enum class MapMode {
        ReadOnly,      ///< Read-only mapping
        ReadWrite,     ///< Read-write mapping (default)
        CopyOnWrite    ///< Copy-on-write mapping (private changes)
    };

    /**
     * @brief Synchronization modes for file writes
     */
    enum class SyncMode {
        Async,         ///< OS decides when to flush (default)
        Sync,          ///< Synchronous writes
        DataSync       ///< Sync data but not metadata
    };

private:
    std::string file_path_;             ///< Path to the backing file
    MapMode map_mode_;                  ///< Mapping mode
    SyncMode sync_mode_;                ///< Synchronization mode
    void* mapped_data_{nullptr};        ///< Pointer to mapped memory
    size_t mapped_size_{0};             ///< Size of mapped region
    bool is_temporary_{false};          ///< Whether to delete file on destruction
    bool is_sparse_{false};             ///< Whether file is sparse
    
#ifdef _WIN32
    HANDLE file_handle_{INVALID_HANDLE_VALUE};     ///< Windows file handle
    HANDLE mapping_handle_{INVALID_HANDLE_VALUE};   ///< Windows mapping handle
#else
    int file_descriptor_{-1};           ///< POSIX file descriptor
#endif

    /**
     * @brief Create or open the backing file
     * @param create_new Whether to create a new file
     * @return true on success, false on failure
     */
    bool open_or_create_file(bool create_new);

    /**
     * @brief Map the file into memory
     * @return true on success, false on failure
     */
    bool map_file();

    /**
     * @brief Unmap the file from memory
     */
    void unmap_file();

    /**
     * @brief Ensure file is at least the specified size
     * @param size Required size in bytes
     * @return true on success, false on failure
     */
    bool ensure_file_size(size_t size);

public:
    /**
     * @brief Constructor for new file-backed buffer
     * @param capacity Size of the buffer in bytes
     * @param file_path Path to the backing file (empty for temp file)
     * @param map_mode Mapping mode
     * @param sync_mode Synchronization mode
     * @param sparse Whether to create a sparse file
     */
    FileBackedBuffer(size_t capacity = 0,
                     const std::string& file_path = "",
                     MapMode map_mode = MapMode::ReadWrite,
                     SyncMode sync_mode = SyncMode::Async,
                     bool sparse = false);

    /**
     * @brief Constructor for mapping existing file
     * @param file_path Path to existing file
     * @param map_mode Mapping mode
     * @param offset Offset in file to start mapping
     * @param length Length to map (0 for entire file)
     */
    FileBackedBuffer(const std::string& file_path,
                     MapMode map_mode,
                     size_t offset,
                     size_t length);

    /**
     * @brief Destructor
     */
    ~FileBackedBuffer() override;

    bool is_local() const override {
        return true;  // Memory-mapped files are locally accessible
    }

    bool is_file_backed() const override {
        return true;  // This is a file-backed buffer
    }

    bool is_shared() const override {
        return map_mode_ != MapMode::CopyOnWrite;  // Shared unless copy-on-write
    }

    /**
     * @brief Factory method to create a FileBackedBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created FileBackedBuffer
     */
    static FileBackedBuffer* create(size_t capacity) {
        return new FileBackedBuffer(capacity);
    }
    
    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new FileBackedBuffer allocated via BuffetAlligator
     */
    ChainBuffer* create_new(size_t size) override;

    /**
     * @brief Clear the buffer, setting all bytes to specified value
     */
    void clear(uint8_t value) override;

    /**
     * @brief Deallocate the buffer memory
     */
    void deallocate() override;

    /**
     * @brief Get a pointer to the raw data
     * @return Pointer to the buffer data
     */
    uint8_t* data() override;

    /**
     * @brief Get a pointer to the raw data (const version)
     * @return Pointer to the buffer data
     */
    const uint8_t* data() const override;

    /**
     * @brief Get a span view of the buffer data
     * @param offset Offset in bytes from the beginning
     * @param size Size in bytes (0 for remaining)
     * @return Span view of the buffer data
     */
    std::span<uint8_t> get_span(size_t offset = 0, size_t size = 0) override;

    /**
     * @brief Get a span view of the buffer data (const version)
     */
    std::span<const uint8_t> get_span(const size_t& offset = 0, const size_t& size = 0) const override;

    /**
     * @brief Sync changes to disk
     * @param data_only If true, sync only data (not metadata)
     * @return true on success, false on failure
     */
    bool sync(bool data_only = false);

    /**
     * @brief Advise kernel about access pattern
     * @param offset Offset in buffer
     * @param length Length of region
     * @param advice Access pattern hint
     * @return true on success, false on failure
     */
    enum class AccessAdvice {
        Normal,        ///< No special treatment
        Sequential,    ///< Will be accessed sequentially
        Random,        ///< Will be accessed randomly
        WillNeed,      ///< Will need this data soon
        DontNeed       ///< Won't need this data soon
    };
    
    bool advise(size_t offset, size_t length, AccessAdvice advice);

    /**
     * @brief Lock pages in memory (prevent swapping)
     * @param offset Offset in buffer
     * @param length Length to lock (0 for entire buffer)
     * @return true on success, false on failure
     */
    bool lock_memory(size_t offset = 0, size_t length = 0);

    /**
     * @brief Unlock pages from memory
     * @param offset Offset in buffer
     * @param length Length to unlock (0 for entire buffer)
     * @return true on success, false on failure
     */
    bool unlock_memory(size_t offset = 0, size_t length = 0);

    /**
     * @brief Resize the backing file and remap
     * @param new_size New size in bytes
     * @return true on success, false on failure
     */
    bool resize(size_t new_size);

    /**
     * @brief Get the file path
     * @return Path to the backing file
     */
    const std::string& get_file_path() const { return file_path_; }

    /**
     * @brief Check if this is a temporary file
     * @return true if temporary, false otherwise
     */
    bool is_temporary() const { return is_temporary_; }

    /**
     * @brief Check if this is a sparse file
     * @return true if sparse, false otherwise
     */
    bool is_sparse() const { return is_sparse_; }

    /**
     * @brief Get file statistics
     */
    struct FileStats {
        size_t file_size;      ///< Total file size
        size_t disk_usage;     ///< Actual disk space used
        uint64_t modify_time;  ///< Last modification time
        uint64_t access_time;  ///< Last access time
    };
    
    std::optional<FileStats> get_stats() const;

    /**
     * @brief Create a temporary file path
     * @return Path to a new temporary file
     */
    static std::string create_temp_path();
};

} // namespace alligator
