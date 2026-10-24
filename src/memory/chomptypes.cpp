/** --------------------------------------------------------------------------------------------------------- ChompTypes
 * @file chomptypes.cpp
 * @brief The table of element types a slot's type byte indexes, and the raw-bytes record at index zero.
 */
#include <logging.hpp>
#include <alligator.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Raw Bytes
 * @brief Raw bytes have no lifecycle; the slot is zeroed on claim and forgotten on release.
 */
void ChompTypes::raw_bytes_construct(uint32_t, size_t) {}
/** --------------------------------------------------------------------------------------------------------- Raw Bytes Destroy
 * @brief Raw bytes have no lifecycle; the slot is zeroed on claim and forgotten on release.
 */
void ChompTypes::raw_bytes_destroy(uint32_t, size_t) {}
/** --------------------------------------------------------------------------------------------------------- Get by Index
 * @brief The record behind a type index.
 * @param id A chomp id or a bare type index; only the type bits are read.
 * @return The record.
 */
const ChompTypeRecord* ChompTypes::get(const uint32_t& id) {
    return inst().records_[id & CHOMP_TYPES_MASK];
}
/** --------------------------------------------------------------------------------------------------------- Get by Type
 * @brief The record registered for a C++ type, or null when none was.
 * @param type The type.
 * @return The record.
 */
const ChompTypeRecord* ChompTypes::get(const std::type_info* type) {
    std::shared_lock lock(inst().type_to_index_mutex_);
    auto it = inst().type_to_index_.find(type);
    if (it == inst().type_to_index_.end()) return nullptr;
    return inst().records_[it->second];
}
/** --------------------------------------------------------------------------------------------------------- Get by Type Reference
 * @brief The record registered for a C++ type, or null when none was.
 * @param type The type.
 * @return The record.
 */
const ChompTypeRecord* ChompTypes::get(const std::type_info& type) {
    return get(&type);
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Returns the singleton instance of the ChompTypes class.
 * @return Reference to the singleton ChompTypes instance.
 */
ChompTypes& ChompTypes::inst() {
    static ChompTypes* instance = [] {
        ChompTypes* types = new ChompTypes();
        types->records_[0] = &RAW_BYTES_RECORD;
        types->type_to_index_[&typeid(uint8_t)] = 0;
        return types;
    }();
    return *instance;
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Registers a new chomp type with the given type record, or returns the index it already holds.
 * @param record Pointer to the ChompTypeRecord describing the type.
 * @return The index assigned to the registered type.
 */
uint32_t ChompTypes::register_type(const ChompTypeRecord* const record) {
    {
        std::shared_lock lock(inst().type_to_index_mutex_);
        auto it = inst().type_to_index_.find(record->type);
        if (it != inst().type_to_index_.end()) return it->second;
    }
    std::unique_lock lock(inst().type_to_index_mutex_);
    auto it = inst().type_to_index_.find(record->type);
    if (it != inst().type_to_index_.end()) return it->second;
    const uint32_t idx = count().fetch_add(1, std::memory_order_relaxed);
    if (idx >= NUMBER_OF_CHOMP_TYPES) [[unlikely]] {
        count().fetch_sub(1, std::memory_order_relaxed);
        ALLIGATOR_THROW("ChompTypes::register_type: maximum number of registered types exceeded for `"
            + std::string(record->type->name()) + "`");
    }
    inst().type_to_index_[record->type] = idx;
    inst().records_[idx] = record;
    return idx;
}
} // namespace buffetalligator
