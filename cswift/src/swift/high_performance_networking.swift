import Foundation
import Network
import Dispatch

#if canImport(Metal)
import Metal
#endif

/**
 * @brief High-performance networking substrate for bufferalligator
 * 
 * Supports:
 * - Zero-copy local GPU coordination
 * - High-speed inter-host streaming (Thunderbolt, 10G+)
 * - Cross-platform Swift networking (macOS Network.framework + Linux Swift NIO)
 * - Direct integration with Metal unified memory
 */

// MARK: - Network Configuration

enum NetworkTransport: Int32 {
    case tcp = 0
    case udp = 1
    case thunderbolt = 2      // IP-over-Thunderbolt
    case rdma = 3             // Future: RDMA-style operations
    case local_gpu = 4        // Local GPU memory coordination
}

enum NetworkOptimization: Int32 {
    case throughput = 0       // Optimize for maximum bandwidth
    case latency = 1          // Optimize for minimum latency
    case gpu_pipeline = 2     // Optimize for GPU data flow
    case neural_engine = 3    // Optimize for Neural Engine coordination
}

struct NetworkEndpoint {
    let host: String
    let port: UInt16
    let transport: NetworkTransport
    let optimization: NetworkOptimization
}

// MARK: - High-Performance Network Connection

@available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *)
class HighPerformanceNetworkConnection {
    private var connection: NWConnection?
    private let endpoint: NetworkEndpoint
    private var isActive: Bool = false
    private let queue: DispatchQueue
    
    // GPU integration
    #if canImport(Metal)
    private var metalDevice: MTLDevice?
    private var metalCommandQueue: MTLCommandQueue?
    #endif
    
    // Zero-copy buffer management
    private var receiveBuffers: [SimpleByteBuffer] = []
    private var sendBuffers: [SimpleByteBuffer] = []
    private let bufferLock = NSLock()
    
    init(endpoint: NetworkEndpoint) {
        self.endpoint = endpoint
        
        // Create optimized dispatch queue based on use case
        let qos: DispatchQoS
        switch endpoint.optimization {
        case .latency, .neural_engine:
            qos = .userInteractive  // Highest priority
        case .gpu_pipeline:
            qos = .userInitiated    // High priority
        case .throughput:
            qos = .utility          // Balanced for throughput
        }
        
        self.queue = DispatchQueue(label: "bufferalligator.network.\(endpoint.host)", qos: qos)
        
        // Initialize Metal for GPU coordination
        #if canImport(Metal)
        if endpoint.optimization == .gpu_pipeline || endpoint.optimization == .neural_engine {
            self.metalDevice = MTLCreateSystemDefaultDevice()
            self.metalCommandQueue = metalDevice?.makeCommandQueue()
        }
        #endif
    }
    
    func connect() async throws {
        let nwEndpoint = NWEndpoint.hostPort(
            host: NWEndpoint.Host(endpoint.host), 
            port: NWEndpoint.Port(integerLiteral: endpoint.port)
        )
        
        let parameters = createOptimizedParameters()
        connection = NWConnection(to: nwEndpoint, using: parameters)
        
        return try await withCheckedThrowingContinuation { continuation in
            connection?.stateUpdateHandler = { state in
                switch state {
                case .ready:
                    self.isActive = true
                    continuation.resume()
                case .failed(let error):
                    continuation.resume(throwing: error)
                case .cancelled:
                    continuation.resume(throwing: NetworkError.connectionCancelled)
                default:
                    break
                }
            }
            
            connection?.start(queue: queue)
        }
    }
    
    private func createOptimizedParameters() -> NWParameters {
        let parameters: NWParameters
        
        switch endpoint.transport {
        case .tcp:
            parameters = .tcp
        case .udp:
            parameters = .udp
        case .thunderbolt:
            // Thunderbolt networking - use TCP with specific optimizations
            parameters = .tcp
            // Configure for high-bandwidth, low-latency Thunderbolt
            if let ipOptions = parameters.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
                ipOptions.version = .v6 // TB prefers IPv6
            }
        case .rdma:
            // Future: Custom protocol for RDMA-style operations
            parameters = .tcp
        case .local_gpu:
            // Local GPU memory coordination - use fastest local transport
            parameters = .tcp
        }
        
