/** --------------------------------------------------------------------------------------------------------- Folly Containers Test
 * @file folly_containers_test.cpp
 * @brief Checks that the vendored Folly links and that the relaxed priority queue, the weighted
 * evicting cache map, the stable radix sort, and the zstd and zlib codecs work through it.
 */
#include <alligator.hpp>
#include <folly/algorithm/StableRadixSort.h>
#include <folly/compression/Compression.h>
#include <folly/concurrency/container/RelaxedConcurrentPriorityQueue.h>
#include <folly/container/WeightedEvictingCacheMap.h>
#include "functional_support.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Relaxed Queue
 * @brief Pushes from several threads and drains in one, checking every element comes back once.
 */
void relaxed_queue() {
    LOG_INFO_STREAM << "Checking folly::RelaxedConcurrentPriorityQueue across four pushers";
    folly::RelaxedConcurrentPriorityQueue<uint64_t, false, true, 16> queue;
    std::vector<std::thread> pushers;
    for (uint64_t thread = 0; thread < 4; ++thread) {
        pushers.emplace_back([&queue, thread] {
            for (uint64_t index = 0; index < 10000; ++index) queue.push(thread * 10000 + index);
        });
    }
    for (std::thread& pusher : pushers) pusher.join();
    require(queue.size() == 40000, "the relaxed queue lost pushes");
    std::vector<bool> seen(40000, false);
    for (size_t count = 0; count < 40000; ++count) {
        uint64_t value = 0;
        queue.pop(value);
        require(value < 40000 && !seen[value], "the relaxed queue returned a value twice");
        seen[value] = true;
    }
    require(queue.empty(), "the relaxed queue was not drained");
}
/** --------------------------------------------------------------------------------------------------------- Weighted Cache
 * @brief Checks that the weighted cache evicts by weight and keeps recently used keys.
 */
void weighted_cache() {
    LOG_INFO_STREAM << "Checking folly::WeightedEvictingCacheMap eviction by weight";
    folly::WeightedEvictingCacheMap<int, std::string> cache(100);
    cache.set(1, "one", 40);
    cache.set(2, "two", 40);
    require(cache.exists(1) && cache.exists(2), "the cache dropped entries under its weight");
    (void)cache.get(1);
    cache.set(3, "three", 40);
    require(cache.exists(1) && cache.exists(3) && !cache.exists(2),
        "the cache did not evict the least recently used entry by weight");
    require(cache.getCurrentTotalWeight() == 80, "the cache weight total is wrong");
}
/** --------------------------------------------------------------------------------------------------------- Radix Sort
 * @brief Checks the stable radix sort against std::sort on a deterministic key sequence.
 */
void radix_sort() {
    LOG_INFO_STREAM << "Checking folly::stable_radix_sort against std::sort";
    std::vector<uint32_t> keys(100000);
    uint64_t state = 99;
    for (uint32_t& key : keys) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        key = static_cast<uint32_t>(state >> 32);
    }
    std::vector<uint32_t> expected = keys;
    std::sort(expected.begin(), expected.end());
    folly::stable_radix_sort(keys.begin(), keys.end());
    require(keys == expected, "the radix sort disagrees with std::sort");
}
/** --------------------------------------------------------------------------------------------------------- Codecs
 * @brief Round-trips a buffer through the zstd and zlib codecs Folly was built with.
 */
void codecs() {
    LOG_INFO_STREAM << "Checking the zstd and zlib codecs round-trip";
    std::string text;
    for (int index = 0; index < 2000; ++index) text += "buffet alligator slice ";
    for (folly::compression::CodecType type :
        {folly::compression::CodecType::ZSTD, folly::compression::CodecType::ZLIB}) {
        require(folly::compression::hasCodec(type), "a codec Folly was built for is missing");
        auto codec = folly::compression::getCodec(type);
        const std::string packed = codec->compress(text);
        require(packed.size() < text.size() / 4, "the codec did not compress repetitive text");
        require(codec->uncompress(packed) == text, "the codec did not round-trip");
    }
}
} // namespace

int main() {
    relaxed_queue();
    weighted_cache();
    radix_sort();
    codecs();
    LOG_INFO_STREAM << "Folly containers link and work";
    return 0;
}
