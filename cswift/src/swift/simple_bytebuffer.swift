import Foundation

/**
 * @brief Simple ByteBuffer view abstraction - replaces SwiftNIO dependency
 * 
 * Provides zero-copy byte buffer view operations. Memory allocation is handled
 * by the specific Substrate implementation:
 * - InMemory substrate: simple Swift Data
 * - TCP substrate: network buffer pools
 * - Thunderbolt DMA substrate: hardware-mapped regions
 * - GPU substrate: Metal unified memory
 * 
 * This class just provides the view interface, not allocation strategy.
 */
class SimpleByteBuffer: @unchecked Sendable {
    private var storage: Data
    internal var readerIndex: Int = 0  // Make internal for C bridge access
    internal var writerIndex: Int = 0  // Make internal for C bridge access
    private let minimumCapacity: Int = 64 * 1024 * 1024  // 64MB minimum for bufferalligator compatibility
    
    init(capacity: Int = 256) {
        let actualCapacity = max(capacity, minimumCapacity)
        storage = Data(count: actualCapacity)
        readerIndex = 0
        writerIndex = 0
    }
    
    /**
     * @brief Create ByteBuffer view over existing memory (substrate-allocated)
     * @param memory Pointer to memory allocated by substrate
     * @param size Size of the memory region
     * @param deallocator Optional custom deallocator for substrate memory
     */
    init(wrapping memory: UnsafeMutableRawPointer, size: Int, deallocator: Data.Deallocator = .free) {
        storage = Data(bytesNoCopy: memory, count: size, deallocator: deallocator)
        readerIndex = 0
        writerIndex = 0
    }
    
    var readableBytes: Int {
        return writerIndex - readerIndex
    }
    
    var writableBytes: Int {
        return storage.count - writerIndex
    }
    
    var capacity: Int {
        return storage.count
    }
    
    /**
     * @brief Create a slab wrapper for donation architecture integration
     * 
     * This allows the ByteBuffer's memory to participate in bufferalligator's 
     * memory management without copying.
     */
    func createSlabWrapper() -> OpaquePointer? {
        // Get a pointer to the underlying storage
        return storage.withUnsafeMutableBytes { bytes in
            guard let baseAddress = bytes.baseAddress else { return nil }
            
            // Create wrapper that keeps this buffer alive
            let wrapper = SlabWrapper(
                data: baseAddress,
                size: bytes.count,
                deallocator: { [self] in
                    // Keep self alive to prevent deallocation
                    _ = self
                }
            )
            
            return OpaquePointer(Unmanaged.passRetained(wrapper).toOpaque())
        }
    }
    
    func clear() {
        readerIndex = 0
        writerIndex = 0
    }
    
    func moveReaderIndex(by offset: Int) {
        let newIndex = readerIndex + offset
        guard newIndex >= 0 && newIndex <= writerIndex else {
            return
        }
        readerIndex = newIndex
    }
    
    func moveWriterIndex(by offset: Int) {
        let newIndex = writerIndex + offset
        guard newIndex >= readerIndex && newIndex <= storage.count else {
            return
        }
        writerIndex = newIndex
    }
    
    func discardReadBytes() {
        if readerIndex > 0 {
            let keepBytes = writerIndex - readerIndex
            if keepBytes > 0 {
                storage.replaceSubrange(0..<keepBytes, 
                                      with: storage[readerIndex..<writerIndex])
            }
            writerIndex = keepBytes
            readerIndex = 0
        }
    }
    
