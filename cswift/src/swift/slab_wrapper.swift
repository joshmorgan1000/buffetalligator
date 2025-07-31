import Foundation

/**
 * @brief Wrapper for Swift-allocated memory to participate in donation architecture
 * 
 * This allows Swift buffers (Network.framework, Metal, etc.) to be wrapped in
 * C++ shared_ptr for zero-copy ownership sharing with bufferalligator Messages.
 */
class SlabWrapper {
    let data: UnsafeMutableRawPointer
    let size: Int
    let deallocator: (() -> Void)?
    
    init(data: UnsafeMutableRawPointer, size: Int, deallocator: (() -> Void)?) {
        self.data = data
        self.size = size
        self.deallocator = deallocator
    }
    
    deinit {
        deallocator?()
    }
}

/**
 * @brief Create a slab wrapper for donation architecture
 * 
 * @param data Pointer to Swift-allocated memory
 * @param size Size of the memory region
 * @param context Optional deallocator context
 * @return Opaque pointer to SlabWrapper
 */
@_cdecl("cswift_create_slab_wrapper")
func cswift_create_slab_wrapper(
    _ data: UnsafeMutableRawPointer,
    _ size: Int,
    _ context: UnsafeMutableRawPointer?
) -> OpaquePointer? {
    
    // If context is provided, it's a pointer to std::function<void(void*)>
    // For now, we'll handle the common cases directly
    let wrapper = SlabWrapper(data: data, size: size, deallocator: nil)
    return OpaquePointer(Unmanaged.passRetained(wrapper).toOpaque())
}

/**
 * @brief Retain a slab wrapper
 */
@_cdecl("cswift_slab_wrapper_retain")
func cswift_slab_wrapper_retain(_ wrapper: OpaquePointer) {
    _ = Unmanaged<SlabWrapper>.fromOpaque(UnsafeRawPointer(wrapper)).retain()
}

/**
 * @brief Release a slab wrapper
 */
@_cdecl("cswift_slab_wrapper_release")
func cswift_slab_wrapper_release(_ wrapper: OpaquePointer) {
    Unmanaged<SlabWrapper>.fromOpaque(UnsafeRawPointer(wrapper)).release()
}

/**
 * @brief Get data pointer from slab wrapper
 */
@_cdecl("cswift_slab_wrapper_get_data")
func cswift_slab_wrapper_get_data(_ wrapper: OpaquePointer) -> UnsafeMutableRawPointer {
    let slabWrapper = Unmanaged<SlabWrapper>.fromOpaque(UnsafeRawPointer(wrapper)).takeUnretainedValue()
    return slabWrapper.data
}

/**
 * @brief Get size from slab wrapper
 */
@_cdecl("cswift_slab_wrapper_get_size")
func cswift_slab_wrapper_get_size(_ wrapper: OpaquePointer) -> Int {
    let slabWrapper = Unmanaged<SlabWrapper>.fromOpaque(UnsafeRawPointer(wrapper)).takeUnretainedValue()
    return slabWrapper.size
}

// MARK: - Integration with existing buffers


#if canImport(Metal)
import Metal

extension MTLBuffer {
    /**
     * @brief Create a slab wrapper for Metal buffer to participate in donation
     * 
     * Metal buffers with shared storage mode can be zero-copy shared between
     * CPU and GPU, making them perfect for the donation architecture.
     */
    func createSlabWrapper() -> OpaquePointer? {
        let contents = contents()
        
        let wrapper = SlabWrapper(
            data: contents,
            size: length,
            deallocator: { [self] in
                // Metal buffer is reference counted, just keep it alive
                _ = self
            }
        )
        
        return OpaquePointer(Unmanaged.passRetained(wrapper).toOpaque())
    }
}
#endif

// MARK: - Network buffer integration
// Note: CSNIOByteBuffer is a C++ class, so we can't extend it in Swift.
// The bridge function below handles the conversion.

/**
 * @brief Bridge function to get slab wrapper from ByteBuffer
 */
@_cdecl("cswift_nio_bytebuffer_get_slab_wrapper")
func cswift_nio_bytebuffer_get_slab_wrapper(_ handle: OpaquePointer) -> OpaquePointer? {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return buffer.createSlabWrapper()
}