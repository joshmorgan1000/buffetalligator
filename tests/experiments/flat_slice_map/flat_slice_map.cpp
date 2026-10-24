/** --------------------------------------------------------------------------------------------------------- Flat Chomp Map
 * @file flat_chomp_map.cpp
 * @brief Implements the experiment's opaque flat table of owned Chomp claims.
 */
#include "flat_chomp_map.hpp"
#include <absl/container/flat_hash_map.h>
#include <utility>

namespace experiments {
/** --------------------------------------------------------------------------------------------------------- State
 * @brief Owns the flat key table behind the experiment's private implementation.
 */
struct FlatChompMap::State {
    absl::flat_hash_map<int64_t, buffetalligator::Chomp> rows;
};
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Reserves expected keys before any benchmark operations begin.
 */
FlatChompMap::FlatChompMap(size_t expected) : state_(std::make_unique<State>()) {
    state_->rows.reserve(expected);
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Releases the table and its Chomp claims through their ordinary destructors.
 */
FlatChompMap::~FlatChompMap() = default;
/** --------------------------------------------------------------------------------------------------------- Add Chomp
 * @brief Moves the new claim into its key with last-write replacement semantics.
 */
void FlatChompMap::add_chomp(int64_t identifier, buffetalligator::Chomp payload) {
    state_->rows.insert_or_assign(identifier, std::move(payload));
}
/** --------------------------------------------------------------------------------------------------------- Get Chomp
 * @brief Copies one retained Chomp without exposing a table pointer or iterator.
 */
buffetalligator::Chomp FlatChompMap::get_chomp(int64_t identifier) const {
    const auto& rows = std::as_const(state_->rows);
    const auto found = rows.find(identifier);
    return found == rows.end() ? buffetalligator::Chomp() : found->second;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Reads the number of stored keys while mutations are externally excluded.
 */
size_t FlatChompMap::size() const { return state_->rows.size(); }
/** --------------------------------------------------------------------------------------------------------- Capacity
 * @brief Reports flat table slots without exposing any internal addresses.
 */
size_t FlatChompMap::capacity() const { return state_->rows.capacity(); }
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Erases the full range using Abseil's capacity-preserving operation.
 */
void FlatChompMap::reset() { state_->rows.erase(state_->rows.begin(), state_->rows.end()); }
} // namespace experiments
