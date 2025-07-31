enable_testing()

# Find or download Catch2
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    Include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.4.0
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# Test executable
add_executable(cswift_tests
    test_main.cpp
    test_bytebuffer.cpp
    test_bytebuffer_comprehensive.cpp
    test_zerocopy_network.cpp
    test_phase2_direct_buffer.cpp
    test_phase3_thunderbolt_dma.cpp
)

target_link_libraries(cswift_tests
    PRIVATE
    cswift
    Catch2::Catch2WithMain
)

target_compile_features(cswift_tests PRIVATE cxx_std_20)
set_target_properties(cswift_tests PROPERTIES LINKER_LANGUAGE CXX)

# Register tests with CTest
include(Catch)
catch_discover_tests(cswift_tests)

# Real usage benchmark
add_executable(test_real_usage_benchmark
    test_real_usage_benchmark.cpp
)

target_link_libraries(test_real_usage_benchmark
    PRIVATE
    cswift
)

target_compile_features(test_real_usage_benchmark PRIVATE cxx_std_20)
set_target_properties(test_real_usage_benchmark PROPERTIES LINKER_LANGUAGE CXX)

# Allocation comparison benchmark
add_executable(test_allocation_comparison
    test_allocation_comparison.cpp
)

target_link_libraries(test_allocation_comparison
    PRIVATE
    cswift
)

target_compile_features(test_allocation_comparison PRIVATE cxx_std_20)
set_target_properties(test_allocation_comparison PROPERTIES LINKER_LANGUAGE CXX)