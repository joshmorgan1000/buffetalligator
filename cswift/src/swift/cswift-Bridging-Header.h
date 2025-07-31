#ifndef CSWIFT_BRIDGING_HEADER_H
#define CSWIFT_BRIDGING_HEADER_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Opaque handle type for Swift objects
 */
typedef void* CSHandle;

/**
 * @brief Error codes
 */
typedef enum {
    CS_SUCCESS = 0,
    CS_ERROR_OUT_OF_MEMORY = -1,
    CS_ERROR_INVALID_ARGUMENT = -2,
    CS_ERROR_BUFFER_OVERFLOW = -3,
    CS_ERROR_OPERATION_FAILED = -4,
    CS_ERROR_NOT_IMPLEMENTED = -5,
    CS_ERROR_CONNECTION_FAILED = -6,
    CS_ERROR_WRITE_FAILED = -7,
    CS_ERROR_READ_FAILED = -8,
    CS_ERROR_TIMEOUT = -9,
    CS_ERROR_CANCELLED = -10,
    CS_ERROR_NOT_FOUND = -11,
    CS_ERROR_INDEX_OUT_OF_BOUNDS = -12,
    CS_ERROR_PROCESSING_FAILED = -13,
    CS_ERROR_NULL_POINTER = -14
} CSError;

/**
 * @brief Error information structure
 */
typedef struct {
    CSError code;
    char message[256];
} CSErrorInfo;

#endif /* CSWIFT_BRIDGING_HEADER_H */