#include <alligator/buffer/file_backed_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <random>
#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#endif

/**
 * @file file_backed_buffer.cpp
 * @brief Implementation of memory-mapped file buffer with chaining support
 */

namespace alligator {

// Default buffer size if not specified (1 MB)
static constexpr size_t DEFAULT_FILE_BACKED_BUFFER_SIZE = 1048576;

std::string FileBackedBuffer::create_temp_path() {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    
    // Generate unique filename
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999999);
    
    std::stringstream ss;
    ss << "bufferalligator_buffer_" << now << "_" << dis(gen) << ".tmp";
    
    return (temp_dir / ss.str()).string();
}

FileBackedBuffer::FileBackedBuffer(size_t capacity, 
                                   const std::string& file_path,
                                   MapMode map_mode,
                                   SyncMode sync_mode,
                                   bool sparse)
    : file_path_(file_path), map_mode_(map_mode), sync_mode_(sync_mode), is_sparse_(sparse) {
    
    // Use default size if not specified
    if (capacity == 0) {
        capacity = DEFAULT_FILE_BACKED_BUFFER_SIZE;
    }
    
    // Generate temp file path if not specified
    if (file_path_.empty()) {
        file_path_ = create_temp_path();
        is_temporary_ = true;
    }
    
    buf_size_ = capacity;
    
    // Create or open the file
    if (!open_or_create_file(true)) {
        throw std::runtime_error("Failed to create file: " + file_path_);
    }
    
    // Ensure file has the required size
    if (!ensure_file_size(capacity)) {
        throw std::runtime_error("Failed to resize file to " + std::to_string(capacity) + " bytes");
    }
    
    // Map the file
    if (!map_file()) {
        throw std::runtime_error("Failed to map file: " + file_path_);
    }
    
    // Clear to zero if new file
    if (is_temporary_ && mapped_data_) {
        clear(0);
    }
    
//    LOG_DEBUG_STREAM << "FileBackedBuffer: Created buffer with capacity " << capacity 
}

FileBackedBuffer::FileBackedBuffer(const std::string& file_path,
                                   MapMode map_mode,
                                   size_t offset,
                                   size_t length)
    : file_path_(file_path), map_mode_(map_mode), sync_mode_(SyncMode::Async) {
    
    if (file_path_.empty()) {
        throw std::invalid_argument("File path cannot be empty for existing file mapping");
    }
    
    // Open existing file
    if (!open_or_create_file(false)) {
        throw std::runtime_error("Failed to open file: " + file_path_);
    }
    
    // Get file size
#ifdef _WIN32
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle_, &file_size)) {
        throw std::runtime_error("Failed to get file size");
    }
    size_t total_size = static_cast<size_t>(file_size.QuadPart);
#else
    struct stat st;
    if (fstat(file_descriptor_, &st) != 0) {
        throw std::runtime_error("Failed to get file size");
    }
    size_t total_size = static_cast<size_t>(st.st_size);
#endif
    
    // Validate offset and length
    if (offset > total_size) {
        throw std::invalid_argument("Offset exceeds file size");
    }
    
    if (length == 0) {
        length = total_size - offset;
    } else if (offset + length > total_size) {
        throw std::invalid_argument("Offset + length exceeds file size");
    }
    
    buf_size_ = length;
    
    // Map the file
    if (!map_file()) {
        throw std::runtime_error("Failed to map file: " + file_path_);
    }
    
//    LOG_DEBUG_STREAM << "FileBackedBuffer: Mapped existing file " << file_path_ 
}

FileBackedBuffer::~FileBackedBuffer() {
    // Unmap and close file
    unmap_file();
    
#ifdef _WIN32
    if (file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle_);
    }
#else
    if (file_descriptor_ != -1) {
        close(file_descriptor_);
    }
#endif
    
    // Delete temporary files
    if (is_temporary_ && !file_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(file_path_, ec);
        if (ec) {
//            LOG_WARN_STREAM << "Failed to delete temporary file " << file_path_ 
        }
    }
    
//    LOG_DEBUG_STREAM << "FileBackedBuffer: Destroyed buffer backed by " << file_path_;
}

