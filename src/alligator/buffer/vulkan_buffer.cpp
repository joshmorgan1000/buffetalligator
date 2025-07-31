#include <alligator/buffer/vulkan_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <vector>

namespace alligator {

// Static member definitions
VkInstance VulkanBuffer::vulkan_instance_ = VK_NULL_HANDLE;
VkDevice VulkanBuffer::default_device_ = VK_NULL_HANDLE;
VkPhysicalDevice VulkanBuffer::default_physical_device_ = VK_NULL_HANDLE;
std::atomic<bool> VulkanBuffer::vulkan_initialized_{false};

VulkanBuffer::VulkanBuffer(size_t capacity) : GPUBuffer(capacity, GPUMemoryType::DEVICE_LOCAL) {
    buf_size_ = capacity;
    
    // Auto-initialize Vulkan if not already initialized
    if (!vulkan_initialized_.load(std::memory_order_acquire)) {
        if (!auto_initialize_vulkan()) {
            throw std::runtime_error("VulkanBuffer: Failed to initialize Vulkan automatically.");
        }
    }
    
    device_ = default_device_;
    physical_device_ = default_physical_device_;
    
    create_buffer();
    clear(0);
}

VulkanBuffer::~VulkanBuffer() {
    deallocate();
}

void VulkanBuffer::clear(uint8_t value) {
    if (mapped_data_) {
        std::fill(static_cast<uint8_t*>(mapped_data_), 
                  static_cast<uint8_t*>(mapped_data_) + buf_size_, value);
    }
}

void VulkanBuffer::deallocate() {
    if (device_ != VK_NULL_HANDLE) {
        if (mapped_data_) {
            vkUnmapMemory(device_, device_memory_);
            mapped_data_ = nullptr;
        }
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
            buffer_ = VK_NULL_HANDLE;
        }
        if (device_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, device_memory_, nullptr);
            device_memory_ = VK_NULL_HANDLE;
        }
    }
    deallocation_counter_.fetch_add(buf_size_, std::memory_order_relaxed);
}

uint8_t* VulkanBuffer::data() {
    return static_cast<uint8_t*>(mapped_data_);
}

const uint8_t* VulkanBuffer::data() const {
    return static_cast<const uint8_t*>(mapped_data_);
}

std::span<uint8_t> VulkanBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        throw std::out_of_range("Span offset out of bounds");
    }
    size_t span_size = size > 0 ? size : (buf_size_ - offset);
    if (offset + span_size > buf_size_) {
        throw std::out_of_range("Span size out of bounds");
    }
    return std::span<uint8_t>(static_cast<uint8_t*>(mapped_data_) + offset, span_size);
}

std::span<const uint8_t> VulkanBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        throw std::out_of_range("Span offset out of bounds");
    }
    size_t span_size = size > 0 ? size : (buf_size_ - offset);
    if (offset + span_size > buf_size_) {
        throw std::out_of_range("Span size out of bounds");
    }
    return std::span<const uint8_t>(static_cast<const uint8_t*>(mapped_data_) + offset, span_size);
}

ChainBuffer* VulkanBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::VULKAN);
}

void VulkanBuffer::create_buffer() {
    // Create buffer info
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buf_size_;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | 
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | 
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan buffer");
    }

    // Get memory requirements
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(device_, buffer_, &mem_requirements);

    // Allocate memory
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &device_memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate Vulkan buffer memory");
    }

    // Bind buffer to memory
    vkBindBufferMemory(device_, buffer_, device_memory_, 0);

    // Map memory persistently
    if (vkMapMemory(device_, device_memory_, 0, buf_size_, 0, &mapped_data_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to map Vulkan buffer memory");
    }

    allocation_counter_.fetch_add(buf_size_, std::memory_order_relaxed);
}

uint32_t VulkanBuffer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && 
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

