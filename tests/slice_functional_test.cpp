/** --------------------------------------------------------------------------------------------------------- Slice Functional Test
 * @file slice_functional_test.cpp
 * @brief Exercises typed access, non-owning views, copies, and ownership through public APIs.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
/** --------------------------------------------------------------------------------------------------------- Aligned Value
 * @brief Stores a test value in one 64-byte aligned slice.
 */
struct alignas(64) AlignedValue {
    uint32_t value;
};
/** --------------------------------------------------------------------------------------------------------- Aligned Text
 * @brief Stores character data in one 64-byte aligned slice.
 */
struct alignas(64) AlignedText {
    std::array<char, 64> characters;
};
/** --------------------------------------------------------------------------------------------------------- Aligned Numbers
 * @brief Stores integer data in one 64-byte aligned slice.
 */
struct alignas(64) AlignedNumbers {
    std::array<uint32_t, 16> numbers;
};
/** --------------------------------------------------------------------------------------------------------- Slice Element
 * @brief Checks that any element type up to the SIMD line's alignment is a typed slice element.
 */
template<typename Element>
concept SliceElement = requires { typename SliceT<Element>; };
static_assert(SliceElement<AlignedValue>);
static_assert(SliceElement<AlignedText>);
static_assert(SliceElement<AlignedNumbers>);
static_assert(SliceElement<uint32_t>);
static_assert(SliceElement<uint64_t>);
static_assert(SliceElement<std::string_view>);
static_assert(SliceElement<std::span<uint32_t>>);
/** --------------------------------------------------------------------------------------------------------- Typed Slices
 * @brief Checks element access, byte views, placement, and shared ownership of a one-element typed claim.
 */
static void typed_slices() {
    SliceT<AlignedValue> empty;
    require(empty.is_null() && empty.length() == 0, "typed default is not empty");
    SliceT<AlignedValue> value(true);
    require(value.size() == 1 && value.length() == 1, "a typed claim is not one element");
    require(value.size_bytes() == sizeof(AlignedValue), "typed byte count is incorrect");
    require(reinterpret_cast<uintptr_t>(value.data()) % alignof(AlignedValue) == 0,
        "typed storage is not aligned");
    value->value = 10;
    require(value.get_as().value == 10 && (*value).value == 10,
        "typed references read the wrong value");
    const auto& constant = value;
    require(constant.get_as().value == 10 && constant->value == 10, "const access differs");
    SliceT<AlignedValue> copy = value;
    SliceT<AlignedValue> moved = std::move(copy);
    require(copy.is_null() && copy.size_bytes() == 0, "typed move retained its source");
    Slice subview = value.slice(0, sizeof(uint32_t));
    value.free();
    require(moved->value == 10 && subview.get_as<uint32_t>() == 10 && subview.size_bytes() == sizeof(uint32_t),
        "typed shared ownership did not survive free");
    empty = moved;
    copy = std::move(empty);
    require(empty.is_null() && copy.raw() == moved.raw(), "typed assignment lost ownership");
    SliceT<AlignedValue> adopted{Slice(sizeof(AlignedValue))};
    require(adopted.size() == 1, "adopting a raw Slice did not keep its slot");
}
/** --------------------------------------------------------------------------------------------------------- Counted Value
 * @brief Counts live constructions so typed slots can be checked for exactly-once destruction.
 */
struct alignas(64) CountedValue {
    static inline std::atomic<int> live{0};
    uint64_t value = 7;
    CountedValue() { live.fetch_add(1, std::memory_order_relaxed); }
    ~CountedValue() { live.fetch_sub(1, std::memory_order_relaxed); }
};
/** --------------------------------------------------------------------------------------------------------- Typed Lifecycle
 * @brief Checks a typed claim constructs its element and the last handle on the slot destroys it once.
 */
static void typed_lifecycle() {
    {
        SliceT<CountedValue> value(true);
        require(CountedValue::live.load() == 1 && value->value == 7,
            "a typed claim did not construct its element");
        SliceT<CountedValue> shared = value;
        require(shared.root_slice().raw() == value.root_slice().raw(),
            "a typed copy did not share its record");
        Slice view = value.slice(0, sizeof(uint64_t));
        value.free();
        require(CountedValue::live.load() == 1,
            "a typed slot destroyed its element while a copy remained");
        shared.free();
        /// A view of the element's bytes holds the buffer, and the element with it, until it goes.
        require(CountedValue::live.load() == 1 && view.size_bytes() == sizeof(uint64_t),
            "a view of a typed element let the element go, or lost its bytes");
        view.free();
        require(CountedValue::live.load() == 0, "the last handle on a typed element did not destroy it");
    }
    {
        SliceT<CountedValue> single(true, true);
        require(CountedValue::live.load() == 1 && single.get_as().value == 7,
            "a novel typed slice did not construct its element");
    }
    require(CountedValue::live.load() == 0, "a novel typed slice did not destroy its element on release");
}
/** --------------------------------------------------------------------------------------------------------- Typed Views
 * @brief Checks that aligned payload references provide live views of the backing bytes.
 */
