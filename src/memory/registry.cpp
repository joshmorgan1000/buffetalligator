/** --------------------------------------------------------------------------------------------------------- Global Atomic Registry
 * @file registry.cpp
 * @brief Retires globally owned values while Slice metadata and detached accounting remain live.
 */
#include <alligator/atomics.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Global Registry
 * @brief Owns global atomic values until static owners retire and before arena teardown begins.
 */
std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>* AtomicRegistry::global_registry() {
    using Registry = std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>;
    static Registry instance;
    return &instance;
}
/** --------------------------------------------------------------------------------------------------------- Global Gate
 * @brief Keeps the registry gate alive through tracker detachment and global-value destruction.
 */
AtomicMutex& AtomicRegistry::global_gate() {
    static AtomicMutex instance;
    return instance;
}
} // namespace buffetalligator
