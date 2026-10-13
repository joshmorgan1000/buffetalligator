/** --------------------------------------------------------------------------------------------------------- Slice Functional Test
 * @file slice_functional_test.cpp
 * @brief Exercises typed access, non-owning views, copies, and ownership through public APIs.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include "functional_support.hpp"
#include <array>
#include <span>
#include <string_view>
#include <utility>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
/** --------------------------------------------------------------------------------------------------------- Aligned Value
 * @brief Stores a test value in one aligned slice granule.
 */
struct alignas(64) AlignedValue {
    uint32_t value;
};
/** --------------------------------------------------------------------------------------------------------- Aligned Text
 * @brief Stores character data in one aligned slice granule.
 */
struct alignas(64) AlignedText {
    std::array<char, 64> characters;
};
/** --------------------------------------------------------------------------------------------------------- Aligned Numbers
 * @brief Stores integer data in one aligned slice granule.
 */
struct alignas(64) AlignedNumbers {
    std::array<uint32_t, 16> numbers;
};
/** --------------------------------------------------------------------------------------------------------- Slice Element
 * @brief Checks whether an element type satisfies the typed slice constraint.
 */
template<typename Element>
concept SliceElement = requires { typename SliceT<Element>; };
static_assert(SliceElement<AlignedValue>);
static_assert(SliceElement<AlignedText>);
static_assert(SliceElement<AlignedNumbers>);
static_assert(!SliceElement<uint32_t>);
static_assert(!SliceElement<uint64_t>);
static_assert(!SliceElement<std::string_view>);
static_assert(!SliceElement<std::span<uint32_t>>);
/** --------------------------------------------------------------------------------------------------------- Typed Slices
 * @brief Checks element access, byte views, placement, and shared ownership of typed storage.
 */
static void typed_slices() {
    SliceT<AlignedValue> empty;
    require(empty.is_null() && empty.length() == 0, "typed default is not empty");
    SliceT<AlignedValue> values(size_t(8));
    require(values.size() == 8 && values.length() == 8, "typed element counts differ");
    require(values.size_bytes() == 8 * sizeof(AlignedValue), "typed byte count is incorrect");
    require(reinterpret_cast<uintptr_t>(values.data()) % alignof(AlignedValue) == 0,
        "typed storage is not aligned");
    require(values.root_slice().placement() == Slice::default_placement(),
        "typed placement differs");
    for (size_t index = 0; index < values.size(); ++index) values.data()[index].value = index + 10;
    require(values.get_as().value == 10 && (*values).value == 10,
        "typed references read the wrong value");
    const auto& constant = values;
    require(constant.get_as().value == 10 && constant.data()[7].value == 17,
        "const access differs");
    SliceT<AlignedValue> copy = values;
    SliceT<AlignedValue> moved = std::move(copy);
    require(copy.is_null() && copy.size_bytes() == 0, "typed move retained its source");
    Slice subview = values.slice(sizeof(uint32_t), 2 * sizeof(uint32_t));
    values.free();
    require(moved.data()[7].value == 17 && subview.get_as<AlignedValue>().value == 10
        && subview.size_bytes() == 64,
        "typed shared ownership did not survive free");
    empty = moved;
    copy = std::move(empty);
    require(empty.is_null() && copy.raw() == moved.raw(), "typed assignment lost ownership");
    /** ------------------------------------------------------------------------------------------- Two Granules
     * @brief Provides an aligned element larger than the undersized adoption fixture.
     */
    struct alignas(64) TwoGranules { unsigned char bytes[128]; };
    bool rejected = false;
    try { SliceT<TwoGranules> invalid{Slice(64)}; }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "a slice smaller than one aligned element was accepted");
    require_throws([] { SliceT<AlignedValue> invalid(SIZE_MAX / sizeof(AlignedValue) + 1); },
        "typed element count overflow accepted");
}
/** --------------------------------------------------------------------------------------------------------- Typed Views
 * @brief Checks that aligned payload references provide live views of the backing bytes.
 */
static void typed_views() {
    constexpr std::string_view text = "abcdefghijklmnop";
    SliceT<AlignedText> characters{Slice(text.data(), text.size())};
    const auto& character_view = characters.get_as().characters;
    require(character_view.size() == 64 && characters.size_bytes() == character_view.size()
        && std::string_view(character_view.data(), text.size()) == text,
        "string view does not cover the represented payload");
    const auto& constant_characters = characters;
    const auto& constant_character_view = constant_characters.get_as().characters;
    require(constant_character_view.size() == 64
        && constant_characters.size_bytes() == constant_character_view.size()
        && std::string_view(constant_character_view.data(), text.size()) == text,
        "const string view differs");
    const std::array<uint32_t, 4> input{2, 3, 5, 7};
    SliceT<AlignedNumbers> numbers{Slice(input.data(), sizeof(input))};
    auto& view = numbers.get_as().numbers;
    require(view.size() == 16 && numbers.size<uint32_t>() == view.size() && view[3] == 7,
        "array view does not cover the payload");
    view[1] = 11;
    const auto& constant_numbers = numbers;
    auto& constant_view = constant_numbers.get_as().numbers;
    static_assert(std::is_const_v<std::remove_reference_t<decltype(constant_view[0])>>);
    require(constant_view[1] == 11, "array view writes did not reach the backing storage");
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
    require(middle.size_bytes() == 64 && middle.data<uint32_t>()[0] == 22 &&
        middle.data<uint32_t>()[1] == 33, "weak subrange copy has the wrong offset");
    require(suffix.size_bytes() == 64 && suffix.get_as<uint32_t>() == 33,
        "weak default-length subrange did not copy the suffix");
    require(clamped.size_bytes() == 64 && clamped.get_as<uint32_t>() == 22,
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
