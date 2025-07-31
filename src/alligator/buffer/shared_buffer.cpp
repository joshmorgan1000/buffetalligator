#include <alligator/buffer/shared_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <random>
#include <sstream>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace alligator {

SharedBuffer::SharedBuffer(size_t size, const std::string& name)
    : shm_name_(name.empty() ? generate_shm_name() : name),
      shm_fd_(-1),
      mapped_memory_(nullptr),
      is_creator_(true),
      header_(nullptr),
      data_start_(nullptr) {
    
    // Set the total size including header
    buf_size_ = size + sizeof(SharedHeader);
    
    // Try to create new shared memory
    if (!create_shared_memory()) {
        // If creation failed, try to attach to existing
        is_creator_ = false;
        if (!attach_shared_memory()) {
            throw std::runtime_error("Failed to create or attach shared memory");
        }
    }
    
    // Adjust buf_size_ to exclude header
    buf_size_ = size;
}

SharedBuffer::~SharedBuffer() {
    deallocate();
}

ChainBuffer* SharedBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::SHARED);
}

void SharedBuffer::clear(uint8_t value) {
    if (data_start_) {
        std::memset(data_start_, value, buf_size_);
    }
}

void SharedBuffer::deallocate() {
    if (mapped_memory_) {
        // Decrement reference count
        if (header_) {
            header_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        }
        
#ifdef _WIN32
        UnmapViewOfFile(mapped_memory_);
        if (is_creator_) {
            // Windows shared memory is cleaned up when last handle closes
        }
#else
        munmap(mapped_memory_, buf_size_ + sizeof(SharedHeader));
        
        if (shm_fd_ >= 0) {
            close(shm_fd_);
            
            // Unlink if we're the last one
            if (is_creator_ && header_ && header_->ref_count.load() == 0) {
                shm_unlink(shm_name_.c_str());
            }
        }
#endif
        
        mapped_memory_ = nullptr;
        header_ = nullptr;
        data_start_ = nullptr;
        shm_fd_ = -1;
    }
}

uint8_t* SharedBuffer::data() {
    return data_start_;
}

const uint8_t* SharedBuffer::data() const {
    return data_start_;
}

std::span<uint8_t> SharedBuffer::get_span(size_t offset, size_t size) {
    if (!data_start_ || offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(data_start_ + offset, actual_size);
}

std::span<const uint8_t> SharedBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (!data_start_ || offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(data_start_ + offset, actual_size);
}

bool SharedBuffer::create_shared_memory() {
#ifdef _WIN32
    // Windows implementation
    HANDLE hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(buf_size_ + sizeof(SharedHeader)),
        shm_name_.c_str()
    );
    
    if (hMapFile == NULL) {
        return false;
    }
    
    mapped_memory_ = MapViewOfFile(
        hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        buf_size_ + sizeof(SharedHeader)
    );
    
    CloseHandle(hMapFile);
    
    if (!mapped_memory_) {
        return false;
    }
#else
    // POSIX implementation
    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (shm_fd_ < 0) {
        return false;
    }
    
    // Set size
    if (ftruncate(shm_fd_, buf_size_ + sizeof(SharedHeader)) < 0) {
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        shm_fd_ = -1;
        return false;
    }
    
    // Map memory
    mapped_memory_ = mmap(nullptr, buf_size_ + sizeof(SharedHeader),
                         PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    
    if (mapped_memory_ == MAP_FAILED) {
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        shm_fd_ = -1;
        mapped_memory_ = nullptr;
        return false;
    }
#endif
    
    // Initialize header
    header_ = reinterpret_cast<SharedHeader*>(mapped_memory_);
    data_start_ = reinterpret_cast<uint8_t*>(mapped_memory_) + sizeof(SharedHeader);
    
    // Initialize header fields
    header_->ref_count.store(1, std::memory_order_release);
    header_->total_size.store(buf_size_, std::memory_order_release);
    header_->version.store(1, std::memory_order_release);
    header_->create_time.store(
        std::chrono::system_clock::now().time_since_epoch().count(),
        std::memory_order_release
    );
    
    // Set creator name
    std::string creator = "BuffetAlligator";
#ifdef _WIN32
    char username[256];
    DWORD size = sizeof(username);
    if (GetUserNameA(username, &size)) {
        creator += "_";
        creator += username;
    }
#else
    if (const char* user = getenv("USER")) {
        creator += "_";
        creator += user;
    }
#endif
    std::strncpy(header_->creator_name, creator.c_str(), sizeof(header_->creator_name) - 1);
    header_->creator_name[sizeof(header_->creator_name) - 1] = '\0';
    
    return true;
}

bool SharedBuffer::attach_shared_memory() {
#ifdef _WIN32
    // Windows implementation
    HANDLE hMapFile = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        shm_name_.c_str()
    );
    
    if (hMapFile == NULL) {
        return false;
    }
    
    mapped_memory_ = MapViewOfFile(
        hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        0  // Map entire file
    );
    
    CloseHandle(hMapFile);
    
    if (!mapped_memory_) {
        return false;
    }
#else
    // POSIX implementation
    shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666);
    if (shm_fd_ < 0) {
        return false;
    }
    
    // Get size
    struct stat st;
    if (fstat(shm_fd_, &st) < 0) {
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }
    
    // Map memory
    mapped_memory_ = mmap(nullptr, st.st_size,
                         PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    
    if (mapped_memory_ == MAP_FAILED) {
        close(shm_fd_);
        shm_fd_ = -1;
        mapped_memory_ = nullptr;
        return false;
    }
#endif
    
    // Set up pointers
    header_ = reinterpret_cast<SharedHeader*>(mapped_memory_);
    data_start_ = reinterpret_cast<uint8_t*>(mapped_memory_) + sizeof(SharedHeader);
    
    // Increment reference count
    header_->ref_count.fetch_add(1, std::memory_order_acq_rel);
    
    // Verify size matches
    if (header_->total_size.load() != buf_size_) {
        // Size mismatch - adjust our size
        buf_size_ = header_->total_size.load();
    }
    
    return true;
}

std::string SharedBuffer::generate_shm_name() {
    std::stringstream ss;
    ss << "/buffetalligator_shm_";
    
    // Add process ID
    #ifdef _WIN32
        ss << GetCurrentProcessId();
    #else
        ss << getpid();
    #endif
    
    ss << "_";
    
    // Add timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    ss << time_t;
    
    ss << "_";
    
    // Add random component
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    ss << dis(gen);
    
    return ss.str();
}

SharedBuffer* SharedBuffer::attach(const std::string& name, size_t size) {
    try {
        auto* buffer = new SharedBuffer(size, name);
        if (!buffer->is_creator_) {
            // Successfully attached to existing
            return buffer;
        } else {
            // Created new instead of attaching
            delete buffer;
            return nullptr;
        }
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace alligator