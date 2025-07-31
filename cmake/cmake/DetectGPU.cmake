# DetectGPU.cmake - Automatic GPU backend detection for Psyne
# This module detects available GPU backends and sets appropriate flags

include(CheckLanguage)
include(CheckIncludeFileCXX)
include(CheckLibraryExists)

# Initialize all GPU flags to OFF
set(PSYNE_METAL_ENABLED OFF)
set(PSYNE_CUDA_ENABLED OFF)
set(PSYNE_VULKAN_ENABLED OFF)
set(PSYNE_GPU_BACKEND "None")

# Detect Apple Metal (macOS only)
if(APPLE)
    # Check for Metal framework
    find_library(METAL_FRAMEWORK Metal)
    find_library(MPS_FRAMEWORK MetalPerformanceShaders)
    
    if(METAL_FRAMEWORK AND MPS_FRAMEWORK)
        message(STATUS "Metal framework found")
        set(PSYNE_METAL_ENABLED ON)
        set(PSYNE_GPU_BACKEND "Metal")
        
        # Check if we're on Apple Silicon
        execute_process(
            COMMAND sysctl -n hw.optional.arm64
            OUTPUT_VARIABLE ARM64_SUPPORTED
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        if(ARM64_SUPPORTED STREQUAL "1")
            message(STATUS "Apple Silicon detected - enabling unified memory optimizations")
            add_definitions(-DPSYNE_APPLE_SILICON)
        endif()
    endif()
endif()

# Detect NVIDIA CUDA
if(NOT APPLE)  # CUDA not supported on macOS
    # First try CMake's built-in CUDA detection
    check_language(CUDA)
    
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(PSYNE_CUDA_ENABLED ON)
        set(PSYNE_GPU_BACKEND "CUDA")
        
        # Detect CUDA compute capability
        include(FindCUDA/select_compute_arch)
        cuda_select_nvcc_arch_flags(CUDA_ARCH_FLAGS Auto)
        message(STATUS "CUDA compiler found: ${CMAKE_CUDA_COMPILER}")
        message(STATUS "CUDA architectures: ${CUDA_ARCH_FLAGS}")
        
        # Find cuBLAS and cuDNN for enhanced performance
        find_library(CUBLAS_LIBRARY cublas 
            HINTS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
        )
        find_library(CUDNN_LIBRARY cudnn
            HINTS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
        )
        
        if(CUBLAS_LIBRARY)
            message(STATUS "cuBLAS found: ${CUBLAS_LIBRARY}")
            set(PSYNE_CUBLAS_ENABLED ON)
        endif()
        
        if(CUDNN_LIBRARY)
            message(STATUS "cuDNN found: ${CUDNN_LIBRARY}")
            set(PSYNE_CUDNN_ENABLED ON)
        endif()
    else()
        # Fallback to manual CUDA detection
        find_package(CUDA QUIET)
        if(CUDA_FOUND)
            message(STATUS "CUDA Toolkit found: ${CUDA_VERSION}")
            set(PSYNE_CUDA_ENABLED ON)
            set(PSYNE_GPU_BACKEND "CUDA")
        endif()
    endif()
endif()

# Detect Vulkan (cross-platform)
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan found: ${Vulkan_VERSION}")
    set(PSYNE_VULKAN_ENABLED ON)
    set(PSYNE_GPU_BACKEND "Vulkan")
    
    # Check for Vulkan compute support
    check_include_file_cxx("vulkan/vulkan.h" HAVE_VULKAN_H)
    if(HAVE_VULKAN_H)
        # Detect GPU vendor for optimizations
        # This would need a small detection program, simplified here
        message(STATUS "Vulkan compute support available")
    endif()
endif()

# Detect AMD ROCm (for AMD GPUs)
if(NOT APPLE AND NOT PSYNE_CUDA_ENABLED)
    find_package(ROCm QUIET)
    if(ROCm_FOUND)
        message(STATUS "AMD ROCm found: ${ROCm_VERSION}")
        # For now, use Vulkan backend for AMD GPUs
        if(PSYNE_VULKAN_ENABLED)
            message(STATUS "Using Vulkan backend for AMD GPU")
            set(PSYNE_AMD_GPU ON)
        endif()
    endif()
endif()

# Detect Intel oneAPI (for Intel GPUs)
if(NOT APPLE AND NOT PSYNE_CUDA_ENABLED)
    find_package(IntelSYCL QUIET)
    if(IntelSYCL_FOUND)
        message(STATUS "Intel oneAPI found")
        # For now, use Vulkan backend for Intel GPUs
        if(PSYNE_VULKAN_ENABLED)
            message(STATUS "Using Vulkan backend for Intel GPU")
            set(PSYNE_INTEL_GPU ON)
        endif()
    endif()
endif()

# Summary message
message(STATUS "=== Psyne GPU Backend Detection Summary ===")
message(STATUS "Primary GPU Backend: ${PSYNE_GPU_BACKEND}")
message(STATUS "Metal Enabled: ${PSYNE_METAL_ENABLED}")
message(STATUS "CUDA Enabled: ${PSYNE_CUDA_ENABLED}")
message(STATUS "Vulkan Enabled: ${PSYNE_VULKAN_ENABLED}")

# Export configuration
set(PSYNE_GPU_BACKENDS "")
if(PSYNE_METAL_ENABLED)
    list(APPEND PSYNE_GPU_BACKENDS "Metal")
endif()
if(PSYNE_CUDA_ENABLED)
    list(APPEND PSYNE_GPU_BACKENDS "CUDA")
endif()
if(PSYNE_VULKAN_ENABLED)
    list(APPEND PSYNE_GPU_BACKENDS "Vulkan")
endif()

# Set compile definitions
if(PSYNE_METAL_ENABLED)
    add_definitions(-DPSYNE_METAL_ENABLED)
endif()
if(PSYNE_CUDA_ENABLED)
    add_definitions(-DPSYNE_CUDA_ENABLED)
endif()
if(PSYNE_VULKAN_ENABLED)
    add_definitions(-DPSYNE_VULKAN_ENABLED)
endif()

# Function to get optimal GPU flags for current platform
function(bufferalligator_get_gpu_compile_flags OUT_VAR)
    set(FLAGS "")
    
    if(PSYNE_METAL_ENABLED)
        # Metal-specific optimizations
        if(PSYNE_APPLE_SILICON)
            list(APPEND FLAGS "-DPSYNE_UNIFIED_MEMORY_OPTIMAL")
        endif()
    elseif(PSYNE_CUDA_ENABLED)
        # CUDA-specific optimizations
        list(APPEND FLAGS ${CUDA_ARCH_FLAGS})
        if(PSYNE_CUBLAS_ENABLED)
            list(APPEND FLAGS "-DPSYNE_CUBLAS_ENABLED")
        endif()
        if(PSYNE_CUDNN_ENABLED)
            list(APPEND FLAGS "-DPSYNE_CUDNN_ENABLED")
        endif()
    elseif(PSYNE_VULKAN_ENABLED)
        # Vulkan-specific optimizations
        if(PSYNE_AMD_GPU)
            list(APPEND FLAGS "-DPSYNE_AMD_GPU_OPTIMIZATIONS")
        elseif(PSYNE_INTEL_GPU)
            list(APPEND FLAGS "-DPSYNE_INTEL_GPU_OPTIMIZATIONS")
        endif()
    endif()
    
    set(${OUT_VAR} ${FLAGS} PARENT_SCOPE)
endfunction()

# Function to get GPU libraries to link
function(bufferalligator_get_gpu_libraries OUT_VAR)
    set(LIBS "")
    
    if(PSYNE_METAL_ENABLED)
        list(APPEND LIBS ${METAL_FRAMEWORK} ${MPS_FRAMEWORK})
    elseif(PSYNE_CUDA_ENABLED)
        if(CMAKE_CUDA_COMPILER)
            list(APPEND LIBS CUDA::cudart)
            if(PSYNE_CUBLAS_ENABLED)
                list(APPEND LIBS CUDA::cublas)
            endif()
        else()
            list(APPEND LIBS ${CUDA_LIBRARIES})
            if(CUBLAS_LIBRARY)
                list(APPEND LIBS ${CUBLAS_LIBRARY})
            endif()
        endif()
        if(CUDNN_LIBRARY)
            list(APPEND LIBS ${CUDNN_LIBRARY})
        endif()
    elseif(PSYNE_VULKAN_ENABLED)
        list(APPEND LIBS Vulkan::Vulkan)
    endif()
    
    set(${OUT_VAR} ${LIBS} PARENT_SCOPE)
endfunction()