    func writeBytes(from buffer: UnsafeRawPointer, count: Int) -> Bool {
        guard count > 0 else { return true } // Handle zero-length writes
        
        if writableBytes < count {
            // Expand storage if needed
            let newCapacity = max(storage.count * 2, storage.count + count)
            storage.append(Data(count: newCapacity - storage.count))
        }
        
        storage.withUnsafeMutableBytes { bytes in
            let destination = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: writerIndex)
            memcpy(destination, buffer, count)
        }
        writerIndex += count
        return true
    }
    
    func readBytes(to buffer: UnsafeMutableRawPointer, count: Int) -> Int {
        let bytesToRead = min(count, readableBytes)
        guard bytesToRead > 0 else { return 0 }
        
        storage.withUnsafeBytes { bytes in
            let source = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: readerIndex)
            memcpy(buffer, source, bytesToRead)
        }
        readerIndex += bytesToRead
        return bytesToRead
    }
    
    func getBytes(at index: Int, to buffer: UnsafeMutableRawPointer, count: Int) -> Bool {
        guard index >= 0 && index + count <= writerIndex else {
            return false
        }
        
        storage.withUnsafeBytes { bytes in
            let source = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: index)
            memcpy(buffer, source, count)
        }
        return true
    }
    
    func setBytes(at index: Int, from buffer: UnsafeRawPointer, count: Int) -> Bool {
        guard index >= 0 && index + count <= storage.count else {
            return false
        }
        
        storage.withUnsafeMutableBytes { bytes in
            let destination = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: index)
            memcpy(destination, buffer, count)
        }
        
        // Update writer index if we wrote beyond it
        if index + count > writerIndex {
            writerIndex = index + count
        }
        
        return true
    }
    
    func readPointer() -> (UnsafeRawPointer?, Int) {
        let availableBytes = readableBytes
        guard availableBytes > 0 else { return (nil, 0) }
        
        return storage.withUnsafeBytes { bytes in
            let pointer = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: readerIndex)
            return (UnsafeRawPointer(pointer), availableBytes)
        }
    }
    
    func writePointer() -> (UnsafeMutableRawPointer?, Int) {
        let availableBytes = writableBytes
        guard availableBytes > 0 else { return (nil, 0) }
        
        return storage.withUnsafeMutableBytes { bytes in
            let pointer = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: writerIndex)
            return (UnsafeMutableRawPointer(pointer), availableBytes)
        }
    }
    
    func writeWithUnsafeMutableBytes<T>(_ body: (UnsafeMutableRawPointer, Int) throws -> T) rethrows -> T {
        let availableBytes = writableBytes
        return try storage.withUnsafeMutableBytes { bytes in
            let pointer = bytes.bindMemory(to: UInt8.self).baseAddress!.advanced(by: writerIndex)
            return try body(UnsafeMutableRawPointer(pointer), availableBytes)
        }
    }
}

// MARK: - C Bridge Functions

@_cdecl("cswift_nio_bytebuffer_allocate")
public func cswift_nio_bytebuffer_allocate(capacity: Int32) -> OpaquePointer? {
    let buffer = SimpleByteBuffer(capacity: Int(capacity))
    return OpaquePointer(Unmanaged.passRetained(buffer).toOpaque())
}

@_cdecl("cswift_nio_bytebuffer_destroy")
public func cswift_nio_bytebuffer_destroy(handle: OpaquePointer) {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle))
    buffer.release()
}

@_cdecl("cswift_nio_bytebuffer_readable_bytes")
public func cswift_nio_bytebuffer_readable_bytes(handle: OpaquePointer) -> Int32 {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return Int32(buffer.readableBytes)
}

@_cdecl("cswift_nio_bytebuffer_writable_bytes")
public func cswift_nio_bytebuffer_writable_bytes(handle: OpaquePointer) -> Int32 {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return Int32(buffer.writableBytes)
}

@_cdecl("cswift_nio_bytebuffer_capacity")
public func cswift_nio_bytebuffer_capacity(handle: OpaquePointer) -> Int32 {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return Int32(buffer.capacity)
}

@_cdecl("cswift_nio_bytebuffer_clear")
public func cswift_nio_bytebuffer_clear(handle: OpaquePointer) {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    buffer.clear()
}

@_cdecl("cswift_nio_bytebuffer_reader_index")
public func cswift_nio_bytebuffer_reader_index(handle: OpaquePointer) -> Int32 {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return Int32(buffer.readerIndex)
}

@_cdecl("cswift_nio_bytebuffer_writer_index")
public func cswift_nio_bytebuffer_writer_index(handle: OpaquePointer) -> Int32 {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return Int32(buffer.writerIndex)
}

