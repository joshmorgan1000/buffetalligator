# Folly is built by run_build.sh into deps/folly; its package config resolves Boost, glog,
# gflags, fmt, double-conversion, and libevent from the system, so the Homebrew prefixes join
# the search path on macOS where zstd is keg-only.
if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE BUFFETALLIGATOR_BREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(BUFFETALLIGATOR_BREW_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${BUFFETALLIGATOR_BREW_PREFIX}"
            "${BUFFETALLIGATOR_BREW_PREFIX}/opt/zstd")
    endif()
endif()
set(BUFFETALLIGATOR_FOLLY_DIR "${BUFFETALLIGATOR_DEPS_DIR}/folly" CACHE PATH
    "Prefix where run_build.sh installed Folly")
if(NOT EXISTS "${BUFFETALLIGATOR_FOLLY_DIR}/lib/cmake/folly/folly-config.cmake")
    message(FATAL_ERROR
        "Folly is missing at ${BUFFETALLIGATOR_FOLLY_DIR}; run ./run_build.sh to build dependencies")
endif()
find_package(folly CONFIG REQUIRED PATHS "${BUFFETALLIGATOR_FOLLY_DIR}" NO_DEFAULT_PATH)
install(FILES "${BUFFETALLIGATOR_DEPS_SOURCE_DIR}/folly/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/alligator/folly")
