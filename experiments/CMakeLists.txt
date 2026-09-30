# Experiments: standalone comparisons that settle a design before it enters src/.
# Opt in with -DBUFFETALLIGATOR_BUILD_EXPERIMENTS=ON; every *.cpp here becomes buffetalligator_<name>.
file(GLOB experiment_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# libdispatch ships with macOS; Linux gets it from the libdispatch-dev package when installed.
if(NOT APPLE)
    find_path(BUFFETALLIGATOR_DISPATCH_INCLUDE dispatch/dispatch.h)
    find_library(BUFFETALLIGATOR_DISPATCH_LIBRARY dispatch)
    if(NOT BUFFETALLIGATOR_DISPATCH_INCLUDE OR NOT BUFFETALLIGATOR_DISPATCH_LIBRARY)
        message(STATUS "Experiments: libdispatch not found, GCD variants skipped (apt install libdispatch-dev)")
    endif()
endif()
# Apple Clang needs Homebrew's libomp for OpenMP; ask brew where it lives when OpenMP_ROOT is unset.
if(APPLE AND NOT OpenMP_ROOT)
    find_program(BUFFETALLIGATOR_BREW brew)
    if(BUFFETALLIGATOR_BREW)
        execute_process(COMMAND ${BUFFETALLIGATOR_BREW} --prefix libomp
            OUTPUT_VARIABLE BUFFETALLIGATOR_LIBOMP_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(EXISTS "${BUFFETALLIGATOR_LIBOMP_PREFIX}/include/omp.h")
            set(OpenMP_ROOT "${BUFFETALLIGATOR_LIBOMP_PREFIX}")
        endif()
    endif()
endif()
find_package(OpenMP COMPONENTS CXX)
if(NOT OpenMP_CXX_FOUND)
    message(STATUS "Experiments: OpenMP not found, OpenMP variants skipped (brew install libomp)")
endif()
# Experiments link the Kitchen's source directly so they build even while the library does not.
add_library(buffetalligator_experiment_kitchen OBJECT ${PROJECT_SOURCE_DIR}/src/kitchen/kitchen.cpp)
target_include_directories(buffetalligator_experiment_kitchen PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${BUFFETALLIGATOR_DEPS_DIR}/concurrentqueue/include
)
target_compile_features(buffetalligator_experiment_kitchen PUBLIC cxx_std_20)
target_link_libraries(buffetalligator_experiment_kitchen PUBLIC logging::logging Threads::Threads)
foreach(source IN LISTS experiment_sources)
    get_filename_component(name "${source}" NAME_WE)
    set(target buffetalligator_${name})
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE buffetalligator_experiment_kitchen)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(NOT APPLE AND BUFFETALLIGATOR_DISPATCH_INCLUDE AND BUFFETALLIGATOR_DISPATCH_LIBRARY)
        target_include_directories(${target} PRIVATE ${BUFFETALLIGATOR_DISPATCH_INCLUDE})
        target_link_libraries(${target} PRIVATE ${BUFFETALLIGATOR_DISPATCH_LIBRARY})
        target_compile_definitions(${target} PRIVATE TASK_POOL_HAS_DISPATCH=1)
    endif()
    if(OpenMP_CXX_FOUND)
        target_link_libraries(${target} PRIVATE OpenMP::OpenMP_CXX)
    endif()
endforeach()
