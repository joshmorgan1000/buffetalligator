/** --------------------------------------------------------------------------------------------------------- Slice Functional Test
 * @file slice_functional_test.cpp
 * @brief Exercises typed access, non-owning views, copies, and ownership through public APIs.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <array>
#include <span>
#include <string_view>
#include <utility>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
/** --------------------------------------------------------------------------------------------------------- Typed Slices
 * @brief Checks element access, byte views, placement, and shared ownership of typed storage.
 */
static void typed_slices() {
    SliceT<uint32_t> empty;
    require(empty.is_null() && empty.length() == 0, "typed default is not empty");
    SliceT<uint32_t> values(size_t(8));
    require(values.size() == 8 && values.length() == 8, "typed element counts differ");
    require(values.size_bytes() == 32, "typed byte count is incorrect");
    require(values.root_slice().placement() == Slice::default_placement(),
        "typed placement differs");
    for (size_t index = 0; index < values.size(); ++index) values.data()[index] = index + 10;
    require(values.get_as() == 10 && *values == 10, "typed references read the wrong value");
    const auto& constant = values;
    require(constant.get_as() == 10 && constant.data()[7] == 17, "const access differs");
    SliceT<uint32_t> copy = values;
    SliceT<uint32_t> moved = std::move(copy);
    require(copy.is_null() && copy.size_bytes() == 0, "typed move retained its source");
    Slice subview = values.slice(sizeof(uint32_t), 2 * sizeof(uint32_t));
    values.free();
    require(moved.data()[7] == 17 && subview.get_as<uint32_t>() == 11,
        "typed shared ownership did not survive free");
    empty = moved;
    copy = std::move(empty);
    require(empty.is_null() && copy.raw() == moved.raw(), "typed assignment lost ownership");
    require_throws([] { SliceT<uint32_t> invalid{Slice(3)}; }, "partial element accepted");
    require_throws([] { SliceT<uint64_t> invalid(SIZE_MAX / sizeof(uint64_t) + 1); },
        "typed element count overflow accepted");
}
/** --------------------------------------------------------------------------------------------------------- Typed Views
 * @brief Checks that string and span access return live views of the backing bytes.
 */
static void typed_views() {
    constexpr std::string_view text = "abcdefghijklmnop";
    SliceT<std::string_view> characters{Slice(text.data(), text.size())};
    require(characters.get_as() == text, "string view does not cover the payload");
    const auto& constant_characters = characters;
    require(constant_characters.get_as() == text, "const string view differs");
    const std::array<uint32_t, 4> input{2, 3, 5, 7};
    SliceT<std::span<uint32_t>> numbers{Slice(input.data(), sizeof(input))};
    auto view = numbers.get_as();
    require(view.size() == 4 && view[3] == 7, "span view does not cover the payload");
    view[1] = 11;
    const auto& constant_numbers = numbers;
    auto constant_view = constant_numbers.get_as();
    static_assert(std::is_const_v<std::remove_reference_t<decltype(constant_view[0])>>);
    require(constant_view[1] == 11, "span view writes did not reach the backing storage");
}
/** --------------------------------------------------------------------------------------------------------- Weak Slices
 * @brief Checks borrowed memory access and deep copies of full and partial byte ranges.
 */
static void weak_slices() {
    std::array<uint32_t, 4> input{11, 22, 33, 44};
    WeakSlice weak(input.data(), sizeof(input));
    require(weak && weak.placement() == nullptr, "weak slice has incorrect identity");
    require(weak.size<uint32_t>() == input.size(), "weak element count differs");
    require(weak.data<uint32_t>() == input.data(), "weak data did not alias its input");
    (*weak.data<uint32_t>()) = 12;
    const auto& constant = weak;
    require(*constant.data<uint32_t>() == 12 && constant.data<uint32_t>()[3] == 44,
        "const weak access differs");
    Slice whole = static_cast<Slice>(weak);
    Slice middle = weak.slice(sizeof(uint32_t), 2 * sizeof(uint32_t));
    Slice suffix = weak.slice(2 * sizeof(uint32_t));
    Slice clamped = weak.slice(sizeof(uint32_t), SIZE_MAX - 1);
    require(middle.size_bytes() == 8 && middle.data<uint32_t>()[0] == 22 &&
        middle.data<uint32_t>()[1] == 33, "weak subrange copy has the wrong offset");
    require(suffix.size_bytes() == 8 && suffix.get_as<uint32_t>() == 33,
        "weak default-length subrange did not copy the suffix");
    require(clamped.size_bytes() == 12 && clamped.get_as<uint32_t>() == 22,
        "weak length overflow did not clamp to the available bytes");
    input.fill(0);
    require(whole.get_as<uint32_t>() == 12 && middle.get_as<uint32_t>() == 22,
        "weak conversion did not create independent storage");
    require_throws([&] { weak.slice(sizeof(input) + 1); }, "weak invalid offset accepted");
    require_throws([] { WeakSlice invalid(size_t(4), true); }, "weak allocation accepted");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Dispatches independent typed and weak slice functional cases.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {
        {"typed_slices", typed_slices}, {"typed_views", typed_views},
        {"weak_slices", weak_slices}
    });
}
