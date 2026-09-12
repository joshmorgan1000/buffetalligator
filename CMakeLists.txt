cmake_minimum_required(VERSION 3.20)
project(alligator VERSION 0.1.0 DESCRIPTION "Preallocated host-accessible memory slices" HOMEPAGE_URL "https://github.com/joshmorgan1000/buffetalligator" LANGUAGES C CXX)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed; run ./run_build.sh")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ALLIGATOR_SLOT_BITS 24 CACHE STRING "Bits reserved for backing slots in packed Slice metadata")
if(NOT ALLIGATOR_SLOT_BITS MATCHES "^[1-9][0-9]*$" OR ALLIGATOR_SLOT_BITS GREATER 31)
    message(FATAL_ERROR "ALLIGATOR_SLOT_BITS must be an integer between 1 and 31")
endif()
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(BUFFETALLIGATOR_BUILD_TESTS_DEFAULT ON)
else()
    set(BUFFETALLIGATOR_BUILD_TESTS_DEFAULT OFF)
endif()
option(BUFFETALLIGATOR_BUILD_TESTS "Build the BuffetAlligator tests" ${BUFFETALLIGATOR_BUILD_TESTS_DEFAULT})
set(BUFFETALLIGATOR_DEPS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/src" CACHE PATH "Dependency source directory")
set(BUFFETALLIGATOR_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Built dependency directory")
set(THREADSAFE_LOGGER_SOURCE_DIR "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/threadsafe-logger")
set(BUFFETALLIGATOR_OWNS_LOGGING_TARGET OFF)
if(NOT TARGET logging::logging)
    if(NOT EXISTS "${THREADSAFE_LOGGER_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "threadsafe-logger is missing; run ./run_build.sh to fetch and build it")
    endif()
    set(LOGGING_BUILD_TESTS OFF)
    add_subdirectory("${THREADSAFE_LOGGER_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/deps/threadsafe-logger")
    set(BUFFETALLIGATOR_OWNS_LOGGING_TARGET ON)
endif()
find_package(OpenSSL REQUIRED)
if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR "OpenSSL is required by BuffetAlligator and not vendored. Please install OpenSSL and try again.")
endif()
find_package(Threads REQUIRED)
include(cmake/NetworkDependencies.cmake)
add_library(alligator STATIC
    src/core/ba_os.c
    src/core/ba_core.c
    src/core/ba_worker.c
    src/core/ba_queue.c
    src/memory/slice.cpp
    src/memory/slice_queue.cpp
    src/memory/heapbuffet.cpp
    src/memory/ba_vulkan.c
    src/memory/vulkan.cpp
    src/network/ba_wire.c
    src/network/ba_network.c
    src/network/ba_fabric.c
    src/network/slice_channel.cpp
)
add_library(alligator::alligator ALIAS alligator)
target_include_directories(alligator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(alligator PUBLIC cxx_std_20)
target_compile_definitions(alligator PUBLIC ALLIGATOR_SLOT_BITS=${ALLIGATOR_SLOT_BITS})
target_link_libraries(alligator PUBLIC logging::logging Threads::Threads alligator::vulkan
    PRIVATE alligator::uv alligator::sodium alligator::fabric)
if(UNIX AND NOT APPLE)
    set_source_files_properties(src/network/ba_network.c src/network/ba_wire.c
        src/network/ba_fabric.c src/memory/ba_vulkan.c
        PROPERTIES COMPILE_DEFINITIONS _POSIX_C_SOURCE=200809L)
endif()
if(WIN32)
    target_link_libraries(alligator PUBLIC onecore psapi)
endif()
set_target_properties(alligator PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(alligator PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(alligator PRIVATE /W4 $<$<COMPILE_LANGUAGE:C>:/std:c11> $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
if(BUFFETALLIGATOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
set(BUFFETALLIGATOR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/alligator")
install(TARGETS alligator
    EXPORT alligatorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES include/alligator.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(BUFFETALLIGATOR_OWNS_LOGGING_TARGET)
    set(BUFFETALLIGATOR_INSTALLED_LOGGING_TARGETS "../logging/loggingTargets.cmake")
else()
    set(BUFFETALLIGATOR_INSTALLED_LOGGING_TARGETS "")
endif()
install(EXPORT alligatorTargets
    FILE alligatorTargets.cmake
    NAMESPACE alligator::
    DESTINATION ${BUFFETALLIGATOR_INSTALL_CMAKEDIR}
)
configure_package_config_file(
    cmake/alligatorConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/alligatorConfig.cmake"
    INSTALL_DESTINATION ${BUFFETALLIGATOR_INSTALL_CMAKEDIR}
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/alligatorConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/alligatorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/alligatorConfigVersion.cmake"
    DESTINATION ${BUFFETALLIGATOR_INSTALL_CMAKEDIR}
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/alligator)
