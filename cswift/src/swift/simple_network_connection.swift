import Foundation
import Network

/**
 * @brief Simple network connection - replaces SwiftNIO NIOTSConnectionChannel
 * 
 * Uses Apple's Network.framework directly for high-performance networking
 * without SwiftNIO dependency complexity.
 */
@available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *)
class SimpleNetworkConnection {
    private var connection: NWConnection?
    private let host: String
    private let port: UInt16
    private let enableTLS: Bool
    private var isActive: Bool = false
    private let queue = DispatchQueue(label: "cswift.network", qos: .userInitiated)
    
    init(host: String, port: UInt16, enableTLS: Bool = false) {
        self.host = host
        self.port = port
        self.enableTLS = enableTLS
    }
    
    func connect() throws {
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(integerLiteral: port))
        
        let parameters: NWParameters
        if enableTLS {
            parameters = .tls
        } else {
            parameters = .tcp
        }
        
        connection = NWConnection(to: endpoint, using: parameters)
        
        connection?.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                self?.isActive = true
            case .failed(_), .cancelled:
                self?.isActive = false
            default:
                break
            }
        }
        
        connection?.start(queue: queue)
        
        // Wait for connection to be ready (simplified synchronous approach)
        let semaphore = DispatchSemaphore(value: 0)
        var connectionError: Error? = nil
        
        connection?.stateUpdateHandler = { state in
            switch state {
            case .ready:
                self.isActive = true
                semaphore.signal()
            case .failed(let error):
                connectionError = error
                semaphore.signal()
            case .cancelled:
                connectionError = NSError(domain: "Connection", code: -1, userInfo: [NSLocalizedDescriptionKey: "Connection cancelled"])
                semaphore.signal()
            default:
                break
            }
        }
        
        // Wait up to 30 seconds for connection
        let result = semaphore.wait(timeout: .now() + 30)
        if result == .timedOut {
            throw NSError(domain: "Connection", code: -1, userInfo: [NSLocalizedDescriptionKey: "Connection timeout"])
        }
        
        if let error = connectionError {
            throw error
        }
    }
    
    func close() {
        connection?.cancel()
        isActive = false
    }
    
    func write(data: Data) -> Int {
        guard isActive, let connection = connection else { return -1 }
        
        let semaphore = DispatchSemaphore(value: 0)
        var bytesWritten = 0
        
        connection.send(content: data, completion: .contentProcessed { error in
            if error != nil {
                bytesWritten = -1
            } else {
                bytesWritten = data.count
            }
            semaphore.signal()
        })
        
        _ = semaphore.wait(timeout: .now() + 10)
        return bytesWritten
    }
    
    func read(maxBytes: Int = 4096) -> Data? {
        guard isActive, let connection = connection else { return nil }
        
        let semaphore = DispatchSemaphore(value: 0)
        var receivedData: Data? = nil
        
        connection.receive(minimumIncompleteLength: 1, maximumLength: maxBytes) { data, _, isComplete, error in
            if let data = data {
                receivedData = data
            }
            semaphore.signal()
        }
        
        let result = semaphore.wait(timeout: .now() + 10)
        return result == .success ? receivedData : nil
    }
    
    var active: Bool {
        return isActive
    }
}

// MARK: - C Bridge Functions

@_cdecl("cswift_nio_ts_connection_channel_create")
public func cswift_nio_ts_connection_channel_create(
    host: UnsafePointer<CChar>,
    port: UInt16,
    enableTLS: Int32,
    error: UnsafeMutablePointer<CSErrorInfo>?
) -> OpaquePointer? {
    let hostString = String(cString: host)
    let tlsEnabled = enableTLS != 0
    
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = SimpleNetworkConnection(host: hostString, port: port, enableTLS: tlsEnabled)
        return OpaquePointer(Unmanaged.passRetained(connection).toOpaque())
    } else {
        error?.pointee.code = CSError(rawValue: CS_ERROR_NOT_SUPPORTED)
        error?.pointee.message.0 = 0 // Empty string
        return nil
    }
}

@_cdecl("cswift_nio_ts_connection_channel_destroy")
public func cswift_nio_ts_connection_channel_destroy(handle: OpaquePointer) {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<SimpleNetworkConnection>.fromOpaque(UnsafeRawPointer(handle))
        connection.release()
    }
}

@_cdecl("cswift_nio_ts_connection_channel_connect")
public func cswift_nio_ts_connection_channel_connect(
    handle: OpaquePointer,
    error: UnsafeMutablePointer<CSErrorInfo>?
) -> CSError {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<SimpleNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        
        do {
            try connection.connect()
            return CSError(rawValue: CS_SUCCESS)
        } catch let connectError {
            error?.pointee.code = CSError(rawValue: CS_ERROR_CONNECTION_FAILED)
            let errorMessage = connectError.localizedDescription
            _ = errorMessage.withCString { cString in
                strncpy(&error!.pointee.message.0, cString, 255)
            }
            return CSError(rawValue: CS_ERROR_CONNECTION_FAILED)
        }
    } else {
        return CSError(rawValue: CS_ERROR_NOT_SUPPORTED)
    }
}

@_cdecl("cswift_nio_ts_connection_channel_close")
public func cswift_nio_ts_connection_channel_close(
    handle: OpaquePointer,
    error: UnsafeMutablePointer<CSErrorInfo>?
) -> CSError {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<SimpleNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        connection.close()
        return CSError(rawValue: CS_SUCCESS)
    } else {
        return CSError(rawValue: CS_ERROR_NOT_SUPPORTED)
    }
}

@_cdecl("cswift_nio_ts_connection_channel_write")
public func cswift_nio_ts_connection_channel_write(
    handle: OpaquePointer,
    data: UnsafeRawPointer?,
    length: Int,
    error: UnsafeMutablePointer<CSErrorInfo>?
) -> Int {
    guard let data = data else { return -1 }
    
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<SimpleNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        let swiftData = Data(bytes: data, count: length)
        return connection.write(data: swiftData)
    } else {
        return -1
    }
}

@_cdecl("cswift_nio_ts_connection_channel_read")
public func cswift_nio_ts_connection_channel_read(
    handle: OpaquePointer,
    buffer: UnsafeMutableRawPointer?,
    bufferSize: Int,
    error: UnsafeMutablePointer<CSErrorInfo>?
) -> Int {
    guard let buffer = buffer else { return -1 }
    
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<SimpleNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        
        guard let data = connection.read(maxBytes: bufferSize) else { return 0 }
        
        let bytesToCopy = min(data.count, bufferSize)
        _ = data.withUnsafeBytes { bytes in
            memcpy(buffer, bytes.baseAddress!, bytesToCopy)
        }
        
        return bytesToCopy
    } else {
        return -1
    }
}

@_cdecl("cswift_nio_ts_connection_channel_is_active")
public func cswift_nio_ts_connection_channel_is_active(handle: OpaquePointer) -> Int32 {
    if #available(macOS 10.14, iOS 12.0, tvOS 12.0, watchOS 5.0, *) {
        let connection = Unmanaged<SimpleNetworkConnection>.fromOpaque(UnsafeRawPointer(handle)).takeUnretainedValue()
        return connection.active ? 1 : 0
    } else {
        return 0
    }
}