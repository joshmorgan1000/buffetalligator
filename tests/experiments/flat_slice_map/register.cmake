if(NOT PROJECT_NAME STREQUAL "alligator" OR NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    return()
endif()
enable_testing()
function(prepare_flat_map_abseil)
    set(CMAKE_POLICY_DEFAULT_CMP0109 NEW)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT supported LANGUAGES C CXX)
    if(supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
    set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    add_subdirectory("${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/abseil"
        "${CMAKE_BINARY_DIR}/experiment-abseil" EXCLUDE_FROM_ALL)
endfunction()
prepare_flat_map_abseil()
function(register_flat_slice_map_experiment)
    set(experiment "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_library(flat_slice_map_candidate STATIC "${experiment}/flat_slice_map.cpp")
    target_link_libraries(flat_slice_map_candidate PUBLIC alligator::alligator
        PRIVATE absl::flat_hash_map)
    foreach(program IN ITEMS flat_slice_map_test flat_slice_map_benchmark)
        add_executable(${program} "${experiment}/${program}.cpp")
        target_link_libraries(${program} PRIVATE flat_slice_map_candidate)
    endforeach()
    add_executable(slice_map_test "${PROJECT_SOURCE_DIR}/tests/slice_map_test.cpp")
    target_include_directories(slice_map_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(slice_map_test PRIVATE alligator::alligator)
    foreach(check IN ITEMS flat_slice_map_test slice_map_test)
        add_test(NAME ${check} COMMAND ${check})
        set_tests_properties(${check} PROPERTIES TIMEOUT 120
            ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
    endforeach()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.mm"
        "${PROJECT_SOURCE_DIR}/src/*.c" "${PROJECT_SOURCE_DIR}/src/*.h"
        "${PROJECT_SOURCE_DIR}/include/*.hpp" "${PROJECT_SOURCE_DIR}/include/*.h"
        "${PROJECT_SOURCE_DIR}/cmake/*.cmake" "${PROJECT_SOURCE_DIR}/cmake/*.cmake.in"
        "${PROJECT_SOURCE_DIR}/cmake/patches/*.patch" "${experiment}/*.cpp" "${experiment}/*.hpp"
        "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/abseil/absl/*.h"
        "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/abseil/absl/*.cc")
    list(APPEND sources "${PROJECT_SOURCE_DIR}/CMakeLists.txt" "${PROJECT_SOURCE_DIR}/run_build.sh"
        "${experiment}/register.cmake" "${PROJECT_SOURCE_DIR}/tests/benchmarks/benchmark_support.hpp"
        "${PROJECT_SOURCE_DIR}/tests/functional_support.hpp" "${PROJECT_SOURCE_DIR}/tests/slice_map_test.cpp"
        "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/abseil/CMakeLists.txt")
    file(WRITE "${CMAKE_BINARY_DIR}/flat-map-sources.json" "{\n  \"source_sha256\": {\n")
    set(separator "")
    foreach(source IN LISTS sources)
        file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
        file(SHA256 "${source}" digest)
        file(APPEND "${CMAKE_BINARY_DIR}/flat-map-sources.json"
            "${separator}    \"${relative}\": \"${digest}\"")
        set(separator ",\n")
    endforeach()
    file(APPEND "${CMAKE_BINARY_DIR}/flat-map-sources.json" "\n  }\n}\n")
    message(STATUS "FlatSliceMap experiment enabled with repository-pinned Abseil; production is unchanged")
endfunction()
cmake_language(DEFER CALL register_flat_slice_map_experiment)
