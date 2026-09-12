#!/usr/bin/env bash
# =========================================================================================================== BuffetAlligator Build
set -Eeuo pipefail
if [[ -t 1 ]]; then
    GREEN=$'\033[1;32m'
    CYAN=$'\033[1;36m'
    YELLOW=$'\033[1;33m'
    RED=$'\033[1;31m'
    DIM=$'\033[0;90m'
    NC=$'\033[0m'
else
    GREEN="" CYAN="" YELLOW="" RED="" DIM="" NC=""
fi
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${REPO_ROOT}/build"
DEPS_DIR="${REPO_ROOT}/deps"
INSTALL_DIR="${REPO_ROOT}/build/install"
CLEAN_BUILD=false
REBUILD_VENDORED=false
RUN_TESTS=true
DEPS_ONLY=false
CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib)
while (( $# > 0 )); do
    case "$1" in
        --clean)
            # Delete the build directory to start fresh.
            CLEAN_BUILD=true
            shift
            ;;
        --rebuild-vendored)
            REBUILD_VENDORED=true
            shift
            ;;
        --deps-only)
            DEPS_ONLY=true
            shift
            ;;
        --skip-tests)
            RUN_TESTS=false
            shift
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || { printf '%s\n' "--build-dir needs a directory"; exit 1; }
            BUILD_ROOT="$2"
            shift 2
            ;;
        --deps-dir)
            [[ $# -ge 2 ]] || { printf '%s\n' "--deps-dir needs a directory"; exit 1; }
            DEPS_DIR="$2"
            shift 2
            ;;
        --install-dir)
            [[ $# -ge 2 ]] || { printf '%s\n' "--install-dir needs a directory"; exit 1; }
            INSTALL_DIR="$2"
            shift 2
            ;;
        -D*)
            CMAKE_ARGS+=("$1")
            shift
            ;;
        *)
            if [[ "$1" != --help && "$1" != -h ]]; then
                printf '%s\n' "${RED}Unknown option: $1${NC}"
                exit 1
            fi
            printf '%s\n' "Usage: $0 [OPTIONS]"
            printf '%s\n' "Options:"
            printf '%s\n' "  --clean              Delete the build directory to start fresh."
            printf '%s\n' "  --rebuild-vendored   Force rebuild of vendored dependencies."
            printf '%s\n' "  --deps-only          Fetch and build the required dependencies only."
            printf '%s\n' "  --skip-tests         Build and install without running the test suite."
            printf '%s\n' "  --build-dir DIR      Specify the build directory."
            printf '%s\n' "  --deps-dir DIR       Specify the dependencies directory."
            printf '%s\n' "  --install-dir DIR    Specify the installation directory."
            printf '%s\n' "  -DNAME=VALUE         Override a CMake cache setting (e.g. -DALLIGATOR_SLOT_BITS=20)."
            printf '%s\n' "  --help               Show this help message."
            exit 0
            ;;
    esac
done
[[ "${BUILD_ROOT}" = /* ]] || BUILD_ROOT="${PWD}/${BUILD_ROOT}"
[[ "${DEPS_DIR}" = /* ]] || DEPS_DIR="${PWD}/${DEPS_DIR}"
[[ "${INSTALL_DIR}" = /* ]] || INSTALL_DIR="${PWD}/${INSTALL_DIR}"
BUILD_DIR="${BUILD_ROOT}/current"
DEPS_SOURCE_DIR="${DEPS_DIR}/src"
DEPS="${DEPS_DIR}"
DEPS_SRC="${DEPS_SOURCE_DIR}"
LOG_FILE="${BUILD_ROOT}/build_buffetalligator.log"
LOGGER_DIR="${DEPS_SOURCE_DIR}/threadsafe-logger"
LOGGER_URL="https://github.com/joshmorgan1000/threadsafe-logger.git"
LIBSODIUM_SRC="${DEPS_SRC}/libsodium"
LIBSODIUM_DEPS="${DEPS}/libsodium"
LIBSODIUM_REPO="https://github.com/jedisct1/libsodium.git"
LIBSODIUM_VERSION="1.0.22-RELEASE"
LIBFABRIC_REPO="https://github.com/ofiwg/libfabric.git"
LIBFABRIC_SRC="${DEPS_SRC}/libfabric"
LIBFABRIC_DEPS="${DEPS}/libfabric"
LIBFABRIC_VERSION="v2.6.0"
CURL_REPO="https://github.com/curl/curl.git"
CURL_SRC="${DEPS_SRC}/curl"
CURL_DEPS="${DEPS}/curl"
CURL_VERSION="curl-8_20_0"
CONCURRENTQUEUE_REPO="https://github.com/cameron314/concurrentqueue.git"
CONCURRENTQUEUE_SRC="${DEPS_SRC}/concurrentqueue"
CONCURRENTQUEUE_DEPS="${DEPS}/concurrentqueue"
CONCURRENTQUEUE_VERSION="v1.0.5"
READERWRITERQUEUE_REPO="https://github.com/cameron314/readerwriterqueue.git"
READERWRITERQUEUE_SRC="${DEPS_SRC}/readerwriterqueue"
READERWRITERQUEUE_DEPS="${DEPS}/readerwriterqueue"
READERWRITERQUEUE_VERSION="v1.0.7"
ABSEIL_REPO="https://github.com/abseil/abseil-cpp.git"
ABSEIL_SRC="${DEPS_SRC}/abseil"
ABSEIL_DEPS="${DEPS}/abseil"
ABSEIL_VERSION="20260107.1"
LIBUV_REPO="https://github.com/libuv/libuv.git"
LIBUV_SRC="${DEPS_SRC}/libuv"
LIBUV_DEPS="${DEPS}/libuv"
LIBUV_VERSION="v1.52.1"
VULKAN_HEADERS_REPO="https://github.com/KhronosGroup/Vulkan-Headers.git"
VULKAN_HEADERS_SRC="${DEPS_SRC}/vulkan-headers"
VULKAN_HEADERS_DEPS="${DEPS}/vulkan-headers"
VULKAN_HEADERS_VERSION="v1.4.350"
SHADERC_REPO="https://github.com/google/shaderc.git"
SHADERC_SRC="${DEPS_SRC}/shaderc"
SHADERC_BUILD_DIR="${SHADERC_SRC}/build"
SHADERC_LIB_FILE="${SHADERC_BUILD_DIR}/libshaderc/libshaderc_combined.a"
SHADERC_GLSLC_FILE="${SHADERC_BUILD_DIR}/glslc/glslc"
SHADERC_SPIRV_HEADERS_DIR="${SHADERC_BUILD_DIR}/install/share/cmake/SPIRV-Headers"
SHADERC_VERSION="v2026.2"
VULKAN_LOADER_REPO="https://github.com/KhronosGroup/Vulkan-Loader.git"
VULKAN_LOADER_SRC="${DEPS_SRC}/vulkan-loader"
VULKAN_LOADER_DEPS="${DEPS}/vulkan-loader"
VULKAN_LOADER_VERSION="v1.4.350"
MOLTENVK_REPO="https://github.com/KhronosGroup/MoltenVK.git"
MOLTENVK_SRC="${DEPS_SRC}/MoltenVK"
MOLTENVK_DEPS="${DEPS}/MoltenVK"
MOLTENVK_VERSION="v1.4.1"
SIMDJSON_REPO="https://github.com/simdjson/simdjson.git"
SIMDJSON_SRC="${DEPS_SRC}/simdjson"
SIMDJSON_DEPS="${DEPS}/simdjson"
SIMDJSON_VERSION="v4.6.3"
printf '%s' "${GREEN}"
cat <<'ALLIGATOR_ART'
                       _.---._
                  _.-'   o   `--..__
              _.-'                   `-._
          _.-'                  o     o  \
         /     /              .__________/
        /     /   `--.________/  /  /  /
       /     /        `-.____________.'
      /  ^  /              /
     /  ^  /      .-------'
    /  ^  /      /      __
   /  ^  /       |     /  `--.
ALLIGATOR_ART
printf '%s%s%s%s%s%s%s\n' "${GREEN}" '  /     /        |    /      ' "${RED}" '( @ ) ' "${YELLOW}" ' _/\/\_'
printf '%s%s%s%s%s%s%s\n' "${GREEN}" ' /     /    .----`---/____  ' "${YELLOW}" '(_____) ' "${GREEN}" '{&}{&}'
printf '%s%s%s%s\n' "${GREEN}" '/     /    /              ' "${CYAN}" '\________________/'
printf '%s%s%s%s\n' "${GREEN}" '|    /     \____.---------' "${CYAN}" '  \____________/'
printf '%s' "${GREEN}"
cat <<'ALLIGATOR_ART'
|   /           /       /    \
 \  \          /       /     |
  \  `-.______|       /____.-'
   `-.._______/______/____)
ALLIGATOR_ART
printf '%s\n' "${CYAN}        B U F F E T   A L L I G A T O R${NC}"
printf '%s\n' "${YELLOW}        Big smiles. Full plates. Happy allocations.${NC}"
missing_dependency() {
    local command_name="$1"
    local package_name="$2"
    printf '%s\n' "${RED}Missing required tool: ${command_name}.${NC}"
    if command -v brew >/dev/null 2>&1; then
        if [[ -t 0 ]]; then
            read -r -p "Install ${package_name} with Homebrew now? [y/N] " answer
            if [[ "${answer}" == "y" || "${answer}" == "Y" ]]; then
                brew install "${package_name}"
                return
            fi
        fi
        printf '%s\n' "Run: brew install ${package_name}"
    elif command -v apt-get >/dev/null 2>&1; then
        if [[ "${command_name}" == c++ ]]; then package_name=build-essential; fi
        if [[ -t 0 ]]; then
            read -r -p "Install ${package_name} with apt now? [y/N] " answer
            if [[ "${answer}" == y || "${answer}" == Y ]]; then
                sudo apt-get install "${package_name}"
                return
            fi
        fi
        printf '%s\n' "Run: sudo apt-get install ${package_name}"
    else
        printf '%s\n' "Install ${package_name} with your system package manager, then rerun ./run_build.sh."
    fi
    exit 1
}
# =========================================================================================================== build Abseil
build_abseil_from_source() {
    local CMAKE_CMD="${CMAKE_CMD:-cmake}"
    local ABSEIL_CONFIG
    local ABSEIL_MAP_LIB
    # find exits non-zero on a missing prefix (first run) — do not let that trip set -e
    ABSEIL_CONFIG=$(find "${ABSEIL_DEPS}" -path '*/cmake/absl/abslConfig.cmake' -print -quit 2>/dev/null || true)
    ABSEIL_MAP_LIB=$(find "${ABSEIL_DEPS}" \( -name 'libabsl_raw_hash_set.a' -o \
        -name 'absl_raw_hash_set.lib' \) -print -quit 2>/dev/null || true)
    if [[ -f "${ABSEIL_DEPS}/.version" ]] && \
       [[ "$(cat "${ABSEIL_DEPS}/.version" 2>/dev/null)" == "${ABSEIL_VERSION}" ]] && \
       [[ -f "${ABSEIL_DEPS}/include/absl/container/flat_hash_map.h" ]] && \
       [[ -n "${ABSEIL_CONFIG}" ]] && \
       [[ -n "${ABSEIL_MAP_LIB}" ]]; then
        echo "Abseil (${ABSEIL_VERSION}) already built at ${ABSEIL_DEPS}"
        return 0
    fi
    mkdir -p "${DEPS_SRC}" "${ABSEIL_DEPS}"
    if [[ ! -d "${ABSEIL_SRC}/.git" ]]; then
        rm -rf "${ABSEIL_SRC}"
        echo "Cloning Abseil (${ABSEIL_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${ABSEIL_VERSION}" \
            "${ABSEIL_REPO}" "${ABSEIL_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${ABSEIL_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${ABSEIL_VERSION}" ]]; then
            pushd "${ABSEIL_SRC}" > /dev/null
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${ABSEIL_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${ABSEIL_VERSION}"
            popd > /dev/null
        fi
    fi
    echo "Building Abseil (${ABSEIL_VERSION})..."
    local ABSEIL_BUILD="${ABSEIL_SRC}/build-psyne"
    local ABSEIL_GEN="Unix Makefiles"
    local ABSEIL_NPROC
    if [[ "$(uname -s)" != "Linux" ]] && command -v ninja >/dev/null 2>&1; then
        ABSEIL_GEN="Ninja"
    fi
    ABSEIL_NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    rm -rf "${ABSEIL_BUILD}" "${ABSEIL_DEPS}"
    mkdir -p "${ABSEIL_BUILD}" "${ABSEIL_DEPS}"
    "${CMAKE_CMD}" -S "${ABSEIL_SRC}" -B "${ABSEIL_BUILD}" \
        -G "${ABSEIL_GEN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${ABSEIL_DEPS}" \
        -DABSL_BUILD_TESTING=OFF \
        -DABSL_ENABLE_INSTALL=ON \
        -DABSL_PROPAGATE_CXX_STD=ON \
        -DBUILD_SHARED_LIBS=OFF > /dev/null
    "${CMAKE_CMD}" --build "${ABSEIL_BUILD}" -j "${ABSEIL_NPROC}"
    "${CMAKE_CMD}" --install "${ABSEIL_BUILD}" > /dev/null
    ABSEIL_CONFIG=$(find "${ABSEIL_DEPS}" -path '*/cmake/absl/abslConfig.cmake' -print -quit 2>/dev/null)
    ABSEIL_MAP_LIB=$(find "${ABSEIL_DEPS}" \( -name 'libabsl_raw_hash_set.a' -o \
        -name 'absl_raw_hash_set.lib' \) -print -quit 2>/dev/null)
    if [[ ! -f "${ABSEIL_DEPS}/include/absl/container/flat_hash_map.h" ]] || \
       [[ -z "${ABSEIL_CONFIG}" ]] || \
       [[ -z "${ABSEIL_MAP_LIB}" ]]; then
        echo "Error: Abseil install is incomplete at ${ABSEIL_DEPS}" >&2
        exit 1
    fi
    echo "${ABSEIL_VERSION}" > "${ABSEIL_DEPS}/.version"
}
# =========================================================================================================== build libfabric
build_libfabric_from_source() {
    if [[ "${REBUILD_VENDORED}" == false && -f "${LIBFABRIC_DEPS}/.version" ]] && \
       [[ "$(cat "${LIBFABRIC_DEPS}/.version" 2>/dev/null)" == "${LIBFABRIC_VERSION}" ]] && \
       [[ -f "${LIBFABRIC_DEPS}/lib/libfabric.a" ]] && \
       [[ -f "${LIBFABRIC_DEPS}/include/rdma/fabric.h" ]]; then
        echo "libfabric (${LIBFABRIC_VERSION}) already built at ${LIBFABRIC_DEPS}"
        return 0
    fi
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${LIBFABRIC_SRC}/.git" ]]; then
        rm -rf "${LIBFABRIC_SRC}"
        echo "Cloning libfabric (${LIBFABRIC_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${LIBFABRIC_VERSION}" \
            "${LIBFABRIC_REPO}" "${LIBFABRIC_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${LIBFABRIC_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${LIBFABRIC_VERSION}" ]]; then
            pushd "${LIBFABRIC_SRC}" > /dev/null
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${LIBFABRIC_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${LIBFABRIC_VERSION}"
            popd > /dev/null
        fi
    fi
    if [[ "${REBUILD_VENDORED}" == true && -f "${LIBFABRIC_SRC}/Makefile" ]]; then
        make -C "${LIBFABRIC_SRC}" clean
    fi
    echo "Building libfabric (${LIBFABRIC_VERSION})..."
    (cd "${LIBFABRIC_SRC}" && ./autogen.sh -s) > /dev/null
    local F_NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    local FABRIC_HMEM_FLAGS=()
    if echo '#include <cuda.h>' | ${CC:-cc} -E -x c - > /dev/null 2>&1; then
        FABRIC_HMEM_FLAGS+=("--enable-cuda-dlopen")
    fi
    if echo '#include <hsa/hsa.h>' | ${CC:-cc} -E -x c - > /dev/null 2>&1; then
        FABRIC_HMEM_FLAGS+=("--enable-rocr-dlopen")
    fi
    (cd "${LIBFABRIC_SRC}" && \
        ./configure \
            --prefix="${LIBFABRIC_DEPS}" \
            --disable-shared \
            --enable-static \
            --enable-pic \
            "${FABRIC_HMEM_FLAGS[@]+"${FABRIC_HMEM_FLAGS[@]}"}") > /dev/null
    make -C "${LIBFABRIC_SRC}" -j "${F_NPROC}"
    make -C "${LIBFABRIC_SRC}" install > /dev/null
    if [[ ! -f "${LIBFABRIC_DEPS}/lib/libfabric.a" ]]; then
        echo "Error: ${LIBFABRIC_DEPS}/lib/libfabric.a not produced" >&2
        exit 1
    fi
    echo "${LIBFABRIC_VERSION}" > "${LIBFABRIC_DEPS}/.version"
}
# =========================================================================================================== stage moodycamel queues (header-only)
build_moodycamel_from_source() {
    local RWQ_INC="${READERWRITERQUEUE_DEPS}/include/moodycamel"
    local CQ_INC="${CONCURRENTQUEUE_DEPS}/include/moodycamel"
    if [[ -f "${READERWRITERQUEUE_DEPS}/.version" ]] && \
       [[ "$(cat "${READERWRITERQUEUE_DEPS}/.version" 2>/dev/null)" == "${READERWRITERQUEUE_VERSION}" ]] && \
       [[ -f "${RWQ_INC}/readerwriterqueue.h" ]] && \
       [[ -f "${RWQ_INC}/atomicops.h" ]] && \
       [[ -f "${CONCURRENTQUEUE_DEPS}/.version" ]] && \
       [[ "$(cat "${CONCURRENTQUEUE_DEPS}/.version" 2>/dev/null)" == "${CONCURRENTQUEUE_VERSION}" ]] && \
       [[ -f "${CQ_INC}/concurrentqueue.h" ]] && \
       [[ -f "${CQ_INC}/blockingconcurrentqueue.h" ]]; then
        echo "moodycamel queues (${READERWRITERQUEUE_VERSION}, ${CONCURRENTQUEUE_VERSION}) already staged"
        return 0
    fi
    mkdir -p "${DEPS_SRC}"
    local repo_dir
    for spec in \
        "${READERWRITERQUEUE_REPO}|${READERWRITERQUEUE_VERSION}|${READERWRITERQUEUE_SRC}" \
        "${CONCURRENTQUEUE_REPO}|${CONCURRENTQUEUE_VERSION}|${CONCURRENTQUEUE_SRC}"; do
        local repo="${spec%%|*}"
        local rest="${spec#*|}"
        local version="${rest%%|*}"
        repo_dir="${rest#*|}"
        if [[ ! -d "${repo_dir}/.git" ]]; then
            rm -rf "${repo_dir}"
            echo "Cloning $(basename "${repo_dir}") (${version})..."
            GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${version}" "${repo}" "${repo_dir}"
        else
            local CURRENT_TAG
            CURRENT_TAG=$(git -C "${repo_dir}" describe --tags --exact-match 2>/dev/null || echo "")
            if [[ "${CURRENT_TAG}" != "${version}" ]]; then
                pushd "${repo_dir}" > /dev/null
                GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${version}" 2>/dev/null \
                    || GIT_TERMINAL_PROMPT=0 git fetch origin
                git checkout "${version}"
                popd > /dev/null
            fi
        fi
    done
    echo "Staging moodycamel headers (${READERWRITERQUEUE_VERSION}, ${CONCURRENTQUEUE_VERSION})..."
    rm -rf "${READERWRITERQUEUE_DEPS}" "${CONCURRENTQUEUE_DEPS}"
    mkdir -p "${RWQ_INC}" "${CQ_INC}"
    cp "${READERWRITERQUEUE_SRC}/readerwriterqueue.h" \
       "${READERWRITERQUEUE_SRC}/readerwritercircularbuffer.h" \
       "${READERWRITERQUEUE_SRC}/atomicops.h" \
       "${RWQ_INC}/"
    cp "${READERWRITERQUEUE_SRC}/LICENSE.md" "${READERWRITERQUEUE_DEPS}/LICENSE.md"
    cp "${CONCURRENTQUEUE_SRC}/concurrentqueue.h" \
       "${CONCURRENTQUEUE_SRC}/blockingconcurrentqueue.h" \
       "${CONCURRENTQUEUE_SRC}/lightweightsemaphore.h" \
       "${CQ_INC}/"
    cp "${CONCURRENTQUEUE_SRC}/LICENSE.md" "${CONCURRENTQUEUE_DEPS}/LICENSE.md"
    echo "${READERWRITERQUEUE_VERSION}" > "${READERWRITERQUEUE_DEPS}/.version"
    echo "${CONCURRENTQUEUE_VERSION}" > "${CONCURRENTQUEUE_DEPS}/.version"
}
# =========================================================================================================== build libsodium
build_libsodium_from_source() {
    if [[ "${REBUILD_VENDORED}" == false && -f "${LIBSODIUM_DEPS}/.version" ]] && \
       [[ "$(cat "${LIBSODIUM_DEPS}/.version" 2>/dev/null)" == "${LIBSODIUM_VERSION}" ]] && \
       [[ -f "${LIBSODIUM_DEPS}/lib/libsodium.a" ]] && \
       [[ -f "${LIBSODIUM_DEPS}/include/sodium.h" ]] && \
       [[ -f "${LIBSODIUM_DEPS}/lib/pkgconfig/libsodium.pc" ]]; then
        echo "libsodium (${LIBSODIUM_VERSION}) already built at ${LIBSODIUM_DEPS}"
        return 0
    fi
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${LIBSODIUM_SRC}/.git" ]]; then
        rm -rf "${LIBSODIUM_SRC}"
        echo "Cloning libsodium (${LIBSODIUM_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${LIBSODIUM_VERSION}" \
            "${LIBSODIUM_REPO}" "${LIBSODIUM_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${LIBSODIUM_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${LIBSODIUM_VERSION}" ]]; then
            pushd "${LIBSODIUM_SRC}" > /dev/null
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${LIBSODIUM_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${LIBSODIUM_VERSION}"
            popd > /dev/null
        fi
    fi
    # Release tags are cut from the stable branch and ship a generated configure; regenerate otherwise.
    if [[ ! -x "${LIBSODIUM_SRC}/configure" ]]; then
        if ! command -v autoreconf >/dev/null 2>&1; then
            echo "Error: libsodium checkout has no configure script and autoconf/automake/libtool are not installed" >&2
            exit 1
        fi
        pushd "${LIBSODIUM_SRC}" > /dev/null
        ./autogen.sh -s
        popd > /dev/null
    fi
    echo "Building libsodium (${LIBSODIUM_VERSION})..."
    local SODIUM_BUILD="${LIBSODIUM_SRC}/build"
    rm -rf "${SODIUM_BUILD}" "${LIBSODIUM_DEPS}"
    mkdir -p "${SODIUM_BUILD}" "${LIBSODIUM_DEPS}"
    local S_NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    pushd "${SODIUM_BUILD}" > /dev/null
    "${LIBSODIUM_SRC}/configure" \
        --prefix="${LIBSODIUM_DEPS}" \
        --disable-shared \
        --enable-static \
        --with-pic \
        --disable-dependency-tracking > /dev/null
    make -j "${S_NPROC}" > /dev/null
    make install > /dev/null
    popd > /dev/null
    if [[ ! -f "${LIBSODIUM_DEPS}/lib/libsodium.a" ]] || [[ ! -f "${LIBSODIUM_DEPS}/include/sodium.h" ]]; then
        echo "Error: libsodium install is incomplete at ${LIBSODIUM_DEPS}" >&2
        exit 1
    fi
    echo "${LIBSODIUM_VERSION}" > "${LIBSODIUM_DEPS}/.version"
}
# =========================================================================================================== build curl
build_curl_from_source() {
    local CMAKE_CMD="${CMAKE_CMD:-cmake}"
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${CURL_SRC}/.git" ]]; then
        rm -rf "${CURL_SRC}"
        echo "Cloning curl (${CURL_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${CURL_VERSION}" \
            "${CURL_REPO}" "${CURL_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${CURL_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${CURL_VERSION}" ]]; then
            pushd "${CURL_SRC}"
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${CURL_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${CURL_VERSION}"
            popd
        fi
    fi
    if [[ -f "${CURL_DEPS}/.version" ]] && \
       [[ "$(cat "${CURL_DEPS}/.version" 2>/dev/null)" == "${CURL_VERSION}" ]] && \
       [[ -f "${CURL_DEPS}/lib/libcurl.a" ]]; then
        return 0
    fi
    echo "Building curl (${CURL_VERSION})..."
    local CURL_BUILD="${CURL_SRC}/build"
    rm -rf "${CURL_BUILD}"
    mkdir -p "${CURL_BUILD}" "${CURL_DEPS}"
    local C_GEN="Unix Makefiles"
    if [[ "$(uname -s)" != "Linux" ]] && command -v ninja >/dev/null 2>&1; then
        C_GEN="Ninja"
    fi
    local C_NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    "${CMAKE_CMD}" -S "${CURL_SRC}" -B "${CURL_BUILD}" \
        -G "${C_GEN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${CURL_DEPS}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_STATIC_LIBS=ON \
        -DBUILD_CURL_EXE=OFF \
        -DBUILD_TESTING=OFF \
        -DCURL_USE_OPENSSL=ON \
        -DCURL_USE_LIBPSL=OFF \
        -DCURL_USE_LIBSSH2=OFF \
        -DCURL_USE_LIBSSH=OFF \
        -DCURL_DISABLE_LDAP=ON \
        -DCURL_DISABLE_LDAPS=ON \
        -DCURL_ZLIB=ON \
        -DCURL_ZSTD=OFF \
        -DCURL_BROTLI=OFF \
        -DUSE_LIBIDN2=OFF \
        -DUSE_NGHTTP2=OFF \
        -DCURL_CA_FALLBACK=ON
    "${CMAKE_CMD}" --build "${CURL_BUILD}" -j "${C_NPROC}"
    "${CMAKE_CMD}" --install "${CURL_BUILD}"
    if [[ ! -f "${CURL_DEPS}/lib/libcurl.a" ]]; then
        echo "Error: ${CURL_DEPS}/lib/libcurl.a not produced" >&2
        exit 1
    fi
    echo "${CURL_VERSION}" > "${CURL_DEPS}/.version"
}
# =========================================================================================================== build Vulkan-Headers
build_vulkan_headers_from_source() {
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${VULKAN_HEADERS_SRC}/.git" ]]; then
        rm -rf "${VULKAN_HEADERS_SRC}"
        echo "Cloning Vulkan-Headers (${VULKAN_HEADERS_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${VULKAN_HEADERS_VERSION}" \
            "${VULKAN_HEADERS_REPO}" "${VULKAN_HEADERS_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${VULKAN_HEADERS_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${VULKAN_HEADERS_VERSION}" ]]; then
            pushd "${VULKAN_HEADERS_SRC}"
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${VULKAN_HEADERS_VERSION}" \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${VULKAN_HEADERS_VERSION}"
            popd
        fi
    fi
    if [[ ! -f "${VULKAN_HEADERS_SRC}/include/vulkan/vulkan.h" ]]; then
        echo "Error: vulkan/vulkan.h missing at ${VULKAN_HEADERS_SRC}/include/"
        exit 1
    fi
    if [[ "${REBUILD_VENDORED}" == false && -f "${VULKAN_HEADERS_DEPS}/.version" ]] && \
       [[ "$(cat "${VULKAN_HEADERS_DEPS}/.version" 2>/dev/null)" == "${VULKAN_HEADERS_VERSION}" ]] && \
       [[ -f "${VULKAN_HEADERS_DEPS}/include/vulkan/vulkan.h" ]] && \
       [[ -f "${VULKAN_HEADERS_DEPS}/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake" ]]; then
        return 0
    fi
    local CMAKE_CMD="${CMAKE_CMD:-cmake}"
    local VH_BUILD="${VULKAN_HEADERS_SRC}/build"
    rm -rf "${VH_BUILD}" "${VULKAN_HEADERS_DEPS}"
    mkdir -p "${VH_BUILD}" "${VULKAN_HEADERS_DEPS}"
    "${CMAKE_CMD}" -S "${VULKAN_HEADERS_SRC}" -B "${VH_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${VULKAN_HEADERS_DEPS}"
    "${CMAKE_CMD}" --install "${VH_BUILD}"
    if [[ ! -f "${VULKAN_HEADERS_DEPS}/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake" ]]; then
        echo "Error: VulkanHeadersConfig.cmake missing after install at ${VULKAN_HEADERS_DEPS}/share/cmake/VulkanHeaders/"
        exit 1
    fi
    echo "${VULKAN_HEADERS_VERSION}" > "${VULKAN_HEADERS_DEPS}/.version"
}
# =========================================================================================================== build shaderc
build_shaderc_from_source() {
    local CMAKE_CMD="${CMAKE_CMD:-cmake}"
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${SHADERC_SRC}/.git" ]]; then
        rm -rf "${SHADERC_SRC}"
        echo "Cloning shaderc (${SHADERC_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --branch "${SHADERC_VERSION}" \
            "${SHADERC_REPO}" "${SHADERC_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${SHADERC_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${SHADERC_VERSION}" ]]; then
            pushd "${SHADERC_SRC}" > /dev/null
            GIT_TERMINAL_PROMPT=0 git fetch origin tag "${SHADERC_VERSION}" \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${SHADERC_VERSION}"
            popd > /dev/null
        fi
    fi
    local SHADERC_HEAD
    SHADERC_HEAD=$(git -C "${SHADERC_SRC}" rev-parse HEAD)
    if [[ -f "${SHADERC_BUILD_DIR}/.head" ]] && \
       [[ "$(cat "${SHADERC_BUILD_DIR}/.head" 2>/dev/null)" == "${SHADERC_HEAD}" ]] && \
       [[ -f "${SHADERC_LIB_FILE}" ]] && \
       [[ -x "${SHADERC_GLSLC_FILE}" || -x "${SHADERC_GLSLC_FILE}.exe" ]] && \
       [[ -f "${SHADERC_SPIRV_HEADERS_DIR}/SPIRV-HeadersConfig.cmake" ]]; then
        return 0
    fi
    echo "Fetching shaderc third-party deps (glslang + SPIRV-Tools + SPIRV-Headers)..."
    pushd "${SHADERC_SRC}"
    if [[ -x "./utils/git-sync-deps" ]]; then
        ./utils/git-sync-deps
    else
        python3 ./utils/git-sync-deps
    fi
    popd
    echo "Building shaderc_combined (${SHADERC_HEAD:0:8})..."
    rm -rf "${SHADERC_BUILD_DIR}"
    mkdir -p "${SHADERC_BUILD_DIR}"
    local SH_GEN="Unix Makefiles"
    if [[ "$(uname -s)" != "Linux" ]] && command -v ninja >/dev/null 2>&1; then
        SH_GEN="Ninja"
    fi
    local SH_NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    "${CMAKE_CMD}" -S "${SHADERC_SRC}" -B "${SHADERC_BUILD_DIR}" \
        -G "${SH_GEN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${SHADERC_BUILD_DIR}/install" \
        -DSHADERC_SKIP_TESTS=ON \
        -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON \
        -DSPIRV_SKIP_TESTS=ON \
        -DENABLE_GLSLANG_BINARIES=OFF \
        -DENABLE_SPVREMAPPER=OFF
    "${CMAKE_CMD}" --build "${SHADERC_BUILD_DIR}" -j "${SH_NPROC}" \
        --target shaderc_combined glslc_exe
    if [[ ! -f "${SHADERC_LIB_FILE}" ]]; then
        echo "Error: ${SHADERC_LIB_FILE} not found after build" >&2
        exit 1
    fi
    if [[ ! -x "${SHADERC_GLSLC_FILE}" && ! -x "${SHADERC_GLSLC_FILE}.exe" ]]; then
        echo "Error: ${SHADERC_GLSLC_FILE} not found after build" >&2
        exit 1
    fi
    local SPIRV_HEADERS_BUILD_DIR="${SHADERC_BUILD_DIR}/spirv-headers-install"
    "${CMAKE_CMD}" -S "${SHADERC_SRC}/third_party/spirv-headers" -B "${SPIRV_HEADERS_BUILD_DIR}" \
        -G "${SH_GEN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${SHADERC_BUILD_DIR}/install" \
        -DSPIRV_HEADERS_ENABLE_TESTS=OFF \
        -DSPIRV_HEADERS_ENABLE_INSTALL=ON
    "${CMAKE_CMD}" --install "${SPIRV_HEADERS_BUILD_DIR}"
    if [[ ! -f "${SHADERC_SPIRV_HEADERS_DIR}/SPIRV-HeadersConfig.cmake" ]]; then
        echo "Error: ${SHADERC_SPIRV_HEADERS_DIR}/SPIRV-HeadersConfig.cmake not found after install" >&2
        exit 1
    fi
    echo "${SHADERC_HEAD}" > "${SHADERC_BUILD_DIR}/.head"
}
# =========================================================================================================== build simdjson
build_simdjson_from_source() {
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${SIMDJSON_SRC}/.git" ]]; then
        rm -rf "${SIMDJSON_SRC}"
        echo "Cloning simdjson (${SIMDJSON_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${SIMDJSON_VERSION}" \
            "${SIMDJSON_REPO}" "${SIMDJSON_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${SIMDJSON_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${SIMDJSON_VERSION}" ]]; then
            pushd "${SIMDJSON_SRC}"
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${SIMDJSON_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${SIMDJSON_VERSION}"
            popd
        fi
    fi
    if [[ -f "${SIMDJSON_DEPS}/.version" ]] && \
       [[ "$(cat "${SIMDJSON_DEPS}/.version" 2>/dev/null)" == "${SIMDJSON_VERSION}" ]] && \
       [[ -f "${SIMDJSON_DEPS}/include/simdjson.h" ]] && \
       [[ -f "${SIMDJSON_DEPS}/src/simdjson.cpp" ]]; then
        return 0
    fi
    echo "Staging simdjson amalgamation (${SIMDJSON_VERSION})..."
    mkdir -p "${SIMDJSON_DEPS}/include" "${SIMDJSON_DEPS}/src"
    if [[ -f "${SIMDJSON_SRC}/singleheader/simdjson.h" \
       && -f "${SIMDJSON_SRC}/singleheader/simdjson.cpp" ]]; then
        cp "${SIMDJSON_SRC}/singleheader/simdjson.h"   "${SIMDJSON_DEPS}/include/simdjson.h"
        cp "${SIMDJSON_SRC}/singleheader/simdjson.cpp" "${SIMDJSON_DEPS}/src/simdjson.cpp"
    else
        echo "Error: simdjson singleheader/ amalgamation missing at ${SIMDJSON_SRC}" >&2
        exit 1
    fi
    echo "${SIMDJSON_VERSION}" > "${SIMDJSON_DEPS}/.version"
}
# =========================================================================================================== build Vulkan-Loader
build_vulkan_loader_from_source() {
    if [[ "$(uname -s)" == "Darwin" ]]; then
        build_moltenvk_from_source
        return $?
    fi
    local CMAKE_CMD="${CMAKE_CMD:-cmake}"
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${VULKAN_LOADER_SRC}/.git" ]]; then
        rm -rf "${VULKAN_LOADER_SRC}"
        echo "Cloning Vulkan-Loader (${VULKAN_LOADER_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${VULKAN_LOADER_VERSION}" \
            "${VULKAN_LOADER_REPO}" "${VULKAN_LOADER_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${VULKAN_LOADER_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${VULKAN_LOADER_VERSION}" ]]; then
            pushd "${VULKAN_LOADER_SRC}"
            GIT_TERMINAL_PROMPT=0 git fetch --depth 1 origin tag "${VULKAN_LOADER_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${VULKAN_LOADER_VERSION}"
            popd
        fi
    fi
    if [[ -f "${VULKAN_LOADER_DEPS}/.version" ]] && \
       [[ "$(cat "${VULKAN_LOADER_DEPS}/.version" 2>/dev/null)" == "${VULKAN_LOADER_VERSION}" ]] && \
       [[ -f "${VULKAN_LOADER_DEPS}/lib/libvulkan.so" \
          || -f "${VULKAN_LOADER_DEPS}/lib/libvulkan.so.1" \
          || -f "${VULKAN_LOADER_DEPS}/lib/libvulkan-1.dll" ]]; then
        return 0
    fi
    echo "Building Vulkan-Loader (${VULKAN_LOADER_VERSION})..."
    local VL_BUILD="${VULKAN_LOADER_SRC}/build"
    rm -rf "${VL_BUILD}"
    mkdir -p "${VL_BUILD}" "${VULKAN_LOADER_DEPS}"
    local VL_GEN="Unix Makefiles"
    if [[ "$(uname -s)" != "Linux" ]] && command -v ninja >/dev/null 2>&1; then
        VL_GEN="Ninja"
    fi
    local VL_NPROC=$(nproc 2>/dev/null || echo 4)
    "${CMAKE_CMD}" -S "${VULKAN_LOADER_SRC}" -B "${VL_BUILD}" \
        -G "${VL_GEN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${VULKAN_LOADER_DEPS}" \
        -DCMAKE_PREFIX_PATH="${VULKAN_HEADERS_DEPS}${CMAKE_PREFIX_PATH:+;${CMAKE_PREFIX_PATH}}" \
        -DVULKAN_HEADERS_INSTALL_DIR="${VULKAN_HEADERS_DEPS}" \
        -DBUILD_TESTS=OFF \
        -DUPDATE_DEPS=OFF \
        -DBUILD_WSI_XCB_SUPPORT=OFF \
        -DBUILD_WSI_XLIB_SUPPORT=OFF \
        -DBUILD_WSI_WAYLAND_SUPPORT=OFF \
        -DBUILD_WSI_DIRECTFB_SUPPORT=OFF
    "${CMAKE_CMD}" --build "${VL_BUILD}" -j "${VL_NPROC}"
    "${CMAKE_CMD}" --install "${VL_BUILD}"
    echo "${VULKAN_LOADER_VERSION}" > "${VULKAN_LOADER_DEPS}/.version"
}
# =========================================================================================================== build MoltenVK
build_moltenvk_from_source() {
    mkdir -p "${DEPS_SRC}"
    if [[ ! -d "${MOLTENVK_SRC}/.git" ]]; then
        rm -rf "${MOLTENVK_SRC}"
        echo "Cloning MoltenVK (${MOLTENVK_VERSION})..."
        GIT_TERMINAL_PROMPT=0 git clone --branch "${MOLTENVK_VERSION}" \
            "${MOLTENVK_REPO}" "${MOLTENVK_SRC}"
    else
        local CURRENT_TAG
        CURRENT_TAG=$(git -C "${MOLTENVK_SRC}" describe --tags --exact-match 2>/dev/null || echo "")
        if [[ "${CURRENT_TAG}" != "${MOLTENVK_VERSION}" ]]; then
            pushd "${MOLTENVK_SRC}"
            GIT_TERMINAL_PROMPT=0 git fetch origin tag "${MOLTENVK_VERSION}" 2>/dev/null \
                || GIT_TERMINAL_PROMPT=0 git fetch origin
            git checkout "${MOLTENVK_VERSION}"
            popd
        fi
    fi
    local DYLIB_OUT="${MOLTENVK_SRC}/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
    if [[ -f "${MOLTENVK_DEPS}/.version" ]] && \
       [[ "$(cat "${MOLTENVK_DEPS}/.version" 2>/dev/null)" == "${MOLTENVK_VERSION}" ]] && \
       [[ -f "${MOLTENVK_DEPS}/lib/libMoltenVK.dylib" ]]; then
        return 0
    fi
    echo "Building MoltenVK (${MOLTENVK_VERSION}) — fetching submodules..."
    pushd "${MOLTENVK_SRC}"
    ./fetchDependencies --macos
    echo "Building MoltenVK macOS dylib via 'make macos'..."
    if [[ "${REBUILD_VENDORED}" == true ]]; then make clean; fi
    make macos
    popd
    if [[ ! -f "${DYLIB_OUT}" ]]; then
        echo "Error: ${DYLIB_OUT} not found after MoltenVK build" >&2
        exit 1
    fi
    mkdir -p "${MOLTENVK_DEPS}/lib" "${MOLTENVK_DEPS}/include"
    cp "${DYLIB_OUT}" "${MOLTENVK_DEPS}/lib/libMoltenVK.dylib"
    if [[ -d "${MOLTENVK_SRC}/MoltenVK/include" ]]; then
        cp -R "${MOLTENVK_SRC}/MoltenVK/include/." "${MOLTENVK_DEPS}/include/"
    fi
    echo "${MOLTENVK_VERSION}" > "${MOLTENVK_DEPS}/.version"
}
# =========================================================================================================== build libuv
build_libuv_from_source() {
    if [[ "${REBUILD_VENDORED}" == false && -f "${LIBUV_DEPS}/.version" ]] &&
       [[ "$(cat "${LIBUV_DEPS}/.version")" == "${LIBUV_VERSION}" ]] &&
       [[ -f "${LIBUV_DEPS}/lib/libuv.a" && -f "${LIBUV_DEPS}/include/uv.h" ]]; then
        return
    fi
    if [[ ! -d "${LIBUV_SRC}/.git" ]]; then
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${LIBUV_VERSION}" \
            "${LIBUV_REPO}" "${LIBUV_SRC}"
    elif [[ "$(git -C "${LIBUV_SRC}" describe --tags --exact-match)" != "${LIBUV_VERSION}" ]]; then
        echo "The libuv checkout differs from ${LIBUV_VERSION}; use a fresh --deps-dir."
        return 1
    fi
    cmake -S "${LIBUV_SRC}" -B "${LIBUV_SRC}/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${LIBUV_DEPS}" -DCMAKE_INSTALL_LIBDIR=lib \
        -DLIBUV_BUILD_SHARED=OFF -DLIBUV_BUILD_TESTS=OFF -DLIBUV_BUILD_BENCH=OFF
    if [[ "${REBUILD_VENDORED}" == true ]]; then
        cmake --build "${LIBUV_SRC}/build" --target clean
    fi
    cmake --build "${LIBUV_SRC}/build" --parallel
    cmake --install "${LIBUV_SRC}/build"
    test -f "${LIBUV_DEPS}/lib/libuv.a"
    echo "${LIBUV_VERSION}" > "${LIBUV_DEPS}/.version"
}
# =========================================================================================================== Build stages
run_stage() {
    local label="$1"
    shift
    printf '%s\n' "${CYAN}${label}...${NC}"
    ( "$@" ) >> "${LOG_FILE}" 2>&1 &
    local stage_process=$!
    local elapsed=0
    while kill -0 "${stage_process}" 2>/dev/null; do
        printf '\r%s' "${DIM}${label} (${elapsed}s)...${NC}"
        sleep 1
        elapsed=$((elapsed + 1))
    done
    if wait "${stage_process}"; then
        printf '\r%s\n' "${GREEN}${label}: done (${elapsed}s).${NC}"
    else
        printf '\r%s\n' "${RED}${label} failed.${NC}"
        printf '%s\n' "Details: ${LOG_FILE}" \
            "Check the log for the dependency or compiler diagnostic, fix it, then rerun ./run_build.sh."
        exit 1
    fi
}
command -v git >/dev/null 2>&1 || missing_dependency git git
command -v cmake >/dev/null 2>&1 || missing_dependency cmake cmake
command -v c++ >/dev/null 2>&1 || missing_dependency c++ llvm
command -v autoreconf >/dev/null 2>&1 || missing_dependency autoreconf autoconf
command -v automake >/dev/null 2>&1 || missing_dependency automake automake
command -v pkg-config >/dev/null 2>&1 || missing_dependency pkg-config pkg-config
if ! command -v libtoolize >/dev/null 2>&1 && ! command -v glibtoolize >/dev/null 2>&1; then
    missing_dependency libtoolize libtool
