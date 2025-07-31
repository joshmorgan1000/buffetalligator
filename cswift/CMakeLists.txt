cmake_minimum_required(VERSION 3.20)
project(cswift VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)
option(GENERATE_ASSEMBLY "Generate assembly output files" ON)

# Build configuration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Platform detection and Swift setup
if(APPLE)
    enable_language(Swift)
    set(CMAKE_Swift_LANGUAGE_VERSION 5)
    
    # Remove -incremental flag from Swift debug flags to avoid warnings
    string(REPLACE "-incremental" "" CMAKE_Swift_FLAGS_DEBUG "${CMAKE_Swift_FLAGS_DEBUG}")
    
    # Enable modern CBLAS interface to avoid deprecated function warnings
    set(CMAKE_Swift_FLAGS "${CMAKE_Swift_FLAGS} -Xcc -DACCELERATE_NEW_LAPACK -Xcc -DACCELERATE_LAPACK_ILP64")
    
    # macOS/iOS specific libraries
    find_library(FOUNDATION Foundation REQUIRED)
    find_library(METAL Metal)
    find_library(MPS MetalPerformanceShaders)
    find_library(COREML CoreML)
    find_library(NETWORK Network)
    find_library(ACCELERATE Accelerate)
    find_library(COMPRESSION compression)
    find_library(CRYPTOKIT CryptoKit)
    
    # Note: SwiftNIO dependency has been replaced with simple_bytebuffer.swift
    # This eliminates complex external dependencies while maintaining zero-copy performance
    set(SWIFT_NIO_FOUND TRUE)  # We have our own implementation now
    
    set(PLATFORM_LIBS ${FOUNDATION})
    if(METAL)
        list(APPEND PLATFORM_LIBS ${METAL})
    endif()
    if(MPS)
        list(APPEND PLATFORM_LIBS ${MPS})
    endif()
    if(COREML)
        list(APPEND PLATFORM_LIBS ${COREML})
    endif()
    if(NETWORK)
        list(APPEND PLATFORM_LIBS ${NETWORK})
    endif()
    if(ACCELERATE)
        list(APPEND PLATFORM_LIBS ${ACCELERATE})
    endif()
    if(COMPRESSION)
        list(APPEND PLATFORM_LIBS ${COMPRESSION})
    endif()
    
    # Swift compilation flags for Apple platforms
    # Don't reset SWIFT_PLATFORM_FLAGS - it may already contain SwiftNIO paths
    if(CMAKE_Swift_COMPILER_TARGET)
        list(APPEND SWIFT_PLATFORM_FLAGS -target ${CMAKE_Swift_COMPILER_TARGET})
    endif()
    if(CMAKE_OSX_SYSROOT)
        list(APPEND SWIFT_PLATFORM_FLAGS -sdk ${CMAKE_OSX_SYSROOT})
    endif()
    
    # Remove -fsanitize flags from Swift (not supported)
    string(REPLACE "-fsanitize=address" "" CMAKE_Swift_FLAGS_DEBUG "${CMAKE_Swift_FLAGS_DEBUG}")
    string(REPLACE "-fno-omit-frame-pointer" "" CMAKE_Swift_FLAGS_DEBUG "${CMAKE_Swift_FLAGS_DEBUG}")
    
elseif(UNIX AND NOT APPLE)
    # Linux Swift setup
    find_package(PkgConfig REQUIRED)
    
    # Check for Swift installation
    execute_process(
        COMMAND swift --print-target-info
        OUTPUT_VARIABLE SWIFT_TARGET_INFO
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE SWIFT_FOUND
    )
    
    if(SWIFT_FOUND EQUAL 0)
        enable_language(Swift)
        set(CMAKE_Swift_LANGUAGE_VERSION 5)
        
        # Parse Swift target info to get library paths
        string(REGEX MATCH "\"runtimeLibraryPaths\":\\[([^\\]]+)\\]" SWIFT_RUNTIME_MATCH ${SWIFT_TARGET_INFO})
        if(SWIFT_RUNTIME_MATCH)
            string(REGEX REPLACE "[\"\[\]]" "" SWIFT_RUNTIME_PATHS ${CMAKE_MATCH_1})
            string(REPLACE "," ";" SWIFT_RUNTIME_PATHS ${SWIFT_RUNTIME_PATHS})
        endif()
        
        # Linux-specific Swift libraries
        find_library(SWIFT_CORE swiftCore PATHS ${SWIFT_RUNTIME_PATHS})
        find_library(SWIFT_GLIBC swiftGlibc PATHS ${SWIFT_RUNTIME_PATHS})
        
        set(PLATFORM_LIBS)
        if(SWIFT_CORE)
            list(APPEND PLATFORM_LIBS ${SWIFT_CORE})
        endif()
        if(SWIFT_GLIBC)
            list(APPEND PLATFORM_LIBS ${SWIFT_GLIBC})
        endif()
        
        # Check for SwiftNIO (required)
        pkg_check_modules(SWIFT_NIO swift-nio)
        if(SWIFT_NIO_FOUND)
            list(APPEND PLATFORM_LIBS ${SWIFT_NIO_LIBRARIES})
        else()
            message(FATAL_ERROR "SwiftNIO not found on Linux! Please install SwiftNIO:\n"
                                "1. Install Swift for Linux from https://swift.org/download/\n"
                                "2. Ensure SwiftNIO is available in the system")
        endif()
        
        set(SWIFT_PLATFORM_FLAGS)
        if(CMAKE_Swift_COMPILER_TARGET)
            list(APPEND SWIFT_PLATFORM_FLAGS -target ${CMAKE_Swift_COMPILER_TARGET})
        endif()
        if(SWIFT_RUNTIME_PATHS)
            list(APPEND SWIFT_PLATFORM_FLAGS -Xlinker -rpath -Xlinker ${SWIFT_RUNTIME_PATHS})
        endif()
    else()
        message(FATAL_ERROR "Swift not found! Swift is required for cswift.\n"
                            "Please install Swift toolchain from https://swift.org/download/")
    endif()
    
