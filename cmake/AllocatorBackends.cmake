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
if(APPLE)
    option(BUFFETALLIGATOR_ENABLE_METAL "Build the native Metal compute backend" ON)
    if(BUFFETALLIGATOR_ENABLE_METAL)
        enable_language(OBJCXX)
        set(BUFFETALLIGATOR_METAL_ENABLED ON)
        set(cross_source "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/MoltenVK/External/SPIRV-Cross")
        file(READ "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/MoltenVK/ExternalRevisions/SPIRV-Cross_repo_revision"
            cross_revision)
        string(STRIP "${cross_revision}" cross_revision)
        execute_process(COMMAND git -C "${cross_source}" rev-parse HEAD
            OUTPUT_VARIABLE cross_checkout OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE cross_revision_result)
        if(NOT cross_revision_result EQUAL 0 OR NOT cross_checkout STREQUAL cross_revision)
            message(FATAL_ERROR "Native Metal requires the SPIRV-Cross revision pinned by MoltenVK; run ./run_build.sh")
        endif()
        add_library(alligator_spirvcross STATIC
            "${cross_source}/spirv_cross.cpp"
            "${cross_source}/spirv_parser.cpp"
            "${cross_source}/spirv_cross_parsed_ir.cpp"
            "${cross_source}/spirv_cfg.cpp"
            "${cross_source}/spirv_glsl.cpp"
            "${cross_source}/spirv_msl.cpp")
        set_target_properties(alligator_spirvcross PROPERTIES EXPORT_NAME spirvcross
            POSITION_INDEPENDENT_CODE ON)
        target_compile_features(alligator_spirvcross PRIVATE cxx_std_20)
        target_include_directories(alligator_spirvcross PRIVATE "${cross_source}")
        target_include_directories(alligator PRIVATE "${cross_source}")
        target_sources(alligator PRIVATE src/memory/metal_allocator.mm src/metal/metal.mm)
        set_source_files_properties(src/memory/metal_allocator.mm src/metal/metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-mmacosx-version-min=13.0")
        set_target_properties(alligator PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_compile_definitions(alligator PUBLIC BUFFETALLIGATOR_HAS_METAL=1)
        target_link_libraries(alligator PRIVATE alligator_spirvcross
            "-framework Metal" "-framework Foundation")
        install(TARGETS alligator_spirvcross EXPORT alligatorTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/alligator)
        install(FILES "${cross_source}/LICENSE"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/alligator/SPIRV-Cross)
        install(DIRECTORY "${cross_source}/LICENSES"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/alligator/SPIRV-Cross)
        message(STATUS "Native Metal enabled with pinned SPIRV-Cross ${cross_revision}")
    endif()
endif()