bool FileBackedBuffer::open_or_create_file(bool create_new) {
#ifdef _WIN32
    DWORD access = GENERIC_READ;
    DWORD share = FILE_SHARE_READ;
    DWORD creation = OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    
    if (map_mode_ != MapMode::ReadOnly) {
        access |= GENERIC_WRITE;
        share |= FILE_SHARE_WRITE;
    }
    
    if (create_new) {
        creation = CREATE_ALWAYS;
    }
    
    if (is_sparse_) {
        flags |= FILE_ATTRIBUTE_SPARSE_FILE;
    }
    
    file_handle_ = CreateFileA(file_path_.c_str(), access, share, nullptr,
                               creation, flags, nullptr);
    
    if (file_handle_ == INVALID_HANDLE_VALUE) {
//        LOG_ERROR_STREAM << "Failed to open/create file " << file_path_ 
        return false;
    }
    
    // Mark as sparse if requested
    if (is_sparse_ && create_new) {
        DWORD bytes_returned;
        if (!DeviceIoControl(file_handle_, FSCTL_SET_SPARSE, nullptr, 0,
                           nullptr, 0, &bytes_returned, nullptr)) {
//            LOG_WARN_STREAM << "Failed to mark file as sparse";
        }
    }
#else
    int flags = O_CLOEXEC;
    mode_t mode = S_IRUSR | S_IWUSR;
    
    if (map_mode_ == MapMode::ReadOnly) {
        flags |= O_RDONLY;
    } else {
        flags |= O_RDWR;
    }
    
    if (create_new) {
        flags |= O_CREAT | O_TRUNC;
    }
    
    file_descriptor_ = open(file_path_.c_str(), flags, mode);
    
    if (file_descriptor_ == -1) {
//        LOG_ERROR_STREAM << "Failed to open/create file " << file_path_ 
        return false;
    }
#endif
    
    return true;
}

bool FileBackedBuffer::ensure_file_size(size_t size) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    
    if (!SetFilePointerEx(file_handle_, li, nullptr, FILE_BEGIN)) {
//        LOG_ERROR_STREAM << "Failed to set file pointer: " << GetLastError();
        return false;
    }
    
    if (!SetEndOfFile(file_handle_)) {
//        LOG_ERROR_STREAM << "Failed to set end of file: " << GetLastError();
        return false;
    }
    
    // For sparse files, this doesn't actually allocate disk space
    if (is_sparse_) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        FILE_ZERO_DATA_INFORMATION fzdi;
        fzdi.FileOffset = zero;
        fzdi.BeyondFinalZero = li;
        
        DWORD bytes_returned;
        if (!DeviceIoControl(file_handle_, FSCTL_SET_ZERO_DATA, &fzdi,
                           sizeof(fzdi), nullptr, 0, &bytes_returned, nullptr)) {
//            LOG_WARN_STREAM << "Failed to mark sparse region";
        }
    }
#else
    if (ftruncate(file_descriptor_, static_cast<off_t>(size)) != 0) {
//        LOG_ERROR_STREAM << "Failed to resize file: " << strerror(errno);
        return false;
    }
    
    // For sparse files on Linux, use fallocate with FALLOC_FL_PUNCH_HOLE
    #ifdef __linux__
    if (is_sparse_) {
        // This creates a sparse file by punching a hole
        fallocate(file_descriptor_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                 0, static_cast<off_t>(size));
    }
    #endif
#endif
    
    return true;
}

bool FileBackedBuffer::map_file() {
#ifdef _WIN32
    DWORD protect = PAGE_READONLY;
    DWORD access = FILE_MAP_READ;
    
    if (map_mode_ == MapMode::ReadWrite) {
        protect = PAGE_READWRITE;
        access = FILE_MAP_WRITE;
    } else if (map_mode_ == MapMode::CopyOnWrite) {
        protect = PAGE_WRITECOPY;
        access = FILE_MAP_COPY;
    }
    
    mapping_handle_ = CreateFileMappingA(file_handle_, nullptr, protect,
                                        0, 0, nullptr);
    
    if (mapping_handle_ == nullptr) {
//        LOG_ERROR_STREAM << "Failed to create file mapping: " << GetLastError();
        return false;
    }
    
    mapped_data_ = MapViewOfFile(mapping_handle_, access, 0, 0, buf_size_);
    
    if (mapped_data_ == nullptr) {
//        LOG_ERROR_STREAM << "Failed to map view of file: " << GetLastError();
        CloseHandle(mapping_handle_);
        mapping_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }
#else
    int prot = PROT_READ;
    int flags = MAP_SHARED;
    
    if (map_mode_ == MapMode::ReadWrite) {
        prot |= PROT_WRITE;
    } else if (map_mode_ == MapMode::CopyOnWrite) {
        prot |= PROT_WRITE;
        flags = MAP_PRIVATE;
    }
    
    mapped_data_ = mmap(nullptr, buf_size_, prot, flags, file_descriptor_, 0);
    
    if (mapped_data_ == MAP_FAILED) {
//        LOG_ERROR_STREAM << "Failed to mmap file: " << strerror(errno);
        mapped_data_ = nullptr;
        return false;
    }
#endif
    
    mapped_size_ = buf_size_;
    return true;
}

