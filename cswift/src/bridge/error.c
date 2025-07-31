#include "cswift/error.h"
#include <string.h>
#include <stdio.h>

void cswift_error_set(CSErrorInfo* error, int code, const char* message) {
    if (!error) {
        return;
    }
    
    error->code = code;
    
    if (message) {
        snprintf(error->message, sizeof(error->message), "%s", message);
        error->message[sizeof(error->message) - 1] = '\0';
    } else {
        const char* default_msg = cswift_error_get_message((CSErrorCode)code);
        snprintf(error->message, sizeof(error->message), "%s", default_msg);
        error->message[sizeof(error->message) - 1] = '\0';
    }
}

const char* cswift_error_get_message(CSErrorCode code) {
    switch (code) {
        case CS_SUCCESS:
            return "Success";
        case CS_ERROR_NULL_POINTER:
            return "Null pointer error";
        case CS_ERROR_INVALID_ARGUMENT:
            return "Invalid argument";
        case CS_ERROR_OUT_OF_MEMORY:
            return "Out of memory";
        case CS_ERROR_NOT_FOUND:
            return "Not found";
        case CS_ERROR_OPERATION_FAILED:
            return "Operation failed";
        case CS_ERROR_NOT_IMPLEMENTED:
            return "Not implemented";
        case CS_ERROR_BUFFER_TOO_SMALL:
            return "Buffer too small";
        case CS_ERROR_INVALID_STATE:
            return "Invalid state";
        case CS_ERROR_TIMEOUT:
            return "Operation timed out";
        case CS_ERROR_PLATFORM_NOT_SUPPORTED:
            return "Platform not supported";
        default:
            return "Unknown error";
    }
}