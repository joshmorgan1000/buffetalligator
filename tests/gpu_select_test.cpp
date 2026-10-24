/** --------------------------------------------------------------------------------------------------------- GPU Select Test
 * @file gpu_select_test.cpp
 * @brief Verifies GPU::select freezes the chosen backend ahead of the auto order and refuses a different one after.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <cstdlib>
#include <string_view>

using namespace buffetalligator;
namespace {
/// @brief The exit code ctest reads as a skip.
constexpr int SKIPPED = 77;
/** --------------------------------------------------------------------------------------------------------- Refuses
 * @brief Whether a select of another backend after the freeze throws.
 * @param backend The backend the late select asks for.
 * @return True when the select threw.
 */
bool refuses(GPUBackend backend) {
    try {
        GPU::select(backend);
    } catch (const GPUException&) {
        return true;
    }
    return false;
}
/** --------------------------------------------------------------------------------------------------------- Auto
 * @brief Freezes with no selection and requires Vulkan wherever a GPU exists, never native Metal.
 * @return The process exit code.
 */
int automatic() {
    Chomp first(64);
    first.get_as<uint32_t>() = 419;
    if (GPU::backend() == GPUBackend::CPU) {
        LOG_INFO_STREAM << "No GPU is present, so the automatic Vulkan preference case is skipped.";
        return SKIPPED;
    }
    if (GPU::backend() != GPUBackend::Vulkan || first.get_as<uint32_t>() != 419) return EXIT_FAILURE;
    LOG_INFO_STREAM << "Automatic selection froze the process on Vulkan.";
    return 0;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the automatic case for "auto", otherwise selects the backend named by the first argument, cpu by
 * default, then freezes on the first Chomp.
 */
int main(int argument_count, char** arguments) {
    if (argument_count > 1 && std::string_view(arguments[1]) == "auto") return automatic();
    const bool vulkan = argument_count > 1 && std::string_view(arguments[1]) == "vulkan";
    const GPUBackend chosen = vulkan ? GPUBackend::Vulkan : GPUBackend::CPU;
    GPU::select(chosen);
    try {
        Chomp first(64);
        first.get_as<uint32_t>() = 137;
        if (first.get_as<uint32_t>() != 137) return EXIT_FAILURE;
    } catch (const GPUException& error) {
        LOG_INFO_STREAM << "No Vulkan compute device is present, so the Vulkan selection case is skipped: "
                        << error.what();
        return SKIPPED;
    }
    if (GPU::backend() != chosen || GPU::exists() != vulkan) return EXIT_FAILURE;
    GPU::select(chosen);
    if (!refuses(vulkan ? GPUBackend::CPU : GPUBackend::Vulkan)) return EXIT_FAILURE;
    LOG_INFO_STREAM << "GPU::select froze the process on " << (vulkan ? "Vulkan" : "the CPU")
                    << " and refused a different backend afterwards.";
    return 0;
}
