/** --------------------------------------------------------------------------------------------------------- Global Atomic Registry
 * @file registry.cpp
 * @brief Retires globally owned values while Slice metadata and detached accounting remain live.
 */
#include <alligator/atomics.hpp>
#include <memory/lifetime.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Global Registry
 * @brief Owns global atomic values until static owners retire and before arena teardown begins.
 */
std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>* AtomicRegistry::global_registry() {
    using Registry = std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>;
    static RuntimeFinalizer lifetime(new Registry, &RuntimeFinalizer::delete_owner<Registry>,
        RuntimeFinalizer::Phase::Registry);
    return static_cast<Registry*>(lifetime.object);
}
/** --------------------------------------------------------------------------------------------------------- Global Gate
 * @brief Keeps the registry gate alive through tracker detachment and global-value destruction.
 */
AtomicMutex& AtomicRegistry::global_gate() {
    static RuntimeFinalizer lifetime(new AtomicMutex, &RuntimeFinalizer::delete_owner<AtomicMutex>);
    return *static_cast<AtomicMutex*>(lifetime.object);
}
} // namespace buffetalligator
