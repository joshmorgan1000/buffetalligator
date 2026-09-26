/** --------------------------------------------------------------------------------------------------------- ID Search Functional Test
 * @file simd_functional_test.cpp
 * @brief Checks the private ID search ABI across every lane and partial vector length.
 */
#include <simd.hpp>
#include "functional_support.hpp"
#include <array>
#include <limits>

using functional::require;
/** --------------------------------------------------------------------------------------------------------- Search Type
 * @brief Exercises all positions, misses, duplicate ordering, and unaligned ranges for one ID type.
 */
template<typename Value>
static void search_type() {
    std::array<Value, 98> values{};
    for (size_t index = 0; index < values.size(); ++index) values[index] = Value(index + 1);
    for (size_t offset : {size_t(0), size_t(1)}) {
        for (size_t length = 0; length <= 97; ++length) {
            const Value* input = values.data() + offset;
            require(buffetalligator::SIMDMisc::find_id(input, length, 999) == -1,
                "ID search reported a nonexistent ID");
            for (size_t index = 0; index < length; ++index) {
                require(buffetalligator::SIMDMisc::find_id(input, length, input[index]) ==
                    static_cast<int64_t>(index), "ID search missed a vector lane or tail");
            }
        }
    }
    values.fill(Value(17));
    require(buffetalligator::SIMDMisc::find_id(values.data(), values.size(), 17) == 0,
        "duplicate search did not return the first match");
    if constexpr (std::is_signed_v<Value>) {
        values[7] = std::numeric_limits<Value>::min();
        require(buffetalligator::SIMDMisc::find_id(values.data(), values.size(), values[7]) == 7,
            "ID search missed the minimum signed value");
    }
}
/** --------------------------------------------------------------------------------------------------------- Search
 * @brief Checks the signed and unsigned ID widths supported by the library.
 */
static void search() {
    search_type<int16_t>();
    search_type<uint16_t>();
    search_type<int32_t>();
    search_type<uint32_t>();
    search_type<int64_t>();
    search_type<uint64_t>();
}
/** --------------------------------------------------------------------------------------------------------- Scattered Words
 * @brief Fills distinct 64-bit words spread across the whole unsigned range, so signed compares
 * would misorder them.
 */
static std::array<uint64_t, 70> scattered_words() {
    std::array<uint64_t, 70> words{};
    for (size_t index = 0; index < words.size(); ++index) {
        words[index] = (index + 1) * 0x9E3779B97F4A7C15ull;
    }
    return words;
}
/** --------------------------------------------------------------------------------------------------------- Max Search
 * @brief Checks the largest word, its slot, and the runner-up for every length, offset, and
 * position, including all-equal fills.
 */
static void max_search() {
    const std::array<uint64_t, 70> words = scattered_words();
    for (size_t offset : {size_t(0), size_t(1)}) {
        for (size_t length = 1; length + offset <= words.size(); ++length) {
            const uint64_t* input = words.data() + offset;
            uint64_t largest = 0;
            uint64_t second = 0;
            for (size_t index = 0; index < length; ++index) {
                if (input[index] > largest) {
                    second = largest;
                    largest = input[index];
                } else if (input[index] > second) {
                    second = input[index];
                }
            }
            uint64_t max = 0;
            uint64_t runner_up = 0;
            const size_t slot =
                buffetalligator::SIMDMisc::max_index(input, length, max, runner_up);
            require(max == largest && input[slot] == largest,
                "max search missed the largest word");
            require(runner_up == second, "max search reported the wrong runner-up");
        }
    }
    std::array<uint64_t, 70> moving = words;
    for (size_t position = 0; position < moving.size(); ++position) {
        moving[position] = ~uint64_t(1);
        uint64_t max = 0;
        uint64_t runner_up = 0;
        const size_t slot =
            buffetalligator::SIMDMisc::max_index(moving.data(), moving.size(), max, runner_up);
        require(slot == position && max == ~uint64_t(1), "max search returned the wrong slot");
        moving[position] = words[position];
    }
    std::array<uint64_t, 37> filled{};
    filled.fill(~uint64_t(0));
    uint64_t max = 0;
    uint64_t runner_up = 0;
    size_t slot =
        buffetalligator::SIMDMisc::max_index(filled.data(), filled.size(), max, runner_up);
    require(max == ~uint64_t(0) && runner_up == ~uint64_t(0) && slot < filled.size(),
        "max search mishandled an all-ones fill");
    filled.fill(0);
    slot = buffetalligator::SIMDMisc::max_index(filled.data(), filled.size(), max, runner_up);
    require(slot < filled.size() && max == 0 && runner_up == 0,
        "max search mishandled an all-zero fill");
    require(buffetalligator::SIMDMisc::max_index(words.data(), 1, max, runner_up) == 0
        && max == words[0] && runner_up == 0, "max search mishandled a single word");
}
/** --------------------------------------------------------------------------------------------------------- Min Search
 * @brief Checks the smallest word and its slot for every length, offset, and position, including
 * all-ones fills.
 */
static void min_search() {
    const std::array<uint64_t, 70> words = scattered_words();
    for (size_t offset : {size_t(0), size_t(1)}) {
        for (size_t length = 1; length + offset <= words.size(); ++length) {
            const uint64_t* input = words.data() + offset;
            uint64_t smallest = ~uint64_t(0);
            uint64_t second = ~uint64_t(0);
            for (size_t index = 0; index < length; ++index) {
                if (input[index] < smallest) {
                    second = smallest;
                    smallest = input[index];
                } else if (input[index] < second) {
                    second = input[index];
                }
            }
            uint64_t min = 0;
            const size_t slot = buffetalligator::SIMDMisc::min_index(input, length, min);
            require(min == smallest && input[slot] == smallest,
                "min search missed the smallest word");
            uint64_t runner_up = 0;
            const size_t again =
                buffetalligator::SIMDMisc::min_index(input, length, min, runner_up);
            require(min == smallest && input[again] == smallest && runner_up == second,
                "min search with runner-up disagreed with the reference");
        }
    }
    std::array<uint64_t, 70> moving = words;
    for (size_t position = 0; position < moving.size(); ++position) {
        moving[position] = 0;
        uint64_t min = 0;
        const size_t slot =
            buffetalligator::SIMDMisc::min_index(moving.data(), moving.size(), min);
        require(slot == position && min == 0, "min search returned the wrong slot");
        moving[position] = words[position];
    }
    std::array<uint64_t, 37> filled{};
    filled.fill(~uint64_t(0));
    uint64_t min = 0;
    require(buffetalligator::SIMDMisc::min_index(filled.data(), filled.size(), min) < filled.size()
        && min == ~uint64_t(0), "min search mishandled an all-ones fill");
    require(buffetalligator::SIMDMisc::min_index(words.data(), 1, min) == 0 && min == words[0],
        "min search mishandled a single word");
    uint64_t runner_up = 0;
    require(buffetalligator::SIMDMisc::min_index(filled.data(), filled.size(), min, runner_up)
        < filled.size() && min == ~uint64_t(0) && runner_up == ~uint64_t(0),
        "min search with runner-up mishandled an all-ones fill");
    require(buffetalligator::SIMDMisc::min_index(words.data(), 1, min, runner_up) == 0
        && min == words[0] && runner_up == ~uint64_t(0),
        "min search with runner-up mishandled a single word");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the private SIMD kernel functional cases.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {
        {"id_search", search}, {"max_search", max_search}, {"min_search", min_search}
    });
}
