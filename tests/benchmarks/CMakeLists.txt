file(GLOB benchmark_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM benchmark_sources "${CMAKE_CURRENT_SOURCE_DIR}/slicemapvsvecset_backup.cpp")
foreach(source IN LISTS benchmark_sources)
    get_filename_component(name "${source}" NAME_WE)
    set(target buffetalligator_${name})
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE alligator::alligator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
target_include_directories(buffetalligator_slicemapvsvecset
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
if(BUFFETALLIGATOR_BUILD_TESTS)
    add_test(NAME buffetalligator_map_comparison_contracts
        COMMAND buffetalligator_slicemapvsvecset --case contracts)
    set_tests_properties(buffetalligator_map_comparison_contracts PROPERTIES
        LABELS "containers;contracts" TIMEOUT 120)
endif()
