/** --------------------------------------------------------------------------------------------------------- Global Slice Lifetime Test
 * @file global_slice_lifetime_test.cpp
 * @brief Retains pre-main null owners until after main and verifies them during static teardown, before the
 * Kitchen initializer declared ahead of them clears the registry and finalizes the Alligator.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/kitchen.hpp>
#include <gpu/easyvulkan.hpp>
#include "functional_support.hpp"
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

using namespace buffetalligator;
using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Global Owner
 * @brief Validates and releases late-initialized owners during static destruction.
 */
struct GlobalOwner {
    uint32_t expected = 0;
    AtomicContainer* registered = nullptr;
    Slice dedicated;
    Slice chained;
    Slice view;
    std::optional<Shader> shader;
    /** ------------------------------------------------------------------------------------------- Dispatch
     * @brief Runs the retained Shader over the retained dedicated Slice and waits for it.
     */
    void dispatch() {
        (*shader)(dedicated).get();
        ++expected;
    }
    ~GlobalOwner() {
        if (expected == 0) return;
        if (shader) dispatch();
        require(dedicated.get_as<uint32_t>() == expected && view.get_as<uint32_t>() == 137
            && chained.get_as<uint32_t>() == 211, "a pre-main owner lost its Slice before static teardown");
        require(registered->load<Slice>().get_as<uint32_t>() == 419,
            "the registered global Slice lost its bytes before static teardown");
        LOG_INFO_STREAM << "Late global Slice and Shader ownership verified during static teardown";
    }
};
GlobalOwner owner;
/** --------------------------------------------------------------------------------------------------------- Late Function Owner
 * @brief Adds a function-static Slice initialized after the pre-main owner objects.
 */
Slice& function_owner() {
    static Slice value;
    return value;
}
constexpr std::string_view body = R"shader(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32(stream, 0u, slice_load_u32(stream, 0u) + 1u);
}
)shader";
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Populates global owners after the lazy Alligator and backend construction can register teardown.
 */
int main(int count, char** arguments) {
    const std::string_view mode = count == 2 ? arguments[1] : "cpu";
    if (mode != "cpu" && mode != "translated") return EXIT_FAILURE;
    if (mode == "cpu") require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    if (mode == "translated" && !VulkanKernel::available()) return 77;
    const BuffetDescriptor* placement = mode == "cpu"
        ? BuffetDescriptors::descriptor<AlignedHeapBuffer>() : Slice::default_placement();
    owner.dedicated = Slice(193, true, placement);
    owner.dedicated.get_as<uint32_t>() = 73;
    owner.dedicated.data<uint32_t>()[16] = 137;
    owner.view = owner.dedicated.slice(65, 65);
    owner.chained = Slice(64, placement);
    owner.chained.get_as<uint32_t>() = 211;
    Slice registered(64, true, placement);
    registered.get_as<uint32_t>() = 419;
    owner.registered = AtomicRegistry::create_global("late_global_slice", std::move(registered));
    function_owner() = Slice(64, placement);
    owner.expected = 73;
    if (mode == "translated") {
        owner.shader.emplace(body, "global_lifetime", 1);
        owner.dispatch();
    }
    LOG_INFO_STREAM << "Leaving pre-main owners populated; backend lifetime mode=" << mode;
    return EXIT_SUCCESS;
}
