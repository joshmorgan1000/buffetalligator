#ifndef CSWIFT_ERROR_H
#define CSWIFT_ERROR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Error codes for cswift library
 */
typedef enum {
    CS_SUCCESS = 0,
    CS_ERROR_NULL_POINTER = -1,
    CS_ERROR_INVALID_ARGUMENT = -2,
    CS_ERROR_OUT_OF_MEMORY = -3,
    CS_ERROR_NOT_FOUND = -4,
    CS_ERROR_OPERATION_FAILED = -5,
    CS_ERROR_NOT_IMPLEMENTED = -6,
    CS_ERROR_BUFFER_TOO_SMALL = -7,
    CS_ERROR_INVALID_STATE = -8,
    CS_ERROR_TIMEOUT = -9,
    CS_ERROR_PLATFORM_NOT_SUPPORTED = -10,
    CS_ERROR_NOT_SUPPORTED = -15
} CSErrorCode;

/**
 * @brief Error information structure
 */
typedef struct {
    int code;
    char message[256];
} CSErrorInfo;

/**
 * @brief Set error information
 * 
 * @param error Pointer to error info structure
 * @param code Error code
 * @param message Error message (optional, will use default if NULL)
 */
void cswift_error_set(CSErrorInfo* error, int code, const char* message);

/**
 * @brief Get default error message for error code
 * 
 * @param code Error code
 * @return Constant string with error message
 */
const char* cswift_error_get_message(CSErrorCode code);

#ifdef __cplusplus
}
#endif

#endif /* CSWIFT_ERROR_H */