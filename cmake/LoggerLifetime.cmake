# Keep bundled logger labels usable while global Slice and Shader owners retire.
if(TARGET logging::logging)
    return()
endif()
find_package(Git REQUIRED)
set(BUFFETALLIGATOR_LOGGER_LIFETIME_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/logger-static-stanzas.patch")
file(STRINGS "${THREADSAFE_LOGGER_SOURCE_DIR}/include/loggingutils.hpp"
    BUFFETALLIGATOR_LOGGER_CONSTANT_LABEL
    REGEX "^inline static constexpr char INFO_LOG_STANZA\\[\\] =")
set(BUFFETALLIGATOR_LOGGER_PATCH_DIRECTION)
if(BUFFETALLIGATOR_LOGGER_CONSTANT_LABEL)
    set(BUFFETALLIGATOR_LOGGER_PATCH_DIRECTION --reverse)
    message(STATUS "Verifying the applied logger static-label lifetime patch")
else()
    message(STATUS "Checking the logger static-label lifetime patch")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${THREADSAFE_LOGGER_SOURCE_DIR}"
    apply ${BUFFETALLIGATOR_LOGGER_PATCH_DIRECTION} --check
    "${BUFFETALLIGATOR_LOGGER_LIFETIME_PATCH}"
    RESULT_VARIABLE BUFFETALLIGATOR_LOGGER_PATCH_CHECK)
if(NOT BUFFETALLIGATOR_LOGGER_PATCH_CHECK EQUAL 0)
    message(FATAL_ERROR "Logger lifetime patch check failed (${BUFFETALLIGATOR_LOGGER_PATCH_CHECK}); resolve the reported Git error or update cmake/patches/logger-static-stanzas.patch for the current loggingutils.hpp before rebuilding")
endif()
if(BUFFETALLIGATOR_LOGGER_PATCH_DIRECTION)
    message(STATUS "Logger static-label lifetime patch is already applied")
else()
    message(STATUS "Applying the logger static-label lifetime patch")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${THREADSAFE_LOGGER_SOURCE_DIR}"
        apply "${BUFFETALLIGATOR_LOGGER_LIFETIME_PATCH}"
        RESULT_VARIABLE BUFFETALLIGATOR_LOGGER_PATCH_APPLY)
    if(NOT BUFFETALLIGATOR_LOGGER_PATCH_APPLY EQUAL 0)
        message(FATAL_ERROR "Could not apply the logger lifetime patch (${BUFFETALLIGATOR_LOGGER_PATCH_APPLY}); resolve the reported dependency error and rerun ./run_build.sh")
    endif()
endif()