@_cdecl("cswift_nio_bytebuffer_move_reader_index")
public func cswift_nio_bytebuffer_move_reader_index(handle: OpaquePointer, offset: Int32) {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    buffer.moveReaderIndex(by: Int(offset))
}

@_cdecl("cswift_nio_bytebuffer_move_writer_index")
public func cswift_nio_bytebuffer_move_writer_index(handle: OpaquePointer, offset: Int32) {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    buffer.moveWriterIndex(by: Int(offset))
}

@_cdecl("cswift_nio_bytebuffer_discard_read_bytes")
public func cswift_nio_bytebuffer_discard_read_bytes(handle: OpaquePointer) {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    buffer.discardReadBytes()
}

@_cdecl("cswift_nio_bytebuffer_write_bytes")
public func cswift_nio_bytebuffer_write_bytes(
    handle: OpaquePointer,
    bytes: UnsafeRawPointer?,
    length: Int
) -> CSError {
    // Allow nullptr for zero-length writes
    guard let bytes = bytes else { 
        return length == 0 ? CSError(rawValue: CS_SUCCESS) : CSError(rawValue: CS_ERROR_NULL_POINTER)
    }
    
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return buffer.writeBytes(from: bytes, count: length) 
        ? CSError(rawValue: CS_SUCCESS) 
        : CSError(rawValue: CS_ERROR_OPERATION_FAILED)
}

@_cdecl("cswift_nio_bytebuffer_read_bytes")
public func cswift_nio_bytebuffer_read_bytes(
    handle: OpaquePointer,
    bytes: UnsafeMutableRawPointer?,
    length: Int
) -> CSError {
    // Allow nullptr for zero-length reads
    guard let bytes = bytes else { 
        return length == 0 ? CSError(rawValue: CS_SUCCESS) : CSError(rawValue: CS_ERROR_NULL_POINTER)
    }
    
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    let bytesRead = buffer.readBytes(to: bytes, count: length)
    return bytesRead >= 0 ? CSError(rawValue: CS_SUCCESS) : CSError(rawValue: CS_ERROR_OPERATION_FAILED)
}

@_cdecl("cswift_nio_bytebuffer_get_bytes")
public func cswift_nio_bytebuffer_get_bytes(
    handle: OpaquePointer,
    index: Int32,
    bytes: UnsafeMutableRawPointer?,
    length: Int
) -> CSError {
    guard let bytes = bytes else { return CSError(rawValue: CS_ERROR_NULL_POINTER) }
    
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return buffer.getBytes(at: Int(index), to: bytes, count: length)
        ? CSError(rawValue: CS_SUCCESS)
        : CSError(rawValue: CS_ERROR_OPERATION_FAILED)
}

@_cdecl("cswift_nio_bytebuffer_set_bytes")
public func cswift_nio_bytebuffer_set_bytes(
    handle: OpaquePointer,
    index: Int32,
    bytes: UnsafeRawPointer?,
    length: Int
) -> CSError {
    guard let bytes = bytes else { return CSError(rawValue: CS_ERROR_NULL_POINTER) }
    
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    return buffer.setBytes(at: Int(index), from: bytes, count: length)
        ? CSError(rawValue: CS_SUCCESS)
        : CSError(rawValue: CS_ERROR_OPERATION_FAILED)
}

@_cdecl("cswift_nio_bytebuffer_read_pointer")
public func cswift_nio_bytebuffer_read_pointer(
    handle: OpaquePointer,
    length: UnsafeMutablePointer<Int>?
) -> UnsafeRawPointer? {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    let (pointer, count) = buffer.readPointer()
    length?.pointee = count
    return pointer
}

@_cdecl("cswift_nio_bytebuffer_write_pointer")
public func cswift_nio_bytebuffer_write_pointer(
    handle: OpaquePointer,
    length: UnsafeMutablePointer<Int>?
) -> UnsafeMutableRawPointer? {
    let buffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
    let (pointer, count) = buffer.writePointer()
    length?.pointee = count
    return pointer
}