void FileBackedBuffer::unmap_file() {
    if (mapped_data_) {
#ifdef _WIN32
        UnmapViewOfFile(mapped_data_);
        if (mapping_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(mapping_handle_);
            mapping_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        munmap(mapped_data_, mapped_size_);
#endif
        mapped_data_ = nullptr;
        mapped_size_ = 0;
    }
}

ChainBuffer* FileBackedBuffer::create_new(size_t size) {
    // Allocate through BuffetAlligator to ensure proper registration
    return get_buffet_alligator().allocate_buffer(
        static_cast<uint32_t>(size), 
        BFType::FILE_BACKED
    );
}

void FileBackedBuffer::clear(uint8_t value) {
    if (!mapped_data_) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::clear() called on unmapped buffer";
        return;
    }
    
    std::memset(mapped_data_, value, buf_size_);
    
    // Force sync if using synchronous mode
    if (sync_mode_ == SyncMode::Sync) {
        sync(false);
    }
}

void FileBackedBuffer::deallocate() {
    // Sync any pending changes
    if (mapped_data_ && map_mode_ != MapMode::ReadOnly) {
        sync(false);
    }
    
    // Unmap the file
    unmap_file();
    
//    LOG_DEBUG_STREAM << "FileBackedBuffer::deallocate() completed";
}

uint8_t* FileBackedBuffer::data() {
    if (map_mode_ == MapMode::ReadOnly) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::data() called on read-only buffer";
        return nullptr;
    }
    return static_cast<uint8_t*>(mapped_data_);
}

const uint8_t* FileBackedBuffer::data() const {
    return static_cast<const uint8_t*>(mapped_data_);
}

std::span<uint8_t> FileBackedBuffer::get_span(size_t offset, size_t size) {
    if (!mapped_data_) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::get_span() called on unmapped buffer";
        return std::span<uint8_t>();
    }
    
    if (map_mode_ == MapMode::ReadOnly) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::get_span() called on read-only buffer";
        return std::span<uint8_t>();
    }
    
    if (offset > buf_size_) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::get_span() offset " << offset 
        return std::span<uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : size;
    
    if (offset + actual_size > buf_size_) {
//        LOG_WARN_STREAM << "FileBackedBuffer::get_span() clamping size from " << actual_size 
        actual_size = buf_size_ - offset;
    }
    
    return std::span<uint8_t>(static_cast<uint8_t*>(mapped_data_) + offset, actual_size);
}

std::span<const uint8_t> FileBackedBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (!mapped_data_) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::get_span() const called on unmapped buffer";
        return std::span<const uint8_t>();
    }
    
    if (offset > buf_size_) {
//        LOG_ERROR_STREAM << "FileBackedBuffer::get_span() const offset " << offset 
        return std::span<const uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : size;
    
    if (offset + actual_size > buf_size_) {
//        LOG_WARN_STREAM << "FileBackedBuffer::get_span() const clamping size from " << actual_size 
        actual_size = buf_size_ - offset;
    }
    
    return std::span<const uint8_t>(static_cast<const uint8_t*>(mapped_data_) + offset, actual_size);
}

bool FileBackedBuffer::sync(bool data_only) {
    if (!mapped_data_ || map_mode_ == MapMode::ReadOnly) {
        return true;  // Nothing to sync
    }
    
#ifdef _WIN32
    if (!FlushViewOfFile(mapped_data_, mapped_size_)) {
//        LOG_ERROR_STREAM << "Failed to flush view of file: " << GetLastError();
        return false;
    }
    
    if (!data_only) {
        if (!FlushFileBuffers(file_handle_)) {
//            LOG_ERROR_STREAM << "Failed to flush file buffers: " << GetLastError();
            return false;
        }
    }
#else
    int flags = data_only ? MS_ASYNC : MS_SYNC;
    if (msync(mapped_data_, mapped_size_, flags) != 0) {
//        LOG_ERROR_STREAM << "Failed to msync: " << strerror(errno);
        return false;
    }
    
    if (!data_only) {
        if (fsync(file_descriptor_) != 0) {
//            LOG_ERROR_STREAM << "Failed to fsync: " << strerror(errno);
            return false;
        }
    }
#endif
    
    return true;
}

