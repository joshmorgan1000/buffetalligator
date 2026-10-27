#pragma once
/** --------------------------------------------------------------------------------------------------------- Nutritionist
 * @file nutritionist.hpp
 * @brief Does your gator need to keep a closer watch on its consumption? We gotchu.
 */
#ifndef ALLIGATOR_WATCH_YOUR_CONSUMPTION
#define ALLIGATOR_WATCH_YOUR_CONSUMPTION 1
#endif
#if ALLIGATOR_WATCH_YOUR_CONSUMPTION
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace buffetalligator {
class ChainBuffet;
/** --------------------------------------------------------------------------------------------------------- Nutritionist class
 * @class Nutritionist
 * @brief Singleton that keeps every outstanding slice under the ChainBuffet it came from, with the public
 * caller that made it, so a buffer that will not go away can be asked what is still eating from it.
 */
class Nutritionist {
public:
    /** ------------------------------------------------------------------------------------------- Bite structure
     * @struct Bite
     * @brief One outstanding slice: its bytes and the public caller that made it.
     */
    struct Bite {
        uint32_t bytes;                 ///< The slice's alignment-rounded bytes.
        std::source_location location;  ///< The public caller, by value since the caller's frame is gone.
    };
    /** ------------------------------------------------------------------------------------------- Site guard
     * @class Site
     * @brief Names the public caller for the slices this thread makes until the guard leaves scope.
     */
    class Site {
    private:
        const std::source_location* previous_;  ///< The site named before this scope.
    public:
        explicit Site(const std::source_location& location) : previous_(std::exchange(site_, &location)) {}
        ~Site() { site_ = previous_; }
        Site(const Site&) = delete;
        Site& operator=(const Site&) = delete;
    };
private:
    Nutritionist() = default;
    /// @brief The public caller of the slices this thread is making, null outside a sited construction.
    static inline thread_local const std::source_location* site_ = nullptr;
    /** ------------------------------------------------------------------------------------------- PerBuffer structure
     * @struct PerBuffer
     * @brief The outstanding slices of one ChainBuffet, keyed by slice id.
     */
    struct PerBuffer {
        std::unordered_map<uint32_t, Bite> sliceed;  ///< Outstanding slices by id.
    };
    /** ------------------------------------------------------------------------------------------- PerChain structure
     * @struct PerChain
     * @brief One shard of the ledger: the buffers whose slice ids land in it.
     */
    struct PerChain {
        std::unordered_map<ChainBuffet*, PerBuffer> per_buffer;  ///< Buffers with outstanding slices.
        std::unordered_map<uint32_t, ChainBuffet*> owner;        ///< Each outstanding slice's buffer, for release.
        std::shared_mutex mutex;                                 ///< Guards both maps.
    };
    /// @brief The ledger, sharded by slice id so a release lands where its slice was recorded.
    std::array<PerChain, 16> per_chains;
    /** ------------------------------------------------------------------------------------------- Shard
     * @brief The shard a slice id lands in.
     * @param id The slice id.
     * @return The shard.
     */
    PerChain& shard(uint32_t id) { return per_chains[(id >> 9) & 0x0f]; }
public:
    /** ------------------------------------------------------------------------------------------- Get Nutritionist instance
     * @brief Returns the singleton instance of the Nutritionist class.
     */
    static Nutritionist& instance() {
        static Nutritionist instance;
        return instance;
    }
    Nutritionist(const Nutritionist&) = delete;
    Nutritionist& operator=(const Nutritionist&) = delete;
    Nutritionist(Nutritionist&&) = delete;
    Nutritionist& operator=(Nutritionist&&) = delete;
    ~Nutritionist() = default;
    /** ------------------------------------------------------------------------------------------- Current Site
     * @brief The public caller named for this thread, or an empty location outside a sited construction.
     * @return The caller.
     */
    static std::source_location current_site() { return site_ ? *site_ : std::source_location(); }
    /** ------------------------------------------------------------------------------------------- Record Slice
     * @brief Records one slice under its ChainBuffet with the public caller named for this thread.
     * @param id The slice id.
     * @param buffet The ChainBuffet the slice's slot counts against.
     * @param bytes The slice's alignment-rounded bytes.
     */
    static void record_slice(uint32_t id, ChainBuffet* buffet, size_t bytes) {
        PerChain& chain = instance().shard(id);
        std::unique_lock lock(chain.mutex);
        chain.owner[id] = buffet;
        chain.per_buffer[buffet].sliceed[id] = Bite{static_cast<uint32_t>(bytes), current_site()};
    }
    /** ------------------------------------------------------------------------------------------- Release Slice
     * @brief Forgets a slice on its last release; a buffer with nothing left is forgotten with it.
     * @param id The slice id.
     */
    static void release_slice(uint32_t id) {
        PerChain& chain = instance().shard(id);
        std::unique_lock lock(chain.mutex);
        const auto owned = chain.owner.find(id);
        if (owned == chain.owner.end()) return;
        const auto buffer = chain.per_buffer.find(owned->second);
        if (buffer != chain.per_buffer.end()) {
            buffer->second.sliceed.erase(id);
            if (buffer->second.sliceed.empty()) chain.per_buffer.erase(buffer);
        }
        chain.owner.erase(owned);
    }
    /** ------------------------------------------------------------------------------------------- Report
     * @brief Writes every buffer with outstanding slices, fewest first, each with its slices grouped by the
     * caller that made them, then the callers over the whole ledger.
     * @param path The file written.
     */
    static void report(const char* path);
};
/** --------------------------------------------------------------------------------------------------------- Nutritionist::report
 * @brief Writes every buffer with outstanding slices, fewest first, then the callers over the whole ledger.
 * @param path The file written.
 */
inline void Nutritionist::report(const char* path) {
    std::FILE* out = std::fopen(path, "w");
    if (out == nullptr) return;
    using Caller = std::pair<size_t, size_t>;  ///< Slices and bytes.
    struct Row {
        size_t slices = 0;
        size_t bytes = 0;
        std::unordered_map<std::string, Caller> callers;
    };
    std::unordered_map<ChainBuffet*, Row> by_buffet;
    std::unordered_map<std::string, Caller> ledger;
    for (PerChain& chain : instance().per_chains) {
        std::shared_lock lock(chain.mutex);
        for (auto& [buffet, per_buffer] : chain.per_buffer) {
            Row& row = by_buffet[buffet];
            for (auto& [id, bite] : per_buffer.sliceed) {
                const std::string caller = std::string(bite.location.file_name()) + ":"
                    + std::to_string(bite.location.line()) + " " + bite.location.function_name();
                row.slices += 1;
                row.bytes += bite.bytes;
                Caller& per_caller = row.callers[caller];
                per_caller.first += 1;
                per_caller.second += bite.bytes;
                Caller& total = ledger[caller];
                total.first += 1;
                total.second += bite.bytes;
            }
        }
    }
    std::vector<std::pair<ChainBuffet*, Row*>> rows;
    size_t slices = 0;
    size_t bytes = 0;
    for (auto& [buffet, row] : by_buffet) {
        rows.emplace_back(buffet, &row);
        slices += row.slices;
        bytes += row.bytes;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.second->slices != right.second->slices ? left.second->slices < right.second->slices
            : left.second->bytes < right.second->bytes;
    });
    std::fprintf(out, "%zu buffers with outstanding slices, %zu slices, %zu bytes\n", rows.size(), slices, bytes);
    for (const auto& [buffet, row] : rows) {
        std::fprintf(out, "buffer %p: %zu slices, %zu bytes\n", static_cast<void*>(buffet), row->slices, row->bytes);
        std::vector<std::pair<std::string, Caller>> callers(row->callers.begin(), row->callers.end());
        std::sort(callers.begin(), callers.end(), [](const auto& left, const auto& right) {
            return left.second.first > right.second.first;
        });
        for (const auto& [caller, count] : callers) {
            std::fprintf(out, "    %zu slices %zu bytes %s\n", count.first, count.second, caller.c_str());
        }
    }
    std::vector<std::pair<std::string, Caller>> callers(ledger.begin(), ledger.end());
    std::sort(callers.begin(), callers.end(), [](const auto& left, const auto& right) {
        return left.second.second > right.second.second;
    });
    std::fprintf(out, "callers over the ledger:\n");
    for (const auto& [caller, count] : callers) {
        std::fprintf(out, "    %zu slices %zu bytes %s\n", count.first, count.second, caller.c_str());
    }
    std::fclose(out);
}
} // namespace buffetalligator
#endif