bool VulkanBuffer::auto_initialize_vulkan() {
    // Check if already initialized by another thread
    if (vulkan_initialized_.load(std::memory_order_acquire)) {
        return true;
    }

    try {
        // Create Vulkan instance
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "Psyne";
        app_info.applicationVersion = VK_MAKE_VERSION(3, 0, 0);
        app_info.pEngineName = "Psyne Engine";
        app_info.engineVersion = VK_MAKE_VERSION(3, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_0;

        // Enable required extensions for macOS
        std::vector<const char*> extensions;
#ifdef __APPLE__
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif
        
        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
#ifdef __APPLE__
        create_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        if (vkCreateInstance(&create_info, nullptr, &vulkan_instance_) != VK_SUCCESS) {
            return false;
        }

        // Get physical devices
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(vulkan_instance_, &device_count, nullptr);

        if (device_count == 0) {
            return false;
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(vulkan_instance_, &device_count, devices.data());

        // Use the first available device
        default_physical_device_ = devices[0];

        // Get queue families
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(default_physical_device_, &queue_family_count, nullptr);

        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(default_physical_device_, &queue_family_count, queue_families.data());

        // Find a queue family that supports graphics or compute
        uint32_t queue_family_index = UINT32_MAX;
        for (uint32_t i = 0; i < queue_family_count; i++) {
            if (queue_families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                queue_family_index = i;
                break;
            }
        }

        if (queue_family_index == UINT32_MAX) {
            return false;
        }

        // Create logical device
        VkDeviceQueueCreateInfo queue_create_info{};
        queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = queue_family_index;
        queue_create_info.queueCount = 1;
        float queue_priority = 1.0f;
        queue_create_info.pQueuePriorities = &queue_priority;

        VkPhysicalDeviceFeatures device_features{};

        VkDeviceCreateInfo device_create_info{};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.pQueueCreateInfos = &queue_create_info;
        device_create_info.queueCreateInfoCount = 1;
        device_create_info.pEnabledFeatures = &device_features;

        if (vkCreateDevice(default_physical_device_, &device_create_info, nullptr, &default_device_) != VK_SUCCESS) {
            return false;
        }

        // Mark as initialized
        vulkan_initialized_.store(true, std::memory_order_release);
        return true;

    } catch (const std::exception& e) {
        return false;
    }
}

void* VulkanBuffer::map(size_t offset, size_t size) {
    if (!mapped_data_ && buffer_ && device_memory_) {
        size_t map_size = (size == 0) ? buf_size_ : size;
        VkResult result = vkMapMemory(device_, device_memory_, offset, map_size, 0, &mapped_data_);
        if (result != VK_SUCCESS) {
            return nullptr;
        }
    }
    return mapped_data_ ? static_cast<uint8_t*>(mapped_data_) + offset : nullptr;
}

void VulkanBuffer::unmap() {
    if (mapped_data_ && device_ && device_memory_) {
        vkUnmapMemory(device_, device_memory_);
        mapped_data_ = nullptr;
    }
}

bool VulkanBuffer::upload(const void* src, size_t size, size_t offset) {
    if (!src || offset + size > buf_size_) {
        return false;
    }
    
    void* dst = map(offset, size);
    if (!dst) {
        return false;
    }
    
    std::memcpy(dst, src, size);
    
    // Flush if not host coherent
    if (!(memory_properties_ & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = device_memory_;
        range.offset = offset;
        range.size = size;
        vkFlushMappedMemoryRanges(device_, 1, &range);
    }
    
    return true;
}

bool VulkanBuffer::download(void* dst, size_t size, size_t offset) {
    if (!dst || offset + size > buf_size_) {
        return false;
    }
    
    void* src = map(offset, size);
    if (!src) {
        return false;
    }
    
    // Invalidate cache if not host coherent
    if (!(memory_properties_ & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = device_memory_;
        range.offset = offset;
        range.size = size;
        vkInvalidateMappedMemoryRanges(device_, 1, &range);
    }
    
    std::memcpy(dst, src, size);
    return true;
}

bool VulkanBuffer::copy_from(GPUBuffer* src, size_t size, size_t src_offset, size_t dst_offset) {
    // This would require command buffer recording and submission
    // For now, fallback to CPU copy
    VulkanBuffer* src_vulkan = dynamic_cast<VulkanBuffer*>(src);
    if (!src_vulkan) {
        return false;
    }
    
    // Simple CPU-side copy for now
    std::vector<uint8_t> temp(size);
    if (!src_vulkan->download(temp.data(), size, src_offset)) {
        return false;
    }
    
    return upload(temp.data(), size, dst_offset);
}

void VulkanBuffer::sync() {
    // Would submit and wait for command buffer if we had GPU operations pending
    // For HOST_VISIBLE buffers, this is a no-op
}

} // namespace alligator