        // Apply optimization-specific tuning
        switch endpoint.optimization {
        case .throughput:
            configureThroughputOptimized(parameters)
        case .latency:
            configureLatencyOptimized(parameters)
        case .gpu_pipeline:
            configureGPUOptimized(parameters)
        case .neural_engine:
            configureNeuralEngineOptimized(parameters)
        }
        
        return parameters
    }
    
    private func configureThroughputOptimized(_ parameters: NWParameters) {
        // Maximize bandwidth - larger buffers, bulk transfers
        if let tcpOptions = parameters.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options {
            tcpOptions.noDelay = false          // Allow Nagle's algorithm for bulk data
            tcpOptions.enableKeepalive = true
            tcpOptions.keepaliveIdle = 7200     // 2 hours
        }
    }
    
    private func configureLatencyOptimized(_ parameters: NWParameters) {
        // Minimize latency - disable buffering, immediate sends
        if let tcpOptions = parameters.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options {
            tcpOptions.noDelay = true           // Disable Nagle's algorithm
            tcpOptions.enableKeepalive = true
            tcpOptions.keepaliveIdle = 1        // Fast detection of dead connections
        }
    }
    
    private func configureGPUOptimized(_ parameters: NWParameters) {
        // Optimize for GPU memory bandwidth patterns
        configureLatencyOptimized(parameters)  // Start with low latency base
        
        // Additional GPU-specific optimizations
        parameters.preferNoProxies = true      // Direct connection for best performance
        parameters.multipathServiceType = .interactive  // Prioritize this traffic
    }
    
    private func configureNeuralEngineOptimized(_ parameters: NWParameters) {
        // Optimize for Neural Engine data flows - very latency sensitive
        configureLatencyOptimized(parameters)
        parameters.multipathServiceType = .interactive
        parameters.serviceClass = .responsiveData  // Highest QoS
    }
    
    // MARK: - Zero-Copy Data Transfer
    
    func sendZeroCopy(buffer: SimpleByteBuffer) async throws -> Int {
        guard isActive, let connection = connection else {
            throw NetworkError.connectionNotActive
        }
        
        return try await withCheckedThrowingContinuation { continuation in
            // Get zero-copy pointer from buffer
            let (pointer, length) = buffer.readPointer()
            guard let dataPointer = pointer, length > 0 else {
                continuation.resume(throwing: NetworkError.invalidBuffer)
                return
            }
            
            // Create Data wrapper that doesn't copy
            let data = Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: dataPointer), 
                          count: length, 
                          deallocator: .none)  // Don't deallocate - buffer owns memory
            
            connection.send(content: data, completion: .contentProcessed { error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume(returning: length)
                }
            })
        }
    }
    
    func receiveZeroCopy(into buffer: SimpleByteBuffer, maxBytes: Int = 65536) async throws -> Int {
        guard isActive, let connection = connection else {
            throw NetworkError.connectionNotActive
        }
        
        return try await withCheckedThrowingContinuation { continuation in
            connection.receive(minimumIncompleteLength: 1, maximumLength: maxBytes) { data, _, isComplete, error in
                if let error = error {
                    continuation.resume(throwing: error)
                    return
                }
                
                guard let data = data, !data.isEmpty else {
                    continuation.resume(returning: 0)
                    return
                }
                
                // Zero-copy write into buffer
                // Note: This operation is synchronous within the closure
                let dataCount = data.count
                let bytesWritten = buffer.writeWithUnsafeMutableBytes { bufferPtr, availableSpace in
                    let bytesToCopy = min(dataCount, availableSpace)
                    _ = data.withUnsafeBytes { bytes in
                        memcpy(bufferPtr, bytes.baseAddress!, bytesToCopy)
                    }
                    return bytesToCopy
                }
                
                continuation.resume(returning: bytesWritten)
            }
        }
    }
    
    // MARK: - GPU Memory Integration
    
    #if canImport(Metal)
    func sendFromGPUBuffer(metalBuffer: MTLBuffer, offset: Int = 0, length: Int? = nil) async throws -> Int {
        guard let device = metalDevice, let cmdQueue = metalCommandQueue else {
            throw NetworkError.gpuNotAvailable
        }
        
        let transferLength = length ?? (metalBuffer.length - offset)
        
        // For shared/managed memory, we can send directly
        if metalBuffer.storageMode == .shared || metalBuffer.storageMode == .managed {
            let pointer = metalBuffer.contents().advanced(by: offset)
            let data = Data(bytesNoCopy: pointer, count: transferLength, deallocator: .none)
            
            return try await withCheckedThrowingContinuation { continuation in
                connection?.send(content: data, completion: .contentProcessed { error in
                    if let error = error {
                        continuation.resume(throwing: error)
                    } else {
                        continuation.resume(returning: transferLength)
                    }
                })
            }
        } else {
            // For private GPU memory, need to copy to shared buffer first
            guard let sharedBuffer = device.makeBuffer(length: transferLength, options: .storageModeShared) else {
                throw NetworkError.bufferAllocationFailed
            }
            
            // GPU-to-GPU copy
            guard let commandBuffer = cmdQueue.makeCommandBuffer(),
                  let blitEncoder = commandBuffer.makeBlitCommandEncoder() else {
                throw NetworkError.gpuCommandFailed
            }
            
            blitEncoder.copy(from: metalBuffer, sourceOffset: offset,
                           to: sharedBuffer, destinationOffset: 0,
                           size: transferLength)
            blitEncoder.endEncoding()
            
            return try await withCheckedThrowingContinuation { continuation in
                commandBuffer.addCompletedHandler { _ in
                    // Now send the shared buffer
                    let data = Data(bytesNoCopy: sharedBuffer.contents(), count: transferLength, deallocator: .none)
                    self.connection?.send(content: data, completion: .contentProcessed { error in
                        if let error = error {
                            continuation.resume(throwing: error)
                        } else {
                            continuation.resume(returning: transferLength)
                        }
                    })
                }
                commandBuffer.commit()
            }
        }
    }
    
    func receiveToGPUBuffer(metalBuffer: MTLBuffer, offset: Int = 0, maxBytes: Int? = nil) async throws -> Int {
        let receiveLength = maxBytes ?? (metalBuffer.length - offset)
        
        // Receive into shared memory first if buffer is private
        if metalBuffer.storageMode == .shared || metalBuffer.storageMode == .managed {
            // Direct receive into GPU buffer
            return try await withCheckedThrowingContinuation { continuation in
                // Get pointer inside the continuation to ensure buffer validity
                let pointer = metalBuffer.contents().advanced(by: offset)
                
                connection?.receive(minimumIncompleteLength: 1, maximumLength: receiveLength) { data, _, _, error in
                    if let error = error {
                        continuation.resume(throwing: error)
                        return
                    }
                    
                    guard let data = data, !data.isEmpty else {
                        continuation.resume(returning: 0)
                        return
                    }
                    
                    // Perform zero-copy transfer
                    let bytesReceived = data.count
                    _ = data.withUnsafeBytes { bytes in
                        memcpy(pointer, bytes.baseAddress!, bytesReceived)
                    }
                    
                    continuation.resume(returning: bytesReceived)
                }
            }
        } else {
            throw NetworkError.unsupportedGPUBuffer
        }
    }
    #endif
    
    func close() {
        connection?.cancel()
        isActive = false
    }
    
    var active: Bool {
        return isActive
    }
}

