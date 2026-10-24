/** --------------------------------------------------------------------------------------------------------- Chomp Functional Test
 * @file chomp_functional_test.cpp
 * @brief Exercises typed access, non-owning views, copies, and ownership through public APIs.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <span>
#include <string_view>
#include <utility>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
/** --------------------------------------------------------------------------------------------------------- Aligned Value
 * @brief Stores a test value in one aligned chomp granule.
 */
struct alignas(64) AlignedValue {
    uint32_t value;
};
/** --------------------------------------------------------------------------------------------------------- Aligned Text
 * @brief Stores character data in one aligned chomp granule.
 */
struct alignas(64) AlignedText {
    std::array<char, 64> characters;
};
/** --------------------------------------------------------------------------------------------------------- Aligned Numbers
 * @brief Stores integer data in one aligned chomp granule.
 */
struct alignas(64) AlignedNumbers {
    std::array<uint32_t, 16> numbers;
};
/** --------------------------------------------------------------------------------------------------------- Chomp Element
 * @brief Checks whether an element type satisfies the typed chomp constraint.
 */
template<typename Element>
concept ChompElement = requires { typename ChompT<Element>; };
static_assert(ChompElement<AlignedValue>);
static_assert(ChompElement<AlignedText>);
static_assert(ChompElement<AlignedNumbers>);
static_assert(!ChompElement<uint32_t>);
static_assert(!ChompElement<uint64_t>);
static_assert(!ChompElement<std::string_view>);
static_assert(!ChompElement<std::span<uint32_t>>);
/** --------------------------------------------------------------------------------------------------------- Typed Chomps
 * @brief Checks element access, byte views, placement, and shared ownership of a one-element typed claim.
 */
static void typed_chomps() {
    ChompT<AlignedValue> empty;
    require(empty.is_null() && empty.length() == 0, "typed default is not empty");
    ChompT<AlignedValue> value(true);
    require(value.size() == 1 && value.length() == 1, "a typed claim is not one element");
    require(value.size_bytes() == sizeof(AlignedValue), "typed byte count is incorrect");
    require(reinterpret_cast<uintptr_t>(value.data()) % alignof(AlignedValue) == 0,
        "typed storage is not aligned");
    require(value.root_chomp().placement() == Chomp::default_placement(),
        "typed placement differs");
    value->value = 10;
    require(value.get_as().value == 10 && (*value).value == 10,
        "typed references read the wrong value");
    const auto& constant = value;
    require(constant.get_as().value == 10 && constant->value == 10, "const access differs");
    ChompT<AlignedValue> copy = value;
    ChompT<AlignedValue> moved = std::move(copy);
    require(copy.is_null() && copy.size_bytes() == 0, "typed move retained its source");
    Chomp subview = value.chomp(0, sizeof(uint32_t));
    value.free();
    require(moved->value == 10 && subview.get_as<uint32_t>() == 10 && subview.size_bytes() == 64,
        "typed shared ownership did not survive free");
    empty = moved;
    copy = std::move(empty);
    require(empty.is_null() && copy.raw() == moved.raw(), "typed assignment lost ownership");
    ChompT<AlignedValue> adopted{Chomp(sizeof(AlignedValue))};
    require(adopted.size() == 1 && adopted.root_chomp().placement() == Chomp::default_placement(),
        "adopting a raw granule did not keep its slot");
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
        ChompT<CountedValue> value(true);
        require(CountedValue::live.load() == 1 && value->value == 7,
            "a typed claim did not construct its element");
        ChompT<CountedValue> shared = value;
        require(shared.root_chomp().id() == value.root_chomp().id(),
            "a typed copy did not share its slot");
        Chomp view = value.chomp(0, sizeof(uint64_t));
        value.free();
        require(CountedValue::live.load() == 1,
            "a typed slot destroyed its element while a copy remained");
        shared.free();
        require(CountedValue::live.load() == 0 && view.size_bytes() == 64,
            "the last typed handle did not destroy its element, or a view of its buffer lost the buffer");
    }
    {
        ChompT<CountedValue> single(true, true);
        require(CountedValue::live.load() == 1 && single.get_as().value == 7,
            "a novel typed chomp did not construct its element");
    }
    require(CountedValue::live.load() == 0, "a novel typed chomp did not destroy its element on release");
}
/** --------------------------------------------------------------------------------------------------------- Typed Views
 * @brief Checks that aligned payload references provide live views of the backing bytes.
 */
static void typed_views() {
    constexpr std::string_view text = "abcdefghijklmnop";
    ChompT<AlignedText> characters{Chomp(text.data(), text.size())};
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
    ChompT<AlignedNumbers> numbers{Chomp(input.data(), sizeof(input))};
    auto& view = numbers.get_as().numbers;
    require(view.size() == 16 && numbers.size<uint32_t>() == view.size() && view[3] == 7,
        "array view does not cover the payload");
    view[1] = 11;
    const auto& constant_numbers = numbers;
    auto& constant_view = constant_numbers.get_as().numbers;
    static_assert(std::is_const_v<std::remove_reference_t<decltype(constant_view[0])>>);
    require(constant_view[1] == 11, "array view writes did not reach the backing storage");
}
/** --------------------------------------------------------------------------------------------------------- Weak Chomps
 * @brief Checks borrowed memory access and deep copies of full and partial byte ranges.
 */
static void weak_chomps() {
    std::array<uint32_t, 4> input{11, 22, 33, 44};
    WeakChomp weak(input.data(), sizeof(input));
    require(weak && weak.placement() == nullptr, "weak chomp has incorrect identity");
    require(weak.size<uint32_t>() == input.size(), "weak element count differs");
    require(weak.data<uint32_t>() == input.data(), "weak data did not alias its input");
    (*weak.data<uint32_t>()) = 12;
    const auto& constant = weak;
    require(*constant.data<uint32_t>() == 12 && constant.data<uint32_t>()[3] == 44,
        "const weak access differs");
    Chomp whole = static_cast<Chomp>(weak);
    Chomp middle = weak.chomp(sizeof(uint32_t), 2 * sizeof(uint32_t));
    Chomp suffix = weak.chomp(2 * sizeof(uint32_t));
    Chomp clamped = weak.chomp(sizeof(uint32_t), SIZE_MAX - 1);
    require(middle.size_bytes() == 64 && middle.data<uint32_t>()[0] == 22 &&
        middle.data<uint32_t>()[1] == 33, "weak subrange copy has the wrong offset");
    require(suffix.size_bytes() == 64 && suffix.get_as<uint32_t>() == 33,
        "weak default-length subrange did not copy the suffix");
    require(clamped.size_bytes() == 64 && clamped.get_as<uint32_t>() == 22,
        "weak length overflow did not clamp to the available bytes");
    input.fill(0);
    require(whole.get_as<uint32_t>() == 12 && middle.get_as<uint32_t>() == 22,
        "weak conversion did not create independent storage");
    require_throws([&] { weak.chomp(sizeof(input) + 1); }, "weak invalid offset accepted");
    require_throws([] { WeakChomp invalid(size_t(4), true); }, "weak allocation accepted");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Dispatches independent typed and weak chomp functional cases.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {
        {"typed_chomps", typed_chomps}, {"typed_lifecycle", typed_lifecycle},
        {"typed_views", typed_views},
        {"weak_chomps", weak_chomps}
    });
}