elseif(WIN32)
    # Windows Swift setup
    find_program(SWIFT_EXECUTABLE swift)
    if(SWIFT_EXECUTABLE)
        enable_language(Swift)
        set(CMAKE_Swift_LANGUAGE_VERSION 5)
        
        # Windows-specific Swift libraries
        get_filename_component(SWIFT_DIR ${SWIFT_EXECUTABLE} DIRECTORY)
        get_filename_component(SWIFT_ROOT ${SWIFT_DIR} DIRECTORY)
        
        find_library(SWIFT_CORE swiftCore PATHS ${SWIFT_ROOT}/lib/swift/windows)
        find_library(SWIFT_WIN32 swiftWin32 PATHS ${SWIFT_ROOT}/lib/swift/windows)
        
        set(PLATFORM_LIBS)
        if(SWIFT_CORE)
            list(APPEND PLATFORM_LIBS ${SWIFT_CORE})
        endif()
        if(SWIFT_WIN32)
            list(APPEND PLATFORM_LIBS ${SWIFT_WIN32})
        endif()
        
        set(SWIFT_PLATFORM_FLAGS)
        if(CMAKE_Swift_COMPILER_TARGET)
            list(APPEND SWIFT_PLATFORM_FLAGS -target ${CMAKE_Swift_COMPILER_TARGET})
        endif()
    else()
        message(FATAL_ERROR "Swift not found! Swift is required for cswift.\n"
                            "Please install Swift for Windows from https://swift.org/download/")
    endif()
endif()

# Swift sources
file(GLOB_RECURSE SWIFT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/swift/*.swift)

if(SWIFT_SOURCES AND CMAKE_Swift_COMPILER)
    # Create Swift static library
    add_library(cswift_swift STATIC ${SWIFT_SOURCES})
    
    # Base Swift compilation flags (simplified for compatibility)
    set(SWIFT_COMPILE_FLAGS
        -parse-as-library
        -emit-library
        -emit-module
        -module-name CSwiftBridge
        -import-objc-header ${CMAKE_CURRENT_SOURCE_DIR}/src/swift/cswift-Bridging-Header.h
    )
    
    # Platform-specific Swift flags (skip if empty or problematic)
    if(DEFINED SWIFT_PLATFORM_FLAGS AND SWIFT_PLATFORM_FLAGS)
        message(STATUS "Adding Swift platform flags: ${SWIFT_PLATFORM_FLAGS}")
        list(APPEND SWIFT_COMPILE_FLAGS ${SWIFT_PLATFORM_FLAGS})
    else()
        message(STATUS "No Swift platform flags defined")
    endif()
    
    # Configuration-specific flags
    list(APPEND SWIFT_COMPILE_FLAGS
        $<$<CONFIG:Debug>:-Onone -g -DDEBUG>
        $<$<CONFIG:Release>:-O -whole-module-optimization -DNDEBUG>
    )
    
    target_compile_options(cswift_swift PRIVATE ${SWIFT_COMPILE_FLAGS})
    
    # Swift module properties
    set_target_properties(cswift_swift PROPERTIES
        Swift_MODULE_NAME CSwiftBridge
        Swift_MODULE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/swift_modules
        POSITION_INDEPENDENT_CODE ON
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    )
    
    # Create module directory
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/swift_modules)
    
    # Platform-specific linking
    if(APPLE)
        target_link_libraries(cswift_swift PRIVATE 
            "-framework Foundation"
            "-framework Network"
            "-framework Accelerate"
            "-lcompression"
        )
        
        # Optional frameworks
        if(METAL)
            target_link_libraries(cswift_swift PRIVATE "-framework Metal")
        endif()
        if(MPS)
            target_link_libraries(cswift_swift PRIVATE "-framework MetalPerformanceShaders")
        endif()
        if(COREML)
            target_link_libraries(cswift_swift PRIVATE "-framework CoreML")
        endif()
    endif()
endif()

# Bridge sources
file(GLOB_RECURSE BRIDGE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/*.c)
if(BRIDGE_SOURCES)
    add_library(cswift_bridge_objs OBJECT ${BRIDGE_SOURCES})
    set_target_properties(cswift_bridge_objs PROPERTIES 
        LINKER_LANGUAGE C
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(cswift_bridge_objs PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    
    # C compilation flags
    target_compile_options(cswift_bridge_objs PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:
            -Wall -Wextra -Werror
            $<$<CONFIG:Debug>:-O0 -g -DDEBUG>
            $<$<CONFIG:Release>:-O3 -DNDEBUG>
        >
        $<$<C_COMPILER_ID:MSVC>:
            /W4 /WX
            $<$<CONFIG:Debug>:/Od /Zi /DDEBUG>
            $<$<CONFIG:Release>:/O2 /DNDEBUG>
        >
    )
endif()

# C++ sources
file(GLOB_RECURSE CPP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/*.cpp)
if(CPP_SOURCES)
    add_library(cswift_cpp_objs OBJECT ${CPP_SOURCES})
    set_target_properties(cswift_cpp_objs PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    
    target_include_directories(cswift_cpp_objs PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    
    # C++ compilation flags
    target_compile_options(cswift_cpp_objs PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
            -Wall -Wextra -Wpedantic -Werror
            $<$<CONFIG:Debug>:-O0 -g -DDEBUG>
            $<$<CONFIG:Release>:-O3 -DNDEBUG>
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4 /WX
            $<$<CONFIG:Debug>:/Od /Zi /DDEBUG>
            $<$<CONFIG:Release>:/O2 /DNDEBUG>
        >
    )
endif()

# Create main library
add_library(cswift
    STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/cswift.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/telemetry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/federated.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/neural.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/nio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/metal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/high_performance_network.cpp
)

# Add C++ objects (these DO get added to the archive)
if(TARGET cswift_cpp_objs)
    target_sources(cswift PRIVATE $<TARGET_OBJECTS:cswift_cpp_objs>)
endif()

# Add bridge objects (these DO get added to the archive)
if(TARGET cswift_bridge_objs)
    target_sources(cswift PRIVATE $<TARGET_OBJECTS:cswift_bridge_objs>)
endif()

# Link Swift library (no external dependencies needed)
if(TARGET cswift_swift)
    target_link_libraries(cswift PUBLIC cswift_swift)
endif()

target_include_directories(cswift PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(cswift PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(PLATFORM_LIBS)
    target_link_libraries(cswift PRIVATE ${PLATFORM_LIBS})
endif()
# Add Swift runtime library directory (dynamic discovery)
if(APPLE AND CMAKE_Swift_COMPILER)
    # Use Swift compiler path to find runtime libraries (more portable than xcode-select)
    get_filename_component(SWIFT_TOOLCHAIN_DIR ${CMAKE_Swift_COMPILER} DIRECTORY)
    get_filename_component(SWIFT_TOOLCHAIN_DIR ${SWIFT_TOOLCHAIN_DIR} DIRECTORY)
    set(SWIFT_RUNTIME_DIR "${SWIFT_TOOLCHAIN_DIR}/lib/swift/macosx")
    
    # Fallback to xcode-select if compiler-based path doesn't exist
    if(NOT EXISTS ${SWIFT_RUNTIME_DIR})
        execute_process(
            COMMAND xcode-select -p
            OUTPUT_VARIABLE XCODE_DEVELOPER_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(SWIFT_RUNTIME_DIR "${XCODE_DEVELOPER_PATH}/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx")
    endif()
    
    message(STATUS "Using Swift runtime directory: ${SWIFT_RUNTIME_DIR}")
    
    if(EXISTS ${SWIFT_RUNTIME_DIR})
        target_link_directories(cswift PUBLIC ${SWIFT_RUNTIME_DIR})
        
        # Add rpath for Swift runtime
        set_target_properties(cswift PROPERTIES
            BUILD_RPATH "${SWIFT_RUNTIME_DIR}"
            INSTALL_RPATH "${SWIFT_RUNTIME_DIR}"
        )
        
        # Dynamically find and link compatibility libraries
        set(COMPAT_LIBS "libswiftCompatibility56.a" "libswiftCompatibilityConcurrency.a" "libswiftCompatibilityPacks.a")
        foreach(COMPAT_LIB ${COMPAT_LIBS})
            if(EXISTS "${SWIFT_RUNTIME_DIR}/${COMPAT_LIB}")
                target_link_libraries(cswift PUBLIC "-force_load ${SWIFT_RUNTIME_DIR}/${COMPAT_LIB}")
                message(STATUS "Linking compatibility library: ${COMPAT_LIB}")
            endif()
        endforeach()
    else()
        message(WARNING "Swift runtime directory not found: ${SWIFT_RUNTIME_DIR}")
    endif()
endif()

# Swift runtime linking (required when using Xcode toolchain)
if(APPLE AND CMAKE_Swift_COMPILER)
    # Find Swift runtime libraries
    get_filename_component(SWIFT_TOOLCHAIN_DIR ${CMAKE_Swift_COMPILER} DIRECTORY)
    get_filename_component(SWIFT_TOOLCHAIN_DIR ${SWIFT_TOOLCHAIN_DIR} DIRECTORY)
    
    # Find the SDK path for Swift runtime libraries
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    # Add Swift runtime libraries
    set(SWIFT_RUNTIME_LIBS
        ${SDK_PATH}/usr/lib/swift/libswiftCore.tbd
        ${SDK_PATH}/usr/lib/swift/libswiftFoundation.tbd
        ${SDK_PATH}/usr/lib/swift/libswiftMetal.tbd
        ${SDK_PATH}/usr/lib/swift/libswiftDarwin.tbd
        ${SDK_PATH}/usr/lib/swift/libswiftDispatch.tbd
        ${SDK_PATH}/usr/lib/swift/libswiftObjectiveC.tbd
        ${SDK_PATH}/usr/lib/swift/libswiftCoreFoundation.tbd
        ${SWIFT_TOOLCHAIN_DIR}/lib/swift/macosx/libswiftCompatibility56.a
        ${SWIFT_TOOLCHAIN_DIR}/lib/swift/macosx/libswiftCompatibilityConcurrency.a
        ${SWIFT_TOOLCHAIN_DIR}/lib/swift/macosx/libswiftCompatibilityPacks.a
    )
    
    # Filter to only existing libraries
    set(EXISTING_SWIFT_LIBS)
    foreach(LIB ${SWIFT_RUNTIME_LIBS})
        if(EXISTS ${LIB})
            list(APPEND EXISTING_SWIFT_LIBS ${LIB})
        endif()
    endforeach()
    
    if(EXISTING_SWIFT_LIBS)
        target_link_libraries(cswift PUBLIC ${EXISTING_SWIFT_LIBS})
    endif()
endif()

# Compiler flags - only for C++ sources
target_compile_options(cswift PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
            -Wall -Wextra -Wpedantic -Werror
            $<$<CONFIG:Debug>:-O0 -g -DDEBUG>
            $<$<CONFIG:Release>:-O3 -DNDEBUG>
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4 /WX
            $<$<CONFIG:Debug>:/Od /Zi /DDEBUG>
            $<$<CONFIG:Release>:/O2 /DNDEBUG>
        >
    >
)

# Sanitizers (only for C++, not Swift)
if(ENABLE_ASAN)
    target_compile_options(cswift PUBLIC 
        $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=address>
    )
    target_link_options(cswift PUBLIC -fsanitize=address)
endif()

if(ENABLE_TSAN)
    target_compile_options(cswift PUBLIC 
        $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=thread>
    )
    target_link_options(cswift PUBLIC -fsanitize=thread)
endif()

# Assembly generation
if(GENERATE_ASSEMBLY AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET cswift POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Generating assembly output..."
        COMMAND objdump -d $<TARGET_FILE:cswift> > ${CMAKE_CURRENT_BINARY_DIR}/cswift.s
        COMMENT "Generating assembly output file"
        VERBATIM
    )
endif()

# Installation
include(GNUInstallDirs)
install(TARGETS cswift
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Package configuration
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cswift-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cswift-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cswift
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cswift-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cswift-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cswift-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cswift
)

# Install assembly file if generated
if(GENERATE_ASSEMBLY AND CMAKE_BUILD_TYPE STREQUAL "Release")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cswift.s
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        OPTIONAL
    )
endif()

# Custom target for zero-copy checking
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/zero_copy_check.sh)
    add_custom_target(zero_copy_check
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/zero_copy_check.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking zero-copy compliance"
        VERBATIM
    )
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Print build configuration summary
message(STATUS "CSwift Configuration:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Swift Compiler: ${CMAKE_Swift_COMPILER}")
message(STATUS "  Swift Version: ${CMAKE_Swift_LANGUAGE_VERSION}")
message(STATUS "  Platform Libraries: ${PLATFORM_LIBS}")
message(STATUS "  Assembly Generation: ${GENERATE_ASSEMBLY}")
message(STATUS "  Tests: ${BUILD_TESTS}")
message(STATUS "  Examples: ${BUILD_EXAMPLES}")