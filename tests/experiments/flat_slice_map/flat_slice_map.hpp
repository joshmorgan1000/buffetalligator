#pragma once
/** --------------------------------------------------------------------------------------------------------- Flat Chomp Map
 * @file flat_chomp_map.hpp
 * @brief Declares an experiment comparing retained Chomp lookup through a flat hash table.
 */
#include <alligator.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace experiments {
/** --------------------------------------------------------------------------------------------------------- FlatChompMap
 * @brief Requires exclusive mutations and permits concurrent const lookups after publication.
 */
class FlatChompMap {
private:
    struct State;
    std::unique_ptr<State> state_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Reserves the expected number of keys before publication begins.
     */
    explicit FlatChompMap(size_t expected);
    FlatChompMap(const FlatChompMap&) = delete;
    FlatChompMap& operator=(const FlatChompMap&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases stored Chomp claims while independently retained results remain owned.
     */
    ~FlatChompMap();
    /** ------------------------------------------------------------------------------------------- Add Chomp
     * @brief Transfers a Chomp into its key and releases any previously stored claim.
     */
    void add_chomp(int64_t identifier, buffetalligator::Chomp payload);
    /** ------------------------------------------------------------------------------------------- Get Chomp
     * @brief Returns an independently owned Chomp copy or a null Chomp for a missing key.
     */
    buffetalligator::Chomp get_chomp(int64_t identifier) const;
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
