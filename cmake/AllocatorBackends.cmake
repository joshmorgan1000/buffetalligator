# Vulkan is not optional: it loads and registers on machines without a GPU and simply
# reports no device at runtime (VulkanKernel::available()).
if(APPLE)
    set(BUFFETALLIGATOR_DEFAULT_VULKAN_RUNTIME "${BUFFETALLIGATOR_DEPS_DIR}/MoltenVK/lib/libMoltenVK.dylib")
else()
    set(BUFFETALLIGATOR_DEFAULT_VULKAN_RUNTIME "${BUFFETALLIGATOR_DEPS_DIR}/vulkan-loader/lib/libvulkan.so.1")
endif()
set(BUFFETALLIGATOR_VULKAN_RUNTIME "${BUFFETALLIGATOR_DEFAULT_VULKAN_RUNTIME}" CACHE FILEPATH "Vulkan runtime library")
get_filename_component(BUFFETALLIGATOR_VULKAN_FILENAME "${BUFFETALLIGATOR_VULKAN_RUNTIME}" NAME)
if(NOT EXISTS "${BUFFETALLIGATOR_VULKAN_RUNTIME}")
    message(FATAL_ERROR "Vulkan runtime is missing; run ./run_build.sh to prepare it")
endif()
add_library(alligator::vulkan SHARED IMPORTED GLOBAL)
set_target_properties(alligator::vulkan PROPERTIES
    IMPORTED_LOCATION "${BUFFETALLIGATOR_VULKAN_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${BUFFETALLIGATOR_DEPS_DIR}/vulkan-headers/include")
add_library(alligator::shaderc STATIC IMPORTED GLOBAL)
set_target_properties(alligator::shaderc PROPERTIES
    IMPORTED_LOCATION "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/shaderc/build/libshaderc/libshaderc_combined.a"
    INTERFACE_INCLUDE_DIRECTORIES "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/shaderc/libshaderc/include")
target_sources(alligator PRIVATE src/vulkan/vulkan.cpp src/vulkan/vulkankernel.cpp)
target_include_directories(alligator PRIVATE "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/shaderc/libshaderc/include")
target_compile_definitions(alligator PUBLIC BUFFETALLIGATOR_HAS_VULKAN=1)
target_link_libraries(alligator PUBLIC alligator::vulkan PRIVATE alligator::shaderc)
file(REAL_PATH "${BUFFETALLIGATOR_VULKAN_RUNTIME}" BUFFETALLIGATOR_VULKAN_REAL_RUNTIME)
install(FILES "${BUFFETALLIGATOR_VULKAN_REAL_RUNTIME}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/alligator
    RENAME "${BUFFETALLIGATOR_VULKAN_FILENAME}")
install(FILES "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/shaderc/build/libshaderc/libshaderc_combined.a"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/alligator)
install(DIRECTORY "${BUFFETALLIGATOR_DEPS_DIR}/vulkan-headers/include/vulkan"
    "${BUFFETALLIGATOR_DEPS_DIR}/vulkan-headers/include/vk_video" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