static void typed_views() {
    constexpr std::string_view text = "abcdefghijklmnop";
    SliceT<AlignedText> characters(true);
    std::memcpy(characters.raw(), text.data(), text.size());
    const auto& character_view = characters.get_as().characters;
    require(character_view.size() == 64 && characters.root_slice().capacity() == character_view.size()
        && std::string_view(character_view.data(), text.size()) == text,
        "string view does not cover the represented payload");
    const auto& constant_characters = characters;
    const auto& constant_character_view = constant_characters.get_as().characters;
    require(constant_character_view.size() == 64
        && constant_characters.root_slice().capacity() == constant_character_view.size()
        && std::string_view(constant_character_view.data(), text.size()) == text,
        "const string view differs");
    const std::array<uint32_t, 4> input{2, 3, 5, 7};
    SliceT<AlignedNumbers> numbers(true);
    std::memcpy(numbers.raw(), input.data(), sizeof(input));
    auto& view = numbers.get_as().numbers;
    require(view.size() == 16 && numbers.root_slice().capacity() / sizeof(uint32_t) == view.size() && view[3] == 7,
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
    require(weak, "weak slice has incorrect identity");
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
    require(middle.size_bytes() == 2 * sizeof(uint32_t) && middle.data<uint32_t>()[0] == 22 &&
        middle.data<uint32_t>()[1] == 33, "weak subrange copy has the wrong offset");
    require(suffix.size_bytes() == 2 * sizeof(uint32_t) && suffix.get_as<uint32_t>() == 33,
        "weak default-length subrange did not copy the suffix");
    require(clamped.size_bytes() == 3 * sizeof(uint32_t) && clamped.get_as<uint32_t>() == 22,
        "weak length overflow did not clamp to the available bytes");
    input.fill(0);
    require(whole.get_as<uint32_t>() == 12 && middle.get_as<uint32_t>() == 22,
        "weak conversion did not create independent storage");
    require_throws([&] { weak.slice(sizeof(input) + 1); }, "weak invalid offset accepted");
    require_throws([] { WeakSlice invalid(size_t(4), true); }, "weak allocation accepted");
}
/** --------------------------------------------------------------------------------------------------------- Resizes
 * @brief Checks that a preserving shrink stays in place, growth keeps the bytes, a discarding resize does not
 * copy, a null Slice resizes into a claim, a novel resize gets its own buffer, and zero frees.
 */
static void resizes() {
    Slice grown(64);
    grown.get_as<uint64_t>() = 0xA5A5u;
    grown.resize(256);
    require(grown.size_bytes() == 256 && grown.get_as<uint64_t>() == 0xA5A5u, "a preserving growth lost its bytes");
    const void* after = grown.raw();
    grown.resize(128);
    require(grown.size_bytes() == 128 && grown.raw() == after, "a preserving shrink did not stay in place");
    grown.resize(64, false);
    require(grown.size_bytes() == 64, "a discarding resize has the wrong size");
    Slice revived;
    revived.resize(96);
    require(revived.valid() && revived.size_bytes() == 96, "resize on a null slice did not allocate");
    Slice novel(64);
    novel.resize(4096, true, true);
    require(novel.is_novel() && novel.size_bytes() == 4096, "a novel resize did not get its own buffer");
    revived.resize(0);
    require(revived.is_null(), "resize to zero did not free");
    const std::array<uint32_t, 4> source{5, 6, 7, 8};
    Slice copied(source.data(), sizeof(source));
    require(copied.size() == sizeof(source) && copied.size<uint32_t>() == 4 && copied.data()[0] == 5
        && copied.data<uint32_t>()[3] == 8 && copied.raw() != source.data(),
        "a copy from a view did not copy the bytes into its own claim");
    Slice novel_copy(source.data(), sizeof(source), true);
    require(novel_copy.is_novel() && novel_copy.get_as<uint32_t>() == 5, "a novel copy from a view differs");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Dispatches independent typed and weak slice functional cases.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {
        {"typed_slices", typed_slices}, {"typed_lifecycle", typed_lifecycle},
        {"typed_views", typed_views},
        {"weak_slices", weak_slices}, {"resizes", resizes}
    });
}
