#pragma once
/** --------------------------------------------------------------------------------------------------------- Flat Slice Map
 * @file flat_slice_map.hpp
 * @brief Declares an experiment comparing retained Slice lookup through a flat hash table.
 */
#include <alligator.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace experiments {
/** --------------------------------------------------------------------------------------------------------- FlatSliceMap
 * @brief Requires exclusive mutations and permits concurrent const lookups after publication.
 */
class FlatSliceMap {
private:
    struct State;
    std::unique_ptr<State> state_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Reserves the expected number of keys before publication begins.
     */
    explicit FlatSliceMap(size_t expected);
    FlatSliceMap(const FlatSliceMap&) = delete;
    FlatSliceMap& operator=(const FlatSliceMap&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases stored Slice claims while independently retained results remain owned.
     */
    ~FlatSliceMap();
    /** ------------------------------------------------------------------------------------------- Add Slice
     * @brief Transfers a Slice into its key and releases any previously stored claim.
     */
    void add_slice(int64_t identifier, buffetalligator::Slice payload);
    /** ------------------------------------------------------------------------------------------- Get Slice
     * @brief Returns an independently owned Slice copy or a null Slice for a missing key.
     */
    buffetalligator::Slice get_slice(int64_t identifier) const;
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Returns the number of stored keys, including keys with null payloads.
     */
    size_t size() const;
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief Reports allocated hash slots for the experiment's layout diagnostics.
     */
    size_t capacity() const;
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Releases all stored claims while preserving the table's reserved capacity.
     */
    void reset();
};
} // namespace experiments
