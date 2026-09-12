set(audit_allocation_definitions
    malloc=ba_audit_malloc calloc=ba_audit_calloc realloc=ba_audit_realloc
    aligned_alloc=ba_audit_aligned_alloc)
add_library(buffetalligator_audit_core OBJECT
    ../../src/core/ba_core.c ../../src/core/ba_worker.c ../../src/core/ba_queue.c
    ../../src/network/ba_wire.c ../../src/network/ba_network.c ../../src/network/ba_fabric.c)
target_compile_definitions(buffetalligator_audit_core PRIVATE ${audit_allocation_definitions}
    ALLIGATOR_SLOT_BITS=${ALLIGATOR_SLOT_BITS})
target_include_directories(buffetalligator_audit_core PRIVATE ../../src)
target_link_libraries(buffetalligator_audit_core PRIVATE alligator::uv alligator::sodium alligator::fabric)
add_library(buffetalligator_audit_os OBJECT ../../src/core/ba_os.c)
target_compile_definitions(buffetalligator_audit_os PRIVATE ${audit_allocation_definitions}
    ba_os_map=ba_audit_os_map_real ba_os_commit=ba_audit_os_commit_real)
add_executable(buffetalligator_audit_contracts contracts.cpp probe.c
    $<TARGET_OBJECTS:buffetalligator_audit_core> $<TARGET_OBJECTS:buffetalligator_audit_os>)
target_include_directories(buffetalligator_audit_contracts PRIVATE ../../src)
target_link_libraries(buffetalligator_audit_contracts PRIVATE alligator::alligator
    alligator::uv alligator::sodium alligator::fabric)
foreach(audit_case prepared_claims cross_thread_lifetime plain_wire encrypted_wire
    channel_allocations queue_identity typed_count)
    add_test(NAME audit_${audit_case} COMMAND buffetalligator_audit_contracts ${audit_case})
    set_tests_properties(audit_${audit_case} PROPERTIES LABELS "arena_audit" TIMEOUT 60)
    set_property(TEST audit_${audit_case} APPEND PROPERTY ENVIRONMENT
        "BUFFETALLIGATOR_TEST_NAME=audit_${audit_case}")
endforeach()
add_executable(buffetalligator_audit_performance performance.cpp)
target_include_directories(buffetalligator_audit_performance PRIVATE ../../src)
target_link_libraries(buffetalligator_audit_performance PRIVATE alligator::alligator)
add_executable(buffetalligator_audit_network_performance network_performance.cpp)
target_link_libraries(buffetalligator_audit_network_performance PRIVATE alligator::alligator)
add_executable(buffetalligator_audit_vulkan_performance vulkan_performance.cpp)
target_link_libraries(buffetalligator_audit_vulkan_performance PRIVATE alligator::alligator)
add_library(buffetalligator_audit_vulkan OBJECT ../../src/memory/ba_vulkan.c)
target_compile_definitions(buffetalligator_audit_vulkan PRIVATE vkCreateDevice=ba_audit_create_device)
target_link_libraries(buffetalligator_audit_vulkan PRIVATE alligator::vulkan alligator::uv)
add_executable(buffetalligator_audit_vulkan_contract vulkan_contract.cpp
    $<TARGET_OBJECTS:buffetalligator_audit_vulkan>)
target_include_directories(buffetalligator_audit_vulkan_contract PRIVATE ../../src)
target_link_libraries(buffetalligator_audit_vulkan_contract PRIVATE alligator::alligator)
add_test(NAME audit_vulkan_device_write COMMAND buffetalligator_audit_vulkan_contract)
set_tests_properties(audit_vulkan_device_write PROPERTIES LABELS "arena_audit" TIMEOUT 60 SKIP_RETURN_CODE 77)
set_property(TEST audit_vulkan_device_write APPEND PROPERTY ENVIRONMENT
    "BUFFETALLIGATOR_TEST_NAME=audit_vulkan_device_write")
foreach(audit_target buffetalligator_audit_core buffetalligator_audit_os buffetalligator_audit_contracts
    buffetalligator_audit_performance buffetalligator_audit_network_performance buffetalligator_audit_vulkan_performance
    buffetalligator_audit_vulkan buffetalligator_audit_vulkan_contract)
    target_compile_options(${audit_target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()
