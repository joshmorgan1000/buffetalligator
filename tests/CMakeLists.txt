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
set_tests_properties(buffetalligator_memory_tracker_test PROPERTIES TIMEOUT 30)
set_tests_properties(buffetalligator_worker_pool_idle_test PROPERTIES TIMEOUT 15 LABELS functional)
foreach(case IN ITEMS vulkan_shader_identity vulkan_reference_dispatch vulkan_job_dispatch
    vulkan_embedded_slices vulkan_dispatch_rounds vulkan_async_completion
    vulkan_program_roundtrip vulkan_coroutine_completion vulkan_region_spread)
    add_test(NAME buffetalligator_${case} COMMAND buffetalligator_shader_slice_test ${case})
    set_tests_properties(buffetalligator_${case} PROPERTIES TIMEOUT 30 LABELS "gpu;functional"
        SKIP_RETURN_CODE 77)
endforeach()
set_tests_properties(buffetalligator_vulkan_region_spread PROPERTIES TIMEOUT 60)
target_link_libraries(buffetalligator_shader_failure_test PRIVATE alligator::shaderc)
set_tests_properties(buffetalligator_shader_failure_test PROPERTIES TIMEOUT 60
    LABELS "gpu;functional" SKIP_RETURN_CODE 77)
set(BUFFETALLIGATOR_TEST_VULKAN_BACKEND vulkan CACHE STRING
    "Backend selection for Vulkan regressions; cpu skips GPU work for CPU sanitizers")
set_property(CACHE BUFFETALLIGATOR_TEST_VULKAN_BACKEND PROPERTY STRINGS vulkan cpu)
foreach(case IN ITEMS vulkan_shader_identity vulkan_reference_dispatch vulkan_job_dispatch
        vulkan_embedded_slices vulkan_dispatch_rounds vulkan_async_completion
        vulkan_program_roundtrip vulkan_coroutine_completion vulkan_region_spread
        shader_failure_test)
    set_tests_properties(buffetalligator_${case} PROPERTIES
        ENVIRONMENT "ALLIGATOR_GPU_BACKEND=${BUFFETALLIGATOR_TEST_VULKAN_BACKEND}")
endforeach()
if(TARGET buffetalligator_backend_shader_test)
    add_test(NAME buffetalligator_backend_vulkan_translated
        COMMAND buffetalligator_backend_shader_test translated)
    set_tests_properties(buffetalligator_backend_vulkan_translated PROPERTIES
        TIMEOUT 120 LABELS "gpu;functional" SKIP_RETURN_CODE 77
        ENVIRONMENT "ALLIGATOR_GPU_BACKEND=${BUFFETALLIGATOR_TEST_VULKAN_BACKEND}")
    if(BUFFETALLIGATOR_METAL_ENABLED AND NOT BUFFETALLIGATOR_TEST_VULKAN_BACKEND STREQUAL cpu)
        foreach(source IN ITEMS translated native)
            add_test(NAME buffetalligator_backend_metal_${source}
                COMMAND buffetalligator_backend_shader_test ${source})
            set_tests_properties(buffetalligator_backend_metal_${source} PROPERTIES
                TIMEOUT 120 LABELS "gpu;functional" SKIP_RETURN_CODE 77
                ENVIRONMENT "ALLIGATOR_GPU_BACKEND=metal;MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
        endforeach()
    endif()
endif()
foreach(target IN ITEMS buffetalligator_network_test buffetalligator_network_lifecycle_test
        buffetalligator_network_exit_test)
    target_link_libraries(${target} PRIVATE alligator::uv alligator::sodium)
endforeach()
set(BUFFETALLIGATOR_TEST_FABRIC_PROVIDER "" CACHE STRING "Explicit libfabric provider for networking tests")
if(BUFFETALLIGATOR_TEST_FABRIC_PROVIDER)
    set_tests_properties(buffetalligator_network_test buffetalligator_network_lifecycle_test
        PROPERTIES ENVIRONMENT "FI_PROVIDER=${BUFFETALLIGATOR_TEST_FABRIC_PROVIDER}")
endif()
function(add_functional_cases component)
    set(target buffetalligator_${component}_functional_test)
    foreach(case IN LISTS ARGN)
        add_test(NAME buffetalligator_${case} COMMAND ${target} ${case})
        set_tests_properties(buffetalligator_${case} PROPERTIES TIMEOUT 30 LABELS functional)
    endforeach()
endfunction()
add_functional_cases(slice typed_slices typed_views weak_slices)
add_functional_cases(atomic native_atomics complex_atomics atomic_ownership atomic_exceptions
    concurrent_atomics atomic_wait_notify registry global_registry concurrent_registry registry_exceptions)
add_functional_cases(synchronization mutex barrier_arrival barrier_reuse barrier_release)
add_functional_cases(map map_rows map_merge map_lifetime map_boundaries concurrent_map map_shared_ownership pipeline)
add_functional_cases(simd id_search max_search min_search)
add_functional_cases(vulkan_interop vulkan_typed_io vulkan_subslice_boundaries)
set_tests_properties(buffetalligator_vulkan_typed_io buffetalligator_vulkan_subslice_boundaries
    PROPERTIES LABELS "gpu;functional" SKIP_RETURN_CODE 77
    ENVIRONMENT "ALLIGATOR_GPU_BACKEND=${BUFFETALLIGATOR_TEST_VULKAN_BACKEND}")
set_tests_properties(buffetalligator_slice_map_growth_test PROPERTIES TIMEOUT 30
    LABELS "containers;functional")
set_tests_properties(buffetalligator_slice_region_test PROPERTIES TIMEOUT 120
    LABELS "allocators;functional" ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
set_tests_properties(buffetalligator_slice_core_regression_test PROPERTIES TIMEOUT 120
    LABELS "allocators;functional" ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
set_tests_properties(buffetalligator_global_slice_startup_test PROPERTIES TIMEOUT 30
    LABELS "allocators;functional" ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
set_tests_properties(buffetalligator_global_slice_lifetime_test PROPERTIES TIMEOUT 30
    LABELS "allocators;functional" ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
add_test(NAME buffetalligator_global_vulkan_lifetime
    COMMAND buffetalligator_global_slice_lifetime_test translated)
set_tests_properties(buffetalligator_global_vulkan_lifetime PROPERTIES TIMEOUT 60
    LABELS "gpu;functional" SKIP_RETURN_CODE 77
    ENVIRONMENT "ALLIGATOR_GPU_BACKEND=${BUFFETALLIGATOR_TEST_VULKAN_BACKEND}")
if(BUFFETALLIGATOR_METAL_ENABLED AND NOT BUFFETALLIGATOR_TEST_VULKAN_BACKEND STREQUAL cpu)
    foreach(mode IN ITEMS native late-vulkan)
        add_test(NAME buffetalligator_global_metal_${mode}_lifetime
            COMMAND buffetalligator_global_slice_lifetime_test ${mode})
        set_tests_properties(buffetalligator_global_metal_${mode}_lifetime PROPERTIES
            TIMEOUT 60 LABELS "gpu;functional" SKIP_RETURN_CODE 77
            ENVIRONMENT "ALLIGATOR_GPU_BACKEND=auto;MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
    endforeach()
endif()
set_tests_properties(buffetalligator_network_exit_test PROPERTIES TIMEOUT 30
    LABELS "network;functional")
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
