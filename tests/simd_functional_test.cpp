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
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the private ID search functional case.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {{"id_search", search}});
}
