/**
 * @brief Main library implementation file
 * 
 * @details This file exists to satisfy CMake's requirement for at least one
 * source file in a library target. The actual implementation is in the
 * Swift and bridge object files.
 */

// Dummy symbol to ensure the library isn't empty
extern "C" {
    const char* cswift_version() {
        return "1.0.0";
    }
}