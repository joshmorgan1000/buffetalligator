/** --------------------------------------------------------------------------------------------------------- Vulkan Fast Path
 * @file vulkan_fastpath_test.cpp
 * @brief The fast path end to end on a device: Slices claimed past one pool and into a dedicated buffer; single
 * dispatches back to back over the two slots; one dispatch of many rows, each resolving its own Slice id; rows
 * whose records are views inside one claim, with the neighbors checked untouched; one Shader dispatched from
 * several threads at once; and the ShaderResult handed back. Every output word is checked.
 *
 * Exit code 77 skips when no Vulkan compute device is present.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <latch>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Slice;
constexpr uint32_t WORDS = 1u << 20;                  ///< Words per single dispatch.
constexpr uint32_t DISPATCHES = 8u;                   ///< Back-to-back single dispatches over the two slots.
constexpr uint32_t ROWS = 64u;                        ///< Rows in the one list dispatch.
constexpr uint32_t ROW_WORDS = 4096u;                 ///< Words per row in the list and view dispatches.
constexpr uint32_t THREADS = 4u;                      ///< Threads dispatching one Shader at once.
constexpr uint32_t PER_THREAD = 16u;                  ///< Dispatches each thread makes.
constexpr uint32_t POOL_BYTES = 64u * 1024u * 1024u;  ///< One pool buffer.
constexpr uint32_t ROLLOVER_CLAIMS = 20u;             ///< 4 MiB claims, enough to roll into a second pool.
/** --------------------------------------------------------------------------------------------------------- Params
 * @brief The record a row's Slice id resolves to.
 */
struct Params {
    uint64_t source;  ///< Device address of the input words.
    uint64_t sink;    ///< Device address of the output words.
    uint32_t count;   ///< Words.
};
/** --------------------------------------------------------------------------------------------------------- Shader Body
 * @brief One lane per word: sink[i] = 3 * source[i] + 1, the record found through the row's Slice id.
 */
constexpr std::string_view SHADER_BODY = R"glsl(
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(buffer_reference, std430, buffer_reference_align = 8) buffer Params { uint64_t source; uint64_t sink; uint count; };
void alligator_main(uint slice) {
    Params params = Params(slice_address(slice));
    const uint index = gl_GlobalInvocationID.x;
    if (index >= params.count) return;
    U32Array source = U32Array(params.source);
    U32Array sink = U32Array(params.sink);
    sink.v[index] = source.v[index] * 3u + 1u;
}
)glsl";
uint64_t failures = 0;  ///< Checks that failed, over the whole run.
/** --------------------------------------------------------------------------------------------------------- Done
 * @brief A dispatch's completion: one count off the latch.
 * @param context The `std::latch`, unowned.
 */
void done(std::shared_ptr<void> context) {
    static_cast<std::latch*>(context.get())->count_down();
}
/** --------------------------------------------------------------------------------------------------------- Now
 * @brief The steady clock in nanoseconds.
 */
int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
/** --------------------------------------------------------------------------------------------------------- Wrong Words
 * @brief Output words that differ from the transformation of the input words.
 */
uint64_t wrong_words(const uint32_t* source, const uint32_t* sink, uint32_t words) {
    uint64_t wrong = 0;
    for (uint32_t index = 0; index < words; ++index) wrong += sink[index] != source[index] * 3u + 1u;
    return wrong;
}
/** --------------------------------------------------------------------------------------------------------- Fill
 * @brief Writes a distinct ramp into the words.
 */
void fill(uint32_t* words, uint32_t count, uint32_t salt) {
    for (uint32_t index = 0; index < count; ++index) words[index] = index * 7u + 3u + salt;
}
/** --------------------------------------------------------------------------------------------------------- Record
 * @brief Writes a row's record over a source and a sink.
 */
void record(Slice& into, const Slice& source, const Slice& sink, uint32_t words) {
    Params* params = into.data<Params>();
    params->source = source.address();
    params->sink = sink.address();
    params->count = words;
}
/** --------------------------------------------------------------------------------------------------------- Claims
 * @brief Claims roll past one pool, a dedicated claim past the pool size, and nothing overlaps.
 */
void claims() {
    std::printf("Claiming %u slices of 4 MiB to roll past one %u MiB pool...\n", ROLLOVER_CLAIMS, POOL_BYTES >> 20);
    std::vector<Slice> held;
    held.reserve(ROLLOVER_CLAIMS);
    for (uint32_t claim = 0; claim < ROLLOVER_CLAIMS; ++claim) held.emplace_back(size_t{WORDS} * sizeof(uint32_t));
    for (uint32_t claim = 0; claim < ROLLOVER_CLAIMS; ++claim) {
        Slice& slice = held[claim];
        const uint64_t address = slice.address();
        if ((address & 63u) != 0u || slice.size_bytes() != size_t{WORDS} * sizeof(uint32_t) || slice.raw() == nullptr
            || address == 0u) {
            ++failures;
            std::printf("  claim %u: address 0x%llx raw %p size %zu\n", claim,
                static_cast<unsigned long long>(address), slice.raw(), slice.size_bytes());
        }
        slice.data<uint32_t>()[0] = claim;
        slice.data<uint32_t>()[WORDS - 1u] = ~claim;
    }
    for (uint32_t claim = 0; claim < ROLLOVER_CLAIMS; ++claim) {
        const uint32_t* words = held[claim].data<uint32_t>();
        if (words[0] != claim || words[WORDS - 1u] != ~claim) {
            ++failures;
            std::printf("  claim %u: wrote %u and %u, read %u and %u\n", claim, claim, ~claim, words[0], words[WORDS - 1u]);
        }
    }
    for (uint32_t left = 0; left < ROLLOVER_CLAIMS; ++left) {
        for (uint32_t right = left + 1u; right < ROLLOVER_CLAIMS; ++right) {
            const uint64_t a = held[left].address();
            const uint64_t b = held[right].address();
            const uint64_t bytes = size_t{WORDS} * sizeof(uint32_t);
            if (a < b + bytes && b < a + bytes) {
                ++failures;
                std::printf("  claims %u and %u overlap: 0x%llx and 0x%llx\n", left, right,
                    static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
            }
        }
    }
    std::printf("Claiming one dedicated slice past the pool size...\n");
    Slice dedicated(size_t{POOL_BYTES} + 4096u, true);
    if (dedicated.address() == 0u || dedicated.raw() == nullptr || dedicated.size_bytes() != size_t{POOL_BYTES} + 4096u
        || !dedicated.is_novel()) {
        ++failures;
        std::printf("  dedicated: address 0x%llx raw %p size %zu novel %d\n",
            static_cast<unsigned long long>(dedicated.address()), dedicated.raw(), dedicated.size_bytes(),
            int(dedicated.is_novel()));
    }
    dedicated.data<uint32_t>()[POOL_BYTES / sizeof(uint32_t)] = 0xC0FFEEu;
    if (dedicated.data<uint32_t>()[POOL_BYTES / sizeof(uint32_t)] != 0xC0FFEEu) ++failures;
}
/** --------------------------------------------------------------------------------------------------------- Singles
 * @brief Single-Slice dispatches back to back over the two slots, with their completions and the last result.
 */
void singles(buffetalligator::Shader& shader) {
    std::printf("Dispatching %u single rows over %u words each...\n", DISPATCHES, WORDS);
    Slice source(size_t{WORDS} * sizeof(uint32_t));
    fill(source.data<uint32_t>(), WORDS, 0u);
    std::vector<Slice> sinks;
    std::vector<Slice> records;
    sinks.reserve(DISPATCHES);
    records.reserve(DISPATCHES);
    for (uint32_t dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
        sinks.emplace_back(size_t{WORDS} * sizeof(uint32_t));
        records.emplace_back(sizeof(Params));
        record(records.back(), source, sinks.back(), WORDS);
    }
    std::latch retired(DISPATCHES);
    const std::shared_ptr<void> latch(std::shared_ptr<std::latch>(), &retired);
    std::vector<buffetalligator::ShaderResult> results;
    results.reserve(DISPATCHES);
    const int64_t start = now_ns();
    for (uint32_t dispatch = 0; dispatch < DISPATCHES; ++dispatch) results.push_back(shader(records[dispatch], &done, latch));
    retired.wait();
    const int64_t elapsed = now_ns() - start;
    std::printf("  %u dispatches retired in %.3f ms, %.1f us each\n", DISPATCHES,
        static_cast<double>(elapsed) / 1e6, static_cast<double>(elapsed) / 1e3 / DISPATCHES);
    if (results.back().get().get() != &retired) {
        ++failures;
        std::printf("  the last result did not hand its context back\n");
    }
    for (uint32_t dispatch = 0; dispatch < DISPATCHES; ++dispatch) {
        const uint64_t wrong = wrong_words(source.data<uint32_t>(), sinks[dispatch].data<uint32_t>(), WORDS);
        if (wrong != 0) {
            failures += wrong;
            std::printf("  single dispatch %u: %llu of %u words wrong\n", dispatch, static_cast<unsigned long long>(wrong), WORDS);
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- List
 * @brief One dispatch of ROWS rows, each row's id resolving its own record with its own source and sink.
 */
void list(buffetalligator::Shader& shader) {
    std::printf("Dispatching one list of %u rows over %u words each...\n", ROWS, ROW_WORDS);
    std::vector<Slice> sources;
    std::vector<Slice> sinks;
    std::vector<Slice> records;
    sources.reserve(ROWS);
    sinks.reserve(ROWS);
    records.reserve(ROWS);
    Slice ids(ROWS * sizeof(Slice));
    Slice* list = ids.data<Slice>();
    for (uint32_t row = 0; row < ROWS; ++row) {
        sources.emplace_back(size_t{ROW_WORDS} * sizeof(uint32_t));
        sinks.emplace_back(size_t{ROW_WORDS} * sizeof(uint32_t));
        records.emplace_back(sizeof(Params));
        fill(sources.back().data<uint32_t>(), ROW_WORDS, row * 1000u);
        record(records.back(), sources.back(), sinks.back(), ROW_WORDS);
        new (list + row) Slice(records.back());
    }
    shader.set_workgroups((ROW_WORDS + 63u) / 64u);
    const int64_t start = now_ns();
    shader(ids, ROWS).get();
    const int64_t elapsed = now_ns() - start;
    std::printf("  %u rows retired in %.3f ms\n", ROWS, static_cast<double>(elapsed) / 1e6);
    for (uint32_t row = 0; row < ROWS; ++row) {
        const uint64_t wrong = wrong_words(sources[row].data<uint32_t>(), sinks[row].data<uint32_t>(), ROW_WORDS);
        if (wrong != 0) {
            failures += wrong;
            std::printf("  list row %u: %llu of %u words wrong\n", row, static_cast<unsigned long long>(wrong), ROW_WORDS);
        }
    }
    for (uint32_t row = 0; row < ROWS; ++row) list[row].~Slice();
}
/** --------------------------------------------------------------------------------------------------------- Views
 * @brief Rows whose sources, sinks and records are views inside one claim; the words between rows stay as written.
 */
void views(buffetalligator::Shader& shader) {
    std::printf("Dispatching %u rows whose records and words are views inside one claim...\n", ROWS);
    constexpr uint32_t ROW_BYTES = ROW_WORDS * sizeof(uint32_t);
    constexpr uint32_t GAP = 64u;
    constexpr uint32_t STRIDE = ROW_BYTES + GAP;
    Slice arena(size_t{ROWS} * STRIDE * 2u + ROWS * 64u);
    uint8_t* base = arena.data<uint8_t>();
    for (size_t byte = 0; byte < arena.size_bytes(); ++byte) base[byte] = 0xEE;
    std::vector<Slice> records;
    records.reserve(ROWS);
    Slice ids(ROWS * sizeof(Slice));
    Slice* list = ids.data<Slice>();
    for (uint32_t row = 0; row < ROWS; ++row) {
        Slice source = arena.slice(size_t{row} * STRIDE, ROW_BYTES);
        Slice sink = arena.slice(size_t{ROWS} * STRIDE + size_t{row} * STRIDE, ROW_BYTES);
        records.push_back(arena.slice(size_t{ROWS} * STRIDE * 2u + size_t{row} * 64u, sizeof(Params)));
        fill(source.data<uint32_t>(), ROW_WORDS, row * 17u);
        record(records.back(), source, sink, ROW_WORDS);
        new (list + row) Slice(records.back());
    }
    shader.set_workgroups((ROW_WORDS + 63u) / 64u);
    shader(ids, ROWS).get();
    for (uint32_t row = 0; row < ROWS; ++row) {
        const uint32_t* source = reinterpret_cast<const uint32_t*>(base + size_t{row} * STRIDE);
        const uint32_t* sink = reinterpret_cast<const uint32_t*>(base + size_t{ROWS} * STRIDE + size_t{row} * STRIDE);
        const uint64_t wrong = wrong_words(source, sink, ROW_WORDS);
        if (wrong != 0) {
            failures += wrong;
            std::printf("  view row %u: %llu of %u words wrong\n", row, static_cast<unsigned long long>(wrong), ROW_WORDS);
        }
        const uint8_t* gap = base + size_t{ROWS} * STRIDE + size_t{row} * STRIDE + ROW_BYTES;
        for (uint32_t byte = 0; byte < GAP; ++byte) {
            if (gap[byte] != 0xEE) {
                ++failures;
                std::printf("  view row %u wrote past its sink at byte %u\n", row, byte);
                break;
            }
        }
    }
    for (uint32_t row = 0; row < ROWS; ++row) list[row].~Slice();
}
/** --------------------------------------------------------------------------------------------------------- Dispatch Rows
 * @brief One thread's share of the concurrent case: its PER_THREAD single dispatches, each waited on.
 */
void dispatch_rows(buffetalligator::Shader* shader, std::vector<Slice>* records, uint32_t thread) {
    for (uint32_t index = 0; index < PER_THREAD; ++index) (*shader)((*records)[thread * PER_THREAD + index]).get();
}
/** --------------------------------------------------------------------------------------------------------- Concurrent
 * @brief THREADS threads each make PER_THREAD single dispatches on the one Shader and wait on their own results.
 */
void concurrent(buffetalligator::Shader& shader) {
    std::printf("Dispatching from %u threads, %u single rows each, on one shader...\n", THREADS, PER_THREAD);
    shader.set_workgroups((ROW_WORDS + 63u) / 64u);
    std::vector<Slice> sources(THREADS * PER_THREAD);
    std::vector<Slice> sinks(THREADS * PER_THREAD);
    std::vector<Slice> records(THREADS * PER_THREAD);
    for (uint32_t index = 0; index < THREADS * PER_THREAD; ++index) {
        sources[index] = Slice(size_t{ROW_WORDS} * sizeof(uint32_t));
        sinks[index] = Slice(size_t{ROW_WORDS} * sizeof(uint32_t));
        records[index] = Slice(sizeof(Params));
        fill(sources[index].data<uint32_t>(), ROW_WORDS, index * 31u);
        record(records[index], sources[index], sinks[index], ROW_WORDS);
    }
    std::vector<std::thread> threads;
    for (uint32_t thread = 0; thread < THREADS; ++thread) threads.emplace_back(&dispatch_rows, &shader, &records, thread);
    for (std::thread& thread : threads) thread.join();
    for (uint32_t index = 0; index < THREADS * PER_THREAD; ++index) {
        const uint64_t wrong = wrong_words(sources[index].data<uint32_t>(), sinks[index].data<uint32_t>(), ROW_WORDS);
        if (wrong != 0) {
            failures += wrong;
            std::printf("  concurrent dispatch %u: %llu of %u words wrong\n", index, static_cast<unsigned long long>(wrong), ROW_WORDS);
        }
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Claims, then every dispatch shape, then the verdict.
 */
int main() {
    if (!buffetalligator::GPU::exists()) {
        std::printf("SKIP: no Vulkan compute device\n");
        return 77;
    }
    std::printf("Device: %s\n", buffetalligator::GPU::device_name().c_str());
    claims();
    std::printf("Compiling the shader and preparing its slots...\n");
    std::unique_ptr<buffetalligator::Shader> shader;
    try {
        shader = std::make_unique<buffetalligator::Shader>(SHADER_BODY, "fastpath_check", (WORDS + 63u) / 64u);
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
    singles(*shader);
    list(*shader);
    views(*shader);
    concurrent(*shader);
    shader.reset();
    std::printf("Failures: %llu\n", static_cast<unsigned long long>(failures));
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
