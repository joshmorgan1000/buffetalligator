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
        --build-dir)
            BUILD_ROOT="$2"
            shift 2
            ;;
        --deps-dir)
            DEPS_DIR="$2"
            shift 2
            ;;
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        *)
            printf '%s\n' "${RED}Unknown option: $1${NC}"
            printf '%s\n' "Usage: $0 [OPTIONS]"
            printf '%s\n' "Options:"
            printf '%s\n' "  --clean              Delete the build directory to start fresh."
            printf '%s\n' "  --rebuild-vendored   Force rebuild of vendored dependencies."
            printf '%s\n' "  --build-dir DIR      Specify the build directory."
            printf '%s\n' "  --deps-dir DIR       Specify the dependencies directory."
            printf '%s\n' "  --install-dir DIR    Specify the installation directory."
            printf '%s\n' "  --help               Show this help message."
            exit 0
            ;;
    esac
done
BUILD_DIR="${BUILD_ROOT}/current"
DEPS_SOURCE_DIR="${DEPS_DIR}/src"
LOG_FILE="${BUILD_ROOT}/build_buffetalligator.log"
LOGGER_DIR="${DEPS_SOURCE_DIR}/threadsafe-logger"
LOGGER_URL="https://github.com/joshmorgan1000/threadsafe-logger.git"
printf '%s\n' "${CYAN}BUFFET ALLIGATOR${NC}"
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
    else
        printf '%s\n' "Install ${package_name} with your system package manager, then rerun ./run_build.sh."
    fi
    exit 1
}
command -v git >/dev/null 2>&1 || missing_dependency git git
command -v cmake >/dev/null 2>&1 || missing_dependency cmake cmake
command -v c++ >/dev/null 2>&1 || missing_dependency c++ llvm
mkdir -p "${BUILD_DIR}" "${DEPS_SOURCE_DIR}"
: > "${LOG_FILE}"
if [[ ! -d "${LOGGER_DIR}/.git" ]]; then
    printf '%s\n' "${CYAN}Fetching threadsafe-logger...${NC}"
    if ! git clone "${LOGGER_URL}" "${LOGGER_DIR}" >>"${LOG_FILE}" 2>&1; then
        printf '%s\n' "${RED}Could not fetch ${LOGGER_URL}.${NC}" "Check network access, then rerun ./run_build.sh." "Details: ${LOG_FILE}"
        exit 1
    fi
fi
if git -C "${LOGGER_DIR}" remote get-url origin >>"${LOG_FILE}" 2>&1; then
    printf '%s\n' "${CYAN}Updating threadsafe-logger to the latest main...${NC}"
    if ! git -C "${LOGGER_DIR}" fetch origin main >>"${LOG_FILE}" 2>&1 ||
       ! git -C "${LOGGER_DIR}" checkout --detach FETCH_HEAD >>"${LOG_FILE}" 2>&1; then
        printf '%s\n' "${RED}Could not update threadsafe-logger.${NC}" "Check network access, then rerun ./run_build.sh." "Details: ${LOG_FILE}"
        exit 1
    fi
else
    printf '%s\n' "${CYAN}Using the vendored threadsafe-logger checkout...${NC}"
fi
GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi
printf '%s\n' "${CYAN}Configuring BuffetAlligator...${NC}"
if ! cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release -DBUFFETALLIGATOR_BUILD_TESTS=ON -DBUFFETALLIGATOR_DEPS_SOURCE_DIR="${DEPS_SOURCE_DIR}" >>"${LOG_FILE}" 2>&1; then
    printf '%s\n' "${RED}Configuration failed.${NC}" "The last diagnostics were:"
    tail -n 30 "${LOG_FILE}"
    exit 1
fi
printf '%s\n' "${CYAN}Building the library and its static dependencies...${NC}"
if ! cmake --build "${BUILD_DIR}" --parallel >>"${LOG_FILE}" 2>&1; then
    printf '%s\n' "${RED}Build failed.${NC}" "The last diagnostics were:"
    tail -n 30 "${LOG_FILE}"
    exit 1
fi
printf '%s\n' "${CYAN}Running the memory contract tests...${NC}"
if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure >>"${LOG_FILE}" 2>&1; then
    printf '%s\n' "${RED}Tests failed.${NC}" "The last diagnostics were:"
    tail -n 40 "${LOG_FILE}"
    exit 1
fi
printf '%s\n' "${CYAN}Installing the library...${NC}"
if ! cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}" >>"${LOG_FILE}" 2>&1; then
    printf '%s\n' "${RED}Installation failed.${NC}" "The last diagnostics were:"
    tail -n 30 "${LOG_FILE}"
    exit 1
fi
printf '%s\n' "${GREEN}BuffetAlligator is built and all tests passed.${NC}" "${DIM}Static library: ${BUILD_DIR}/liballigator.a${NC}"
printf '%s\n' "${DIM}Installation directory: ${INSTALL_DIR}${NC}"