// MARK: - Network Error Types

enum NetworkError: Error {
    case connectionNotActive
    case connectionCancelled
    case invalidBuffer
    case bufferAllocationFailed
    case gpuNotAvailable
    case gpuCommandFailed
    case unsupportedGPUBuffer
}

// MARK: - C Bridge Functions

@_cdecl("cswift_network_connection_create")
public func cswift_network_connection_create(
    host: UnsafePointer<CChar>,
    port: UInt16,
    transport: Int32,
    optimization: Int32
) -> OpaquePointer? {
    let hostString = String(cString: host)
    
    guard let transportType = NetworkTransport(rawValue: transport),
          let optimizationType = NetworkOptimization(rawValue: optimization) else {
        return nil
    }
    
    let endpoint = NetworkEndpoint(
        host: hostString, 
        port: port, 
        transport: transportType, 
        optimization: optimizationType
    )
    
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = HighPerformanceNetworkConnection(endpoint: endpoint)
        return OpaquePointer(Unmanaged.passRetained(connection).toOpaque())
    } else {
        return nil
    }
}

@_cdecl("cswift_network_connection_destroy")
public func cswift_network_connection_destroy(handle: OpaquePointer) {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<HighPerformanceNetworkConnection>.fromOpaque(UnsafeRawPointer(handle))
        connection.release()
    }
}

