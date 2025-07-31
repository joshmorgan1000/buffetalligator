# DetectGPU.cmake - Detect available GPU support

# Check for Vulkan
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan support detected")
    set(VULKAN_ENABLED ON CACHE BOOL "Enable Vulkan support")
else()
    message(STATUS "Vulkan not found")
    set(VULKAN_ENABLED OFF CACHE BOOL "Enable Vulkan support")
endif()

# Check for CUDA
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA support detected")
    set(CUDA_ENABLED ON CACHE BOOL "Enable CUDA support")
else()
    message(STATUS "CUDA not found")
    set(CUDA_ENABLED OFF CACHE BOOL "Enable CUDA support")
endif()

# Check for Metal (Apple only)
if(APPLE)
    find_library(METAL_FRAMEWORK Metal)
    if(METAL_FRAMEWORK)
        message(STATUS "Metal support detected")
        set(METAL_ENABLED ON CACHE BOOL "Enable Metal support")
    else()
        set(METAL_ENABLED OFF CACHE BOOL "Enable Metal support")
    endif()
else()
    set(METAL_ENABLED OFF CACHE BOOL "Enable Metal support")
endif()