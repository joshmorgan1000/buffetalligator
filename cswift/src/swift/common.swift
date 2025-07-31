import Foundation

// Error codes matching C definitions
let CS_SUCCESS: Int32 = 0
let CS_ERROR_OUT_OF_MEMORY: Int32 = -1
let CS_ERROR_INVALID_ARGUMENT: Int32 = -2
let CS_ERROR_BUFFER_OVERFLOW: Int32 = -3
let CS_ERROR_OPERATION_FAILED: Int32 = -4
let CS_ERROR_NOT_IMPLEMENTED: Int32 = -5
let CS_ERROR_CONNECTION_FAILED: Int32 = -6
let CS_ERROR_WRITE_FAILED: Int32 = -7
let CS_ERROR_READ_FAILED: Int32 = -8
let CS_ERROR_TIMEOUT: Int32 = -9
let CS_ERROR_CANCELLED: Int32 = -10
let CS_ERROR_NOT_FOUND: Int32 = -11
let CS_ERROR_INDEX_OUT_OF_BOUNDS: Int32 = -12
let CS_ERROR_PROCESSING_FAILED: Int32 = -13
let CS_ERROR_NULL_POINTER: Int32 = -14
let CS_ERROR_NOT_SUPPORTED: Int32 = -15

// Helper extensions for pointer conversions
extension OpaquePointer {
    func toRawPointer() -> UnsafeRawPointer {
        return UnsafeRawPointer(self)
    }
}

extension UnsafeMutableRawPointer {
    func toOpaquePointer() -> OpaquePointer {
        return OpaquePointer(self)
    }
}