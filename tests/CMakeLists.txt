file(GLOB test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
foreach(source IN LISTS test_sources)
    get_filename_component(name "${source}" NAME_WE)
    set(target buffetalligator_${name})
    add_executable(${target} "${source}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(${target} PRIVATE alligator::alligator)
    if(NOT name MATCHES "_functional_test$" AND NOT name STREQUAL "shader_slice_test"
            AND NOT name STREQUAL "backend_shader_test")
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 120)
    endif()
endforeach()
set_tests_properties(buffetalligator_vulkan_fastpath_test PROPERTIES TIMEOUT 120 LABELS "gpu;functional"
    SKIP_RETURN_CODE 77)
set_tests_properties(buffetalligator_fastpath_leak_test PROPERTIES TIMEOUT 300 LABELS "allocators;functional")
set_tests_properties(buffetalligator_gpu_select_test PROPERTIES TIMEOUT 30 LABELS "functional"
    ENVIRONMENT "ALLIGATOR_GPU_BACKEND=auto")
foreach(case IN ITEMS vulkan auto)
    add_test(NAME buffetalligator_gpu_select_${case} COMMAND buffetalligator_gpu_select_test ${case})
    set_tests_properties(buffetalligator_gpu_select_${case} PROPERTIES TIMEOUT 30 LABELS "gpu;functional"
        SKIP_RETURN_CODE 77 ENVIRONMENT "ALLIGATOR_GPU_BACKEND=auto")
endforeach()
set_tests_properties(buffetalligator_kitchen_order_test buffetalligator_worker_pool_idle_test
    buffetalligator_kitchen_shutdown_test
    PROPERTIES TIMEOUT 45 LABELS "kitchen;functional" ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
set(BUFFETALLIGATOR_TEST_VULKAN_BACKEND vulkan CACHE STRING
    "Backend selection for Vulkan regressions; cpu skips GPU work for CPU sanitizers")
set_property(CACHE BUFFETALLIGATOR_TEST_VULKAN_BACKEND PROPERTY STRINGS vulkan cpu)
function(add_functional_cases component)
    set(target buffetalligator_${component}_functional_test)
    foreach(case IN LISTS ARGN)
        add_test(NAME buffetalligator_${case} COMMAND ${target} ${case})
        set_tests_properties(buffetalligator_${case} PROPERTIES TIMEOUT 30 LABELS functional)
    endforeach()
endfunction()
add_functional_cases(slice typed_slices typed_lifecycle typed_views weak_slices resizes)
add_functional_cases(atomic native_atomics complex_atomics atomic_ownership atomic_exceptions
    concurrent_atomics atomic_wait_notify registry global_registry concurrent_registry registry_exceptions)
add_functional_cases(map map_rows map_merge map_lifetime map_boundaries concurrent_map map_shared_ownership pipeline)
add_functional_cases(simd id_search max_search min_search)
add_functional_cases(synchronization mutex)
set_tests_properties(buffetalligator_slice_map_growth_test PROPERTIES TIMEOUT 30
    LABELS "containers;functional")
add_test(NAME buffetalligator_installed_consumer
    COMMAND "${CMAKE_COMMAND}"
        "-DALLIGATOR_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DALLIGATOR_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DALLIGATOR_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/installed_consumer"
        "-DALLIGATOR_PACKAGE_CMAKEDIR=${CMAKE_INSTALL_LIBDIR}/cmake/alligator"
        "-DALLIGATOR_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
        "-DALLIGATOR_CONFIGURATION=$<CONFIG>"
        "-DALLIGATOR_GENERATOR=${CMAKE_GENERATOR}"
        "-DALLIGATOR_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DALLIGATOR_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DALLIGATOR_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DALLIGATOR_CONSUMER_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DALLIGATOR_EXPECTED_DEFINITIONS=$<JOIN:$<TARGET_GENEX_EVAL:alligator,$<TARGET_PROPERTY:alligator,INTERFACE_COMPILE_DEFINITIONS>>,|>"
        -P "${PROJECT_SOURCE_DIR}/cmake/VerifyInstalledConsumer.cmake")
set_tests_properties(buffetalligator_installed_consumer PROPERTIES TIMEOUT 300 LABELS packaging)
