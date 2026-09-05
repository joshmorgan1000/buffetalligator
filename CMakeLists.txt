cmake_minimum_required(VERSION 3.20)
project(buffetalligator VERSION 0.1.0 DESCRIPTION "Preallocated host-accessible memory slices" HOMEPAGE_URL "https://github.com/joshmorgan1000/buffetalligator" LANGUAGES CXX)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed; run ./run_build.sh")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(BUFFETALLIGATOR_BUILD_TESTS_DEFAULT ON)
else()
    set(BUFFETALLIGATOR_BUILD_TESTS_DEFAULT OFF)
endif()
option(BUFFETALLIGATOR_BUILD_TESTS "Build the BuffetAlligator tests" ${BUFFETALLIGATOR_BUILD_TESTS_DEFAULT})
set(BUFFETALLIGATOR_DEPS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/src" CACHE PATH "Dependency source directory")
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
find_package(Threads REQUIRED)
add_library(buffetalligator STATIC
    src/memory/slice.cpp
    src/memory/buffet.cpp
    src/memory/alligator.cpp
    src/memory/heapbuffet.cpp
)
add_library(buffetalligator::buffetalligator ALIAS buffetalligator)
target_include_directories(buffetalligator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(buffetalligator PUBLIC cxx_std_20)
target_link_libraries(buffetalligator PUBLIC logging::logging Threads::Threads)
set_target_properties(buffetalligator PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(buffetalligator PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(buffetalligator PRIVATE /W4)
endif()
if(BUFFETALLIGATOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
set(BUFFETALLIGATOR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/buffetalligator")
install(TARGETS buffetalligator
    EXPORT buffetalligatorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES include/buffetalligator.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(BUFFETALLIGATOR_OWNS_LOGGING_TARGET)
    set(BUFFETALLIGATOR_INSTALLED_LOGGING_TARGETS "../logging/loggingTargets.cmake")
else()
    set(BUFFETALLIGATOR_INSTALLED_LOGGING_TARGETS "")
endif()
install(EXPORT buffetalligatorTargets
    FILE buffetalligatorTargets.cmake
    NAMESPACE buffetalligator::
    DESTINATION ${BUFFETALLIGATOR_INSTALL_CMAKEDIR}
)
configure_package_config_file(
    cmake/buffetalligatorConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/buffetalligatorConfig.cmake"
    INSTALL_DESTINATION ${BUFFETALLIGATOR_INSTALL_CMAKEDIR}
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/buffetalligatorConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/buffetalligatorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/buffetalligatorConfigVersion.cmake"
    DESTINATION ${BUFFETALLIGATOR_INSTALL_CMAKEDIR}
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/buffetalligator)
