if(NOT PROJECT_NAME STREQUAL "alligator" OR TARGET buffetalligator_metal_glsl_gate)
    return()
endif()
if(NOT APPLE)
    message(FATAL_ERROR "The native Metal translation experiment requires an Apple host")
endif()
enable_language(OBJCXX)
set(gate_cross_root "${CMAKE_SOURCE_DIR}/deps/src/MoltenVK/External/SPIRV-Cross")
file(READ "${CMAKE_SOURCE_DIR}/deps/src/MoltenVK/ExternalRevisions/SPIRV-Cross_repo_revision"
    gate_cross_revision)
string(STRIP "${gate_cross_revision}" gate_cross_revision)
execute_process(COMMAND git -C "${gate_cross_root}" rev-parse HEAD
    OUTPUT_VARIABLE gate_cross_checkout OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE gate_cross_revision_result)
if(NOT gate_cross_revision_result EQUAL 0 OR NOT gate_cross_checkout STREQUAL gate_cross_revision)
    message(FATAL_ERROR "The Metal gate requires the SPIRV-Cross revision pinned by MoltenVK")
endif()
add_library(buffetalligator_gate_cross STATIC
    "${gate_cross_root}/spirv_cross.cpp"
    "${gate_cross_root}/spirv_parser.cpp"
    "${gate_cross_root}/spirv_cross_parsed_ir.cpp"
    "${gate_cross_root}/spirv_cfg.cpp"
    "${gate_cross_root}/spirv_glsl.cpp"
    "${gate_cross_root}/spirv_msl.cpp")
target_include_directories(buffetalligator_gate_cross PUBLIC "${gate_cross_root}")
target_compile_features(buffetalligator_gate_cross PUBLIC cxx_std_20)
add_executable(buffetalligator_metal_glsl_gate "${CMAKE_CURRENT_LIST_DIR}/metal_glsl_gate.mm")
set_target_properties(buffetalligator_metal_glsl_gate PROPERTIES OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(buffetalligator_metal_glsl_gate PRIVATE -fobjc-arc
    -mmacosx-version-min=13.0 -Wall -Wextra -Wpedantic)
target_compile_definitions(buffetalligator_metal_glsl_gate PRIVATE
    ALLIGATOR_GATE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    ALLIGATOR_GATE_CROSS_REVISION="${gate_cross_revision}")
target_include_directories(buffetalligator_metal_glsl_gate PRIVATE
    "${CMAKE_SOURCE_DIR}/deps/src/shaderc/libshaderc/include")
target_link_libraries(buffetalligator_metal_glsl_gate PRIVATE alligator::alligator
    alligator::shaderc buffetalligator_gate_cross "-framework Metal" "-framework Foundation")