fi
if [[ "$(uname -s)" == Linux ]]; then
    pkg-config --exists libibverbs || missing_dependency "libibverbs development files" libibverbs-dev
    pkg-config --exists librdmacm || missing_dependency "librdmacm development files" librdmacm-dev
fi
if [[ "${CLEAN_BUILD}" == true && -d "${BUILD_DIR}" ]]; then
    rm -rf "${BUILD_DIR}"
fi
mkdir -p "${BUILD_DIR}" "${DEPS_SOURCE_DIR}"
: > "${LOG_FILE}"
if [[ ! -d "${LOGGER_DIR}/.git" ]]; then
    run_stage "Fetching threadsafe-logger" git clone "${LOGGER_URL}" "${LOGGER_DIR}"
fi
run_stage "Preparing libuv TCP and UDP" build_libuv_from_source
run_stage "Preparing libsodium encryption" build_libsodium_from_source
run_stage "Preparing libfabric RDMA" build_libfabric_from_source
run_stage "Preparing Vulkan headers" build_vulkan_headers_from_source
if [[ "${REBUILD_VENDORED}" == true ]]; then
    rm -f "${VULKAN_LOADER_DEPS}/.version" "${MOLTENVK_DEPS}/.version"
fi
run_stage "Preparing Vulkan runtime" build_vulkan_loader_from_source
if [[ "${DEPS_ONLY}" == true ]]; then
    printf '%s\n' "${GREEN}All networking and Vulkan dependencies are ready in ${DEPS_DIR}.${NC}"
    exit 0
fi
GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi
run_stage "Configuring BuffetAlligator" cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    "${GENERATOR_ARGS[@]}" \
    -DBUFFETALLIGATOR_BUILD_TESTS="$([[ "${RUN_TESTS}" == true ]] && echo ON || echo OFF)" \
    -DBUFFETALLIGATOR_DEPS_SOURCE_DIR="${DEPS_SOURCE_DIR}" \
    -DBUFFETALLIGATOR_DEPS_DIR="${DEPS_DIR}" "${CMAKE_ARGS[@]}"
run_stage "Building BuffetAlligator" cmake --build "${BUILD_DIR}" --parallel
if [[ "${RUN_TESTS}" == true ]]; then
    run_stage "Testing memory and network contracts" ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi
run_stage "Installing BuffetAlligator" cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
printf '%s\n' "${GREEN}BuffetAlligator is built and installed. Full plates, across the wire.${NC}" \
    "${DIM}Static library: ${BUILD_DIR}/liballigator.a${NC}" \
    "${DIM}Installation directory: ${INSTALL_DIR}${NC}"