@_cdecl("cswift_network_connection_connect")
public func cswift_network_connection_connect(handle: OpaquePointer) -> Int32 {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<HighPerformanceNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        
        let semaphore = DispatchSemaphore(value: 0)
        var result: Int32 = CS_ERROR_OPERATION_FAILED
        
        Task {
            do {
                try await connection.connect()
                result = CS_SUCCESS
            } catch {
                result = CS_ERROR_CONNECTION_FAILED
            }
            semaphore.signal()
        }
        
        semaphore.wait()
        return result
    } else {
        return CS_ERROR_NOT_SUPPORTED
    }
}

@_cdecl("cswift_network_send_zero_copy")
public func cswift_network_send_zero_copy(
    handle: OpaquePointer,
    buffer: OpaquePointer
) -> Int32 {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<HighPerformanceNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        let byteBuffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(buffer)).takeUnretainedValue()
        
        let semaphore = DispatchSemaphore(value: 0)
        var result: Int32 = 0
        
        Task {
            do {
                result = Int32(try await connection.sendZeroCopy(buffer: byteBuffer))
            } catch {
                result = -1
            }
            semaphore.signal()
        }
        
        semaphore.wait()
        return result
    } else {
        return -1
    }
}

@_cdecl("cswift_network_receive_zero_copy")
public func cswift_network_receive_zero_copy(
    handle: OpaquePointer,
    buffer: OpaquePointer,
    maxBytes: Int32
) -> Int32 {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<HighPerformanceNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        let byteBuffer = Unmanaged<SimpleByteBuffer>.fromOpaque(UnsafeRawPointer(buffer)).takeUnretainedValue()
        
        let semaphore = DispatchSemaphore(value: 0)
        var result: Int32 = 0
        
        Task {
            do {
                result = Int32(try await connection.receiveZeroCopy(into: byteBuffer, maxBytes: Int(maxBytes)))
            } catch {
                result = -1
            }
            semaphore.signal()
        }
        
        semaphore.wait()
        return result
    } else {
        return -1
    }
}

#if canImport(Metal)
@_cdecl("cswift_network_send_gpu_buffer")
public func cswift_network_send_gpu_buffer(
    handle: OpaquePointer,
    metalBuffer: OpaquePointer,
    offset: Int32,
    length: Int32
) -> Int32 {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<HighPerformanceNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        let buffer = Unmanaged<MTLBuffer>.fromOpaque(UnsafeRawPointer(metalBuffer)).takeUnretainedValue()
        
        let semaphore = DispatchSemaphore(value: 0)
        var result: Int32 = 0
        
        Task {
            do {
                let transferLength = length > 0 ? Int(length) : nil
                result = Int32(try await connection.sendFromGPUBuffer(metalBuffer: buffer, offset: Int(offset), length: transferLength))
            } catch {
                result = -1
            }
            semaphore.signal()
        }
        
        semaphore.wait()
        return result
    } else {
        return -1
    }
}
#endif

@_cdecl("cswift_network_is_active")
public func cswift_network_is_active(handle: OpaquePointer) -> Int32 {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<HighPerformanceNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        return connection.active ? 1 : 0
    } else {
        return 0
    }
}