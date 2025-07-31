cmake_minimum_required(VERSION 3.20)

# Examples for BuffetAlligator
add_executable(01_basic_buffers 01_basic_buffers.cpp)
target_link_libraries(01_basic_buffers PRIVATE buffet_alligator cswift)

add_executable(02_gpu_buffers 02_gpu_buffers.cpp)
target_link_libraries(02_gpu_buffers PRIVATE buffet_alligator cswift)

add_executable(03_network_buffers 03_network_buffers.cpp)
target_link_libraries(03_network_buffers PRIVATE buffet_alligator cswift)

add_executable(04_chain_buffers 04_chain_buffers.cpp)
target_link_libraries(04_chain_buffers PRIVATE buffet_alligator cswift)

add_executable(05_buffer_optimization 05_buffer_optimization.cpp)
target_link_libraries(05_buffer_optimization PRIVATE buffet_alligator cswift)

add_executable(06_swift_interop 06_swift_interop.cpp)
target_link_libraries(06_swift_interop PRIVATE buffet_alligator cswift)

add_executable(07_error_handling 07_error_handling.cpp)
target_link_libraries(07_error_handling PRIVATE buffet_alligator cswift)

add_executable(08_advanced_buffers 08_advanced_buffers.cpp)
target_link_libraries(08_advanced_buffers PRIVATE buffet_alligator cswift)

# Set properties for all examples
set(ALL_EXAMPLES
    01_basic_buffers
    02_gpu_buffers
    03_network_buffers
    04_chain_buffers
    05_buffer_optimization
    06_swift_interop
    07_error_handling
    08_advanced_buffers
)

foreach(example ${ALL_EXAMPLES})
    set_target_properties(${example} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    
    # Add include directories
    target_include_directories(${example} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/cswift/include
        ${ASIO_INCLUDE_DIR}
    )
    
    # Platform-specific settings
    if(APPLE)
        set_target_properties(${example} PROPERTIES
            XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        )
    endif()
endforeach()

# Copy any required resources
# (None needed for these examples)

message(STATUS "Examples configured:")
foreach(example ${ALL_EXAMPLES})
    message(STATUS "  - ${example}")
endforeach()