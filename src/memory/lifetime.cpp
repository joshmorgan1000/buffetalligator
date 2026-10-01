/** --------------------------------------------------------------------------------------------------------- Runtime Lifetime
 * @file lifetime.cpp
 * @brief Orders runtime finalization after every translation unit's public Slice and Shader owners.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <memory/lifetime.hpp>
#include <loggingutils.hpp>
#include <atomic>
#include <new>
extern "C" void ba_net_shutdown(void);

namespace buffetalligator {
namespace {
constinit size_t initializer_count = 0;
constinit RuntimeFinalizer* finalizers = nullptr;
constinit std::atomic_flag finalizers_busy;
alignas(KitchenInitializer) unsigned char executor_storage[sizeof(KitchenInitializer)];
} // namespace
/** --------------------------------------------------------------------------------------------------------- Register
 * @brief Publishes a completed owner without allocating teardown callbacks.
 */
RuntimeFinalizer::RuntimeFinalizer(void* instance, void (*deleter)(void*), Phase order)
    : object(instance), destroy(deleter), previous(nullptr), phase(order) {
    while (finalizers_busy.test_and_set(std::memory_order_acquire))
        finalizers_busy.wait(true, std::memory_order_relaxed);
    previous = finalizers;
    finalizers = this;
    finalizers_busy.clear(std::memory_order_release);
    finalizers_busy.notify_one();
}
/** --------------------------------------------------------------------------------------------------------- Initializer
 * @brief Retains the executor and logger without constructing an arena or selecting a GPU backend.
 */
AlligatorInitializer::AlligatorInitializer() {
    if (initializer_count++ == 0) {
        new (executor_storage) KitchenInitializer();
        using threadsafe_logger::logging::GlobalLoggingContext;
        static_cast<void>(GlobalLoggingContext::progress_mutex());
        static_cast<void>(GlobalLoggingContext::progress_bars());
        static_cast<void>(GlobalLoggingContext::component());
        static_cast<void>(GlobalLoggingContext::instance_name());
    }
}
/** --------------------------------------------------------------------------------------------------------- Finalizer
 * @brief Quiesces accepted work and destroys lazy owners before releasing the retained executor.
 */
AlligatorInitializer::~AlligatorInitializer() {
    if (--initializer_count != 0) return;
    ba_net_shutdown();
    Kitchen::inst().drain();
    for (const RuntimeFinalizer::Phase phase :
            {RuntimeFinalizer::Phase::TrackerDetach, RuntimeFinalizer::Phase::Registry,
                RuntimeFinalizer::Phase::Arena, RuntimeFinalizer::Phase::Runtime}) {
        RuntimeFinalizer** next = &finalizers;
        while (*next != nullptr) {
            RuntimeFinalizer* owner = *next;
            if (owner->phase != phase) { next = &owner->previous; continue; }
            *next = owner->previous;
            owner->destroy(owner->object);
        }
    }
    std::launder(reinterpret_cast<KitchenInitializer*>(executor_storage))->~KitchenInitializer();
}
} // namespace buffetalligator
