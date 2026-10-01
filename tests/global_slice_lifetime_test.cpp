/** --------------------------------------------------------------------------------------------------------- Global Slice Lifetime Test
 * @file global_slice_lifetime_test.cpp
 * @brief Retains pre-main null owners until after main without importing executor declarations.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#if defined(BUFFETALLIGATOR_HAS_METAL)
#include <alligator/easymetal.hpp>
#endif
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>

using namespace buffetalligator;
namespace {
/** --------------------------------------------------------------------------------------------------------- Completion
 * @brief Keeps notification storage owned until the callback has released its lock.
 */
struct Completion {
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
};
/** --------------------------------------------------------------------------------------------------------- Complete
 * @brief Publishes callback completion without requiring the executor API.
 */
void complete(void* context) {
    auto& completion = *static_cast<Completion*>(context);
    std::lock_guard lock(completion.mutex);
    completion.completed = true;
    completion.condition.notify_one();
}
/** --------------------------------------------------------------------------------------------------------- Global Owner
 * @brief Validates and releases late-initialized owners during static destruction.
 */
struct GlobalOwner {
    Completion completion;
    uint32_t expected = 0;
    AtomicContainer* registered = nullptr;
    Slice dedicated;
    Slice chained;
    Slice view;
    std::optional<Shader> shader;
    ShaderResult result;
#if defined(BUFFETALLIGATOR_HAS_METAL)
    std::optional<MetalBuffer> metal;
#endif
    /** ------------------------------------------------------------------------------------------- Dispatch
     * @brief Exercises retained shader and Slice owners using only their public completion channel.
     */
    void dispatch() {
        {
            std::lock_guard lock(completion.mutex);
            completion.completed = false;
        }
        (*shader)(dedicated, result, &complete, &completion);
        {
            std::unique_lock lock(completion.mutex);
            while (!completion.completed) {
                if (completion.condition.wait_for(lock, std::chrono::seconds(1))
                    == std::cv_status::timeout)
                    LOG_INFO_STREAM << "Waiting for late global Shader completion";
            }
        }
        result.rethrow();
        ++expected;
    }
    ~GlobalOwner() {
        if (expected == 0) return;
        if (shader) dispatch();
        if (dedicated.get_as<uint32_t>() != expected
            || view.get_as<uint32_t>() != 137 || chained.get_as<uint32_t>() != 211)
            std::abort();
        if (registered->load<Slice>().get_as<uint32_t>() != 419) std::abort();
#if defined(BUFFETALLIGATOR_HAS_METAL)
        if (metal && *static_cast<const uint32_t*>(metal->raw()) != 313) std::abort();
#endif
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
 * @brief Populates global owners after lazy arena and backend construction can register teardown.
 */
int main(int count, char** arguments) {
    const std::string_view mode = count == 2 ? arguments[1] : "cpu";
    if (mode != "cpu" && mode != "native" && mode != "translated" && mode != "late-vulkan")
        return EXIT_FAILURE;
    if (mode == "translated" && !VulkanContext::device_present()) return 77;
    const BuffetDescriptor* placement = mode == "cpu" || mode == "late-vulkan"
        ? BuffetDescriptors::get(AlignedHeapBuffer::type_idx()) : Slice::default_placement();
    if (mode == "native" && placement->type_idx != 2) return 77;
    owner.dedicated = Slice(193, true, placement);
    owner.dedicated.get_as<uint32_t>() = 73;
    owner.dedicated.data<uint32_t>()[16] = 137;
    owner.view = owner.dedicated.slice(65, 65);
    if (mode == "late-vulkan") {
        if (Slice::default_placement()->type_idx != 2 || !VulkanContext::device_present()) return 77;
        placement = VulkanContext::buffer_placement();
    }
    owner.chained = Slice(64, placement);
    owner.chained.get_as<uint32_t>() = 211;
    Slice registered(64, true, placement);
    registered.get_as<uint32_t>() = 419;
    owner.registered = AtomicRegistry::create_global("late_global_slice", std::move(registered));
    function_owner() = Slice(64, placement);
    owner.expected = 73;
    if (mode == "native" || mode == "translated") {
        owner.shader.emplace(mode == "native" ? ShaderSource{{}, body, {}} : ShaderSource{body, {}, {}},
            "global_lifetime");
#if defined(BUFFETALLIGATOR_HAS_METAL)
        if (mode == "native") {
            owner.metal.emplace(64);
            *static_cast<uint32_t*>(owner.metal->raw()) = 313;
        }
#endif
        owner.dispatch();
    }
    LOG_INFO_STREAM << "Leaving pre-main owners populated; backend lifetime mode=" << mode;
    return EXIT_SUCCESS;
}