bool FileBackedBuffer::advise(size_t offset, size_t length, AccessAdvice advice) {
    if (!mapped_data_) {
        return false;
    }
    
    if (length == 0) {
        length = buf_size_ - offset;
    }
    
#ifdef _WIN32
    // Windows doesn't have direct equivalent to madvise
    // Could use PrefetchVirtualMemory on Windows 8+
    return true;
#else
    int madv = MADV_NORMAL;
    switch (advice) {
        case AccessAdvice::Normal:     madv = MADV_NORMAL; break;
        case AccessAdvice::Sequential: madv = MADV_SEQUENTIAL; break;
        case AccessAdvice::Random:     madv = MADV_RANDOM; break;
        case AccessAdvice::WillNeed:   madv = MADV_WILLNEED; break;
        case AccessAdvice::DontNeed:   madv = MADV_DONTNEED; break;
    }
    
    void* addr = static_cast<uint8_t*>(mapped_data_) + offset;
    if (madvise(addr, length, madv) != 0) {
//        LOG_WARN_STREAM << "madvise failed: " << strerror(errno);
        return false;
    }
#endif
    
    return true;
}

bool FileBackedBuffer::lock_memory(size_t offset, size_t length) {
    if (!mapped_data_) {
        return false;
    }
    
    if (length == 0) {
        length = buf_size_ - offset;
    }
    
#ifdef _WIN32
    void* addr = static_cast<uint8_t*>(mapped_data_) + offset;
    if (!VirtualLock(addr, length)) {
//        LOG_ERROR_STREAM << "VirtualLock failed: " << GetLastError();
        return false;
    }
#else
    void* addr = static_cast<uint8_t*>(mapped_data_) + offset;
    if (mlock(addr, length) != 0) {
//        LOG_ERROR_STREAM << "mlock failed: " << strerror(errno);
        return false;
    }
#endif
    
    return true;
}

bool FileBackedBuffer::unlock_memory(size_t offset, size_t length) {
    if (!mapped_data_) {
        return false;
    }
    
    if (length == 0) {
        length = buf_size_ - offset;
    }
    
#ifdef _WIN32
    void* addr = static_cast<uint8_t*>(mapped_data_) + offset;
    if (!VirtualUnlock(addr, length)) {
        // This can fail if memory wasn't locked, which is OK
        return true;
    }
#else
    void* addr = static_cast<uint8_t*>(mapped_data_) + offset;
    if (munlock(addr, length) != 0) {
        // This can fail if memory wasn't locked, which is OK
        return true;
    }
#endif
    
    return true;
}

bool FileBackedBuffer::resize(size_t new_size) {
    if (map_mode_ == MapMode::ReadOnly) {
//        LOG_ERROR_STREAM << "Cannot resize read-only file-backed buffer";
        return false;
    }
    
    // Unmap current mapping
    unmap_file();
    
    // Resize the file
    if (!ensure_file_size(new_size)) {
        // Try to remap with old size on failure
        map_file();
        return false;
    }
    
    // Update size and remap
    buf_size_ = new_size;
    if (!map_file()) {
//        LOG_ERROR_STREAM << "Failed to remap file after resize";
        return false;
    }
    
    return true;
}

std::optional<FileBackedBuffer::FileStats> FileBackedBuffer::get_stats() const {
    FileStats stats{};
    
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file_handle_, &info)) {
        return std::nullopt;
    }
    
    LARGE_INTEGER size;
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    stats.file_size = static_cast<size_t>(size.QuadPart);
    
    if (is_sparse_) {
        FILE_ALLOCATED_RANGE_BUFFER range_query = {0};
        FILE_ALLOCATED_RANGE_BUFFER ranges[64];
        DWORD bytes_returned;
        
        range_query.FileOffset.QuadPart = 0;
        range_query.Length.QuadPart = size.QuadPart;
        
        stats.disk_usage = 0;
        if (DeviceIoControl(file_handle_, FSCTL_QUERY_ALLOCATED_RANGES,
                          &range_query, sizeof(range_query),
                          ranges, sizeof(ranges), &bytes_returned, nullptr)) {
            int range_count = bytes_returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            for (int i = 0; i < range_count; ++i) {
                stats.disk_usage += static_cast<size_t>(ranges[i].Length.QuadPart);
            }
        } else {
            stats.disk_usage = stats.file_size;  // Assume fully allocated
        }
    } else {
        stats.disk_usage = stats.file_size;
    }
    
    // Convert Windows FILETIME to Unix timestamp
    auto filetime_to_unix = [](const FILETIME& ft) -> uint64_t {
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
    };
    
    stats.modify_time = filetime_to_unix(info.ftLastWriteTime);
    stats.access_time = filetime_to_unix(info.ftLastAccessTime);
#else
    struct stat st;
    if (fstat(file_descriptor_, &st) != 0) {
        return std::nullopt;
    }
    
    stats.file_size = static_cast<size_t>(st.st_size);
    stats.disk_usage = static_cast<size_t>(st.st_blocks * 512);  // st_blocks is in 512-byte units
    stats.modify_time = static_cast<uint64_t>(st.st_mtime);
    stats.access_time = static_cast<uint64_t>(st.st_atime);
#endif
    
    return stats;
}

} // namespace alligator
