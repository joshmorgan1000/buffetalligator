/** --------------------------------------------------------------------------------------------------------- Flat Slice Map
 * @file flat_slice_map.cpp
 * @brief Implements the experiment's opaque flat table of owned Slice claims.
 */
#include "flat_slice_map.hpp"
#include <absl/container/flat_hash_map.h>
#include <utility>

namespace experiments {
/** --------------------------------------------------------------------------------------------------------- State
 * @brief Owns the flat key table behind the experiment's private implementation.
 */
struct FlatSliceMap::State {
    absl::flat_hash_map<int64_t, buffetalligator::Slice> rows;
};
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Reserves expected keys before any benchmark operations begin.
 */
FlatSliceMap::FlatSliceMap(size_t expected) : state_(std::make_unique<State>()) {
    state_->rows.reserve(expected);
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Releases the table and its Slice claims through their ordinary destructors.
 */
FlatSliceMap::~FlatSliceMap() = default;
/** --------------------------------------------------------------------------------------------------------- Add Slice
 * @brief Moves the new claim into its key with last-write replacement semantics.
 */
void FlatSliceMap::add_slice(int64_t identifier, buffetalligator::Slice payload) {
    state_->rows.insert_or_assign(identifier, std::move(payload));
}
/** --------------------------------------------------------------------------------------------------------- Get Slice
 * @brief Copies one retained Slice without exposing a table pointer or iterator.
 */
buffetalligator::Slice FlatSliceMap::get_slice(int64_t identifier) const {
    const auto& rows = std::as_const(state_->rows);
    const auto found = rows.find(identifier);
    return found == rows.end() ? buffetalligator::Slice() : found->second;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Reads the number of stored keys while mutations are externally excluded.
 */
size_t FlatSliceMap::size() const { return state_->rows.size(); }
/** --------------------------------------------------------------------------------------------------------- Capacity
 * @brief Reports flat table slots without exposing any internal addresses.
 */
size_t FlatSliceMap::capacity() const { return state_->rows.capacity(); }
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Erases the full range using Abseil's capacity-preserving operation.
 */
void FlatSliceMap::reset() { state_->rows.erase(state_->rows.begin(), state_->rows.end()); }
} // namespace experiments
