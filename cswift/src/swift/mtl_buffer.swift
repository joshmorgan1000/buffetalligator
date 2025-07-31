import Foundation

#if canImport(Metal)
import Metal
#endif

/**
 * @brief Create a default Metal device
 * 
 * @return Device handle or NULL on error
 */
@_cdecl("cswift_mtl_device_create_default")
func cswift_mtl_device_create_default() -> OpaquePointer? {
    #if canImport(Metal)
    guard let device = MTLCreateSystemDefaultDevice() else {
        return nil
    }
    
    let wrapper = MTLDeviceWrapper(device: device)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #else
    return nil
    #endif
}

/**
 * @brief Destroy Metal device
 * 
 * @param deviceHandle Device handle
 */
@_cdecl("cswift_mtl_device_destroy")
func cswift_mtl_device_destroy(_ deviceHandle: OpaquePointer) {
    _ = Unmanaged<MTLDeviceWrapper>.fromOpaque(deviceHandle.toRawPointer()).takeRetainedValue()
}

/**
 * @brief Create unified memory buffer
 * 
 * @param deviceHandle Device handle
 * @param size Buffer size in bytes
 * @param options Storage options (0 for default shared storage)
 * @return Buffer handle or NULL on error
 */
@_cdecl("cswift_mtl_buffer_create_unified")
func cswift_mtl_buffer_create_unified(
    _ deviceHandle: OpaquePointer,
    _ size: Int,
    _ options: UInt32
) -> OpaquePointer? {
    let deviceWrapper = Unmanaged<MTLDeviceWrapper>.fromOpaque(deviceHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    // Use shared storage mode for unified memory (CPU+GPU accessible)
    let resourceOptions: MTLResourceOptions = options == 0 ? .storageModeShared : MTLResourceOptions(rawValue: UInt(options))
    
    guard let buffer = deviceWrapper.device.makeBuffer(length: size, options: resourceOptions) else {
        return nil
    }
    
    let wrapper = MTLBufferWrapper(buffer: buffer)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #else
    // Stub implementation - allocate regular memory
    let buffer = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 16)
    let wrapper = MTLBufferWrapper(stubBuffer: buffer, size: size)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Create buffer with data
 * 
 * @param deviceHandle Device handle
 * @param data Data pointer
 * @param size Data size in bytes
 * @param options Storage options
 * @return Buffer handle or NULL on error
 */
@_cdecl("cswift_mtl_buffer_create_with_data")
func cswift_mtl_buffer_create_with_data(
    _ deviceHandle: OpaquePointer,
    _ data: UnsafeRawPointer,
    _ size: Int,
    _ options: UInt32
) -> OpaquePointer? {
    let deviceWrapper = Unmanaged<MTLDeviceWrapper>.fromOpaque(deviceHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    let resourceOptions: MTLResourceOptions = options == 0 ? .storageModeShared : MTLResourceOptions(rawValue: UInt(options))
    
    guard let buffer = deviceWrapper.device.makeBuffer(bytes: data, length: size, options: resourceOptions) else {
        return nil
    }
    
    let wrapper = MTLBufferWrapper(buffer: buffer)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #else
    // Stub implementation - copy data
    // ZERO_COPY_ALLOWED - stub fallback for non-Metal platforms
    let buffer = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 16)
    buffer.copyMemory(from: data, byteCount: size)
    let wrapper = MTLBufferWrapper(stubBuffer: buffer, size: size)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Get buffer contents pointer
 * 
 * @param bufferHandle Buffer handle
 * @return Pointer to buffer contents or NULL
 */
@_cdecl("cswift_mtl_buffer_contents")
func cswift_mtl_buffer_contents(_ bufferHandle: OpaquePointer) -> UnsafeMutableRawPointer? {
    let wrapper = Unmanaged<MTLBufferWrapper>.fromOpaque(bufferHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    return wrapper.buffer?.contents()
    #else
    return wrapper.stubBuffer
    #endif
}

/**
 * @brief Get buffer size
 * 
 * @param bufferHandle Buffer handle
 * @return Buffer size in bytes
 */
@_cdecl("cswift_mtl_buffer_size")
func cswift_mtl_buffer_size(_ bufferHandle: OpaquePointer) -> Int {
    let wrapper = Unmanaged<MTLBufferWrapper>.fromOpaque(bufferHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    return wrapper.buffer?.length ?? 0
    #else
    return wrapper.stubSize
    #endif
}

/**
 * @brief Synchronize buffer for CPU access
 * 
 * @param bufferHandle Buffer handle
 * @param location Location in buffer (offset)
 * @param length Length to synchronize
 */
@_cdecl("cswift_mtl_buffer_did_modify_range")
func cswift_mtl_buffer_did_modify_range(
    _ bufferHandle: OpaquePointer,
    _ location: Int,
    _ length: Int
) {
    #if canImport(Metal)
    let wrapper = Unmanaged<MTLBufferWrapper>.fromOpaque(bufferHandle.toRawPointer()).takeUnretainedValue()
    
    // For shared storage mode, we need to notify that CPU modified the buffer
    if let buffer = wrapper.buffer, buffer.storageMode == .shared {
        #if os(macOS)
        buffer.didModifyRange(location..<(location + length))
        #endif
    }
    #endif
}

/**
 * @brief Destroy buffer
 * 
 * @param bufferHandle Buffer handle
 */
@_cdecl("cswift_mtl_buffer_destroy")
func cswift_mtl_buffer_destroy(_ bufferHandle: OpaquePointer) {
    _ = Unmanaged<MTLBufferWrapper>.fromOpaque(bufferHandle.toRawPointer()).takeRetainedValue()
}

/**
 * @brief Create command queue
 * 
 * @param deviceHandle Device handle
 * @return Command queue handle or NULL on error
 */
@_cdecl("cswift_mtl_command_queue_create")
func cswift_mtl_command_queue_create(_ deviceHandle: OpaquePointer) -> OpaquePointer? {
    let deviceWrapper = Unmanaged<MTLDeviceWrapper>.fromOpaque(deviceHandle.toRawPointer()).takeUnretainedValue()
    
    #if canImport(Metal)
    guard let queue = deviceWrapper.device.makeCommandQueue() else {
        return nil
    }
    
    let wrapper = MTLCommandQueueWrapper(queue: queue)
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #else
    // Stub implementation
    let wrapper = MTLCommandQueueWrapper()
    return Unmanaged.passRetained(wrapper).toOpaque().toOpaquePointer()
    #endif
}

/**
 * @brief Destroy command queue
 * 
 * @param queueHandle Command queue handle
 */
@_cdecl("cswift_mtl_command_queue_destroy")
func cswift_mtl_command_queue_destroy(_ queueHandle: OpaquePointer) {
    _ = Unmanaged<MTLCommandQueueWrapper>.fromOpaque(queueHandle.toRawPointer()).takeRetainedValue()
}

// Wrapper classes
class MTLDeviceWrapper {
    #if canImport(Metal)
    let device: MTLDevice
    
    init(device: MTLDevice) {
        self.device = device
    }
    #else
    init() {}
    #endif
}

class MTLBufferWrapper {
    #if canImport(Metal)
    let buffer: MTLBuffer?
    #endif
    
    // For non-Metal platforms
    let stubBuffer: UnsafeMutableRawPointer?
    let stubSize: Int
    
    #if canImport(Metal)
    init(buffer: MTLBuffer) {
        self.buffer = buffer
        self.stubBuffer = nil
        self.stubSize = 0
    }
    #endif
    
    init(stubBuffer: UnsafeMutableRawPointer?, size: Int) {
        #if canImport(Metal)
        self.buffer = nil
        #endif
        self.stubBuffer = stubBuffer
        self.stubSize = size
    }
    
    deinit {
        if let stub = stubBuffer {
            stub.deallocate()
        }
    }
}

class MTLCommandQueueWrapper {
    #if canImport(Metal)
    let queue: MTLCommandQueue
    
    init(queue: MTLCommandQueue) {
        self.queue = queue
    }
    #else
    init() {}
    #endif
}