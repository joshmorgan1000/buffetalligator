# ByteBuffer example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/example_bytebuffer.cpp)
    add_executable(example_bytebuffer example_bytebuffer.cpp)
    target_link_libraries(example_bytebuffer PRIVATE cswift)
endif()

# Zero-copy network example
add_executable(example_zerocopy_network zerocopy_network_example.cpp)
target_link_libraries(example_zerocopy_network PRIVATE cswift)
set_target_properties(example_zerocopy_network PROPERTIES LINKER_LANGUAGE CXX)

# Phase 2 direct buffer example
add_executable(example_phase2_direct_buffer phase2_direct_buffer_example.cpp)
target_link_libraries(example_phase2_direct_buffer PRIVATE cswift)
set_target_properties(example_phase2_direct_buffer PROPERTIES LINKER_LANGUAGE CXX)

# Phase 3 Thunderbolt DMA example
add_executable(example_phase3_thunderbolt_dma phase3_thunderbolt_dma_example.cpp)
target_link_libraries(example_phase3_thunderbolt_dma PRIVATE cswift)
set_target_properties(example_phase3_thunderbolt_dma PROPERTIES LINKER_LANGUAGE CXX)





# MPSGraph example (only on Apple platforms)
if(APPLE)
    add_executable(example_mps_graph mps_graph_example.cpp)
    target_link_libraries(example_mps_graph PRIVATE cswift)
    set_target_properties(example_mps_graph PROPERTIES LINKER_LANGUAGE CXX)
    
    add_executable(example_mps_training mps_training_example.cpp)
    target_link_libraries(example_mps_training PRIVATE cswift)
    set_target_properties(example_mps_training PROPERTIES LINKER_LANGUAGE CXX)
    
    add_executable(example_mtl_buffer mtl_buffer_example.cpp)
    target_link_libraries(example_mtl_buffer PRIVATE cswift)
    set_target_properties(example_mtl_buffer PROPERTIES LINKER_LANGUAGE CXX)
endif()