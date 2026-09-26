/** --------------------------------------------------------------------------------------------------------- Heap Slice
 * @file heapslice.cpp
 * @brief Bounded min-max heap over packed slot words in one Slice, for a single owner.
 */
#include <alligator.hpp>

namespace buffetalligator {
namespace {
/// @brief Header word holding the element count.
constexpr size_t COUNT = 0;
/// @brief Words in the header.
constexpr size_t HEADER_WORDS = HeapSlice::HEADER_BYTES / sizeof(uint64_t);
/** --------------------------------------------------------------------------------------------------------- Min Level
 * @brief Whether a heap position sits on a minimum level, the root being one.
 * @param index The position.
 * @return True on minimum levels.
 */
inline bool min_level(size_t index) noexcept {
    return (std::bit_width(index + 1) & 1) != 0;
}
/** --------------------------------------------------------------------------------------------------------- Natural Less
 * @brief Orders words by their unsigned value.
 */
struct NaturalLess {
    bool operator()(uint64_t left, uint64_t right) const noexcept { return left < right; }
};
/** --------------------------------------------------------------------------------------------------------- Ordered Less
 * @brief Orders words by a three-way comparator over their key bits.
 */
struct OrderedLess {
    HeapSlice::Compare compare;
    bool operator()(uint64_t left, uint64_t right) const noexcept {
        const uint32_t left_key = HeapSlice::key_of(left);
        const uint32_t right_key = HeapSlice::key_of(right);
        return compare(&left_key, &right_key) < 0;
    }
};
/** --------------------------------------------------------------------------------------------------------- Heap
 * @brief Min-max heap operations over a word array with a fixed order; the count lives in a
 * register for the duration of one operation and is written back by the caller.
 * @tparam Less The strict order.
 */
template<typename Less>
struct Heap {
    uint64_t* words;  ///< The heap positions.
    size_t count;  ///< The element count.
    Less less;  ///< The order.
    /** ------------------------------------------------------------------------------------------- Swap
     * @brief Exchanges two positions.
     */
    void swap(size_t left, size_t right) noexcept {
        const uint64_t held = words[left];
        words[left] = words[right];
        words[right] = held;
    }
    /** ------------------------------------------------------------------------------------------- Bubble Up Min
     * @brief Moves a minimum-level position up while it beats its grandparent.
     * @return True when the position moved.
     */
    bool bubble_up_min(size_t index) noexcept {
        bool moved = false;
        while (index >= 3) {
            const size_t grandparent = (index - 3) / 4;
            if (!less(words[index], words[grandparent])) break;
            swap(index, grandparent);
            index = grandparent;
            moved = true;
        }
        return moved;
    }
    /** ------------------------------------------------------------------------------------------- Bubble Up Max
     * @brief Moves a maximum-level position up while its grandparent beats it.
     * @return True when the position moved.
     */
    bool bubble_up_max(size_t index) noexcept {
        bool moved = false;
        while (index >= 3) {
            const size_t grandparent = (index - 3) / 4;
            if (!less(words[grandparent], words[index])) break;
            swap(index, grandparent);
            index = grandparent;
            moved = true;
        }
        return moved;
    }
    /** ------------------------------------------------------------------------------------------- Extreme Descendant
     * @brief The child or grandchild of a position that pops first in the given direction.
     * @tparam Max Whether to look for the largest rather than the smallest.
     * @param index The position, which has at least one child.
     * @return The descendant position.
     */
    template<bool Max>
    size_t extreme_descendant(size_t index) const noexcept {
        const size_t child = 2 * index + 1;
        const size_t grandchild = 4 * index + 3;
        size_t pick = child;
        if (grandchild + 4 <= count) {
            pick = prefer<Max>(pick, child + 1);
            pick = prefer<Max>(pick, grandchild);
            pick = prefer<Max>(pick, grandchild + 1);
            pick = prefer<Max>(pick, grandchild + 2);
            pick = prefer<Max>(pick, grandchild + 3);
            return pick;
        }
        if (child + 1 < count) pick = prefer<Max>(pick, child + 1);
        for (size_t current = grandchild; current < count; ++current) {
            pick = prefer<Max>(pick, current);
        }
        return pick;
    }
    /** ------------------------------------------------------------------------------------------- Prefer
     * @brief Picks the position whose word pops first in the given direction.
     */
    template<bool Max>
    size_t prefer(size_t held, size_t candidate) const noexcept {
        const bool wins = Max ? less(words[held], words[candidate])
                              : less(words[candidate], words[held]);
        return wins ? candidate : held;
    }
    /** ------------------------------------------------------------------------------------------- Trickle Down
     * @brief Sinks a position below its extreme child or grandchild in the given direction.
     * @tparam Max Whether the position sits on a maximum level.
     */
    template<bool Max>
    void trickle_down(size_t index) noexcept {
        while (2 * index + 1 < count) {
            const size_t pick = extreme_descendant<Max>(index);
            const bool beats =
                Max ? less(words[index], words[pick]) : less(words[pick], words[index]);
            if (!beats) return;
            swap(pick, index);
            if (pick < 4 * index + 3) return;
            const size_t parent = (pick - 1) / 2;
            const bool crossed =
                Max ? less(words[pick], words[parent]) : less(words[parent], words[pick]);
            if (crossed) swap(pick, parent);
            index = pick;
        }
    }
    /** ------------------------------------------------------------------------------------------- Fix
     * @brief Restores the heap around a position whose word changed.
     */
    void fix(size_t index) noexcept {
        if (index == 0) {
            trickle_down<false>(0);
            return;
        }
        const size_t parent = (index - 1) / 2;
        if (min_level(index)) {
            if (less(words[parent], words[index])) {
                swap(index, parent);
                bubble_up_max(parent);
                trickle_down<false>(index);
            } else if (!bubble_up_min(index)) {
                trickle_down<false>(index);
            }
        } else if (less(words[index], words[parent])) {
            swap(index, parent);
            bubble_up_min(parent);
            trickle_down<true>(index);
        } else if (!bubble_up_max(index)) {
            trickle_down<true>(index);
        }
    }
    /** ------------------------------------------------------------------------------------------- Insert
     * @brief Appends a word and bubbles it into place.
     */
    void insert(uint64_t word) noexcept {
        const size_t index = count++;
        words[index] = word;
        if (index == 0) return;
        const size_t parent = (index - 1) / 2;
        if (min_level(index)) {
            if (less(words[parent], words[index])) {
                swap(index, parent);
                bubble_up_max(parent);
            } else {
                bubble_up_min(index);
            }
        } else if (less(words[index], words[parent])) {
            swap(index, parent);
            bubble_up_min(parent);
        } else {
            bubble_up_max(index);
        }
    }
    /** ------------------------------------------------------------------------------------------- Max Position
     * @brief The position holding the largest word: the larger of the root's children.
     */
    size_t max_position() const noexcept {
        if (count < 3) return count - 1;
        return less(words[1], words[2]) ? 2 : 1;
    }
    /** ------------------------------------------------------------------------------------------- Push
     * @brief Inserts when room remains, otherwise replaces the largest word when the newcomer
     * beats it.
     * @return EMPTY when inserted, the evicted word, or the newcomer itself when rejected.
     */
    uint64_t push(uint64_t word, size_t capacity) noexcept {
        if (count < capacity) {
            insert(word);
            return HeapSlice::EMPTY;
        }
        const size_t position = max_position();
        const uint64_t evicted = words[position];
        if (!less(word, evicted)) return word;
        words[position] = word;
        fix(position);
        return evicted;
    }
    /** ------------------------------------------------------------------------------------------- Pop
     * @brief Removes the smallest word.
     */
    uint64_t pop() noexcept {
        if (count == 0) return HeapSlice::EMPTY;
        const uint64_t best = words[0];
        words[0] = words[--count];
        if (count != 0) trickle_down<false>(0);
        return best;
    }
    /** ------------------------------------------------------------------------------------------- Take
     * @brief Removes the word at a position.
     */
    uint64_t take(size_t index) noexcept {
        if (index >= count) return HeapSlice::EMPTY;
        const uint64_t word = words[index];
        words[index] = words[--count];
        if (index < count) fix(index);
        return word;
    }
    /** ------------------------------------------------------------------------------------------- Worst
     * @brief The largest word, EMPTY when none.
     */
    uint64_t worst() const noexcept {
        return count == 0 ? HeapSlice::EMPTY : words[max_position()];
    }
};
/** --------------------------------------------------------------------------------------------------------- Checked Bytes
 * @brief Sizes the storage for a capacity that must hold at least one entry.
 * @param capacity The entry count.
 * @return The storage size in bytes.
 */
size_t checked_bytes(size_t capacity) {
    if (capacity == 0) ALLIGATOR_THROW("HeapSlice: capacity must hold at least one entry");
    return HeapSlice::HEADER_BYTES + capacity * sizeof(uint64_t);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Claims storage for a fixed number of entries, all free.
 * @param capacity The entry count the heap trims itself to.
 * @param compare The key order, or nullptr for the natural unsigned word order.
 * @param novel_buffer Whether the storage is its own buffer rather than a slab claim.
 * @param placement The placement the storage is claimed from.
 */
HeapSlice::HeapSlice(
    size_t capacity,
    Compare compare,
    bool novel_buffer,
    const Placemat* placement
)
: storage_(checked_bytes(capacity), novel_buffer, placement)
, header_(storage_.data<uint64_t>())
, words_(header_ + HEADER_WORDS)
, capacity_(capacity)
, compare_(compare) {
    header_[COUNT] = 0;
}
/** --------------------------------------------------------------------------------------------------------- Constructor - Adopt
 * @brief Views an existing heap: a count word in the header and the heap positions behind it.
 * @param storage The Slice holding the header and the heap positions.
 * @param compare The key order, or nullptr for the natural unsigned word order.
 */
HeapSlice::HeapSlice(Slice storage, Compare compare)
: storage_(std::move(storage))
, header_(storage_.data<uint64_t>())
, words_(header_ + HEADER_WORDS)
, capacity_(0)
, compare_(compare) {
    if (storage_.size_bytes() < HEADER_BYTES + sizeof(uint64_t)) {
        ALLIGATOR_THROW("HeapSlice: storage must hold the header and at least one word");
    }
    capacity_ = (storage_.size_bytes() - HEADER_BYTES) / sizeof(uint64_t);
    if (header_[COUNT] > capacity_) {
        ALLIGATOR_THROW("HeapSlice: the stored count exceeds the capacity");
    }
}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Takes the storage and leaves the source with no positions.
 * @param other The heap to move from.
 */
HeapSlice::HeapSlice(HeapSlice&& other) noexcept
: storage_(std::move(other.storage_))
, header_(other.header_)
, words_(other.words_)
, capacity_(other.capacity_)
, compare_(other.compare_) {
    other.header_ = nullptr;
    other.words_ = nullptr;
    other.capacity_ = 0;
}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases this storage, takes the source's, and leaves the source with no positions.
 * @param other The heap to move from.
 * @return This heap.
 */
HeapSlice& HeapSlice::operator=(HeapSlice&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        header_ = other.header_;
        words_ = other.words_;
        capacity_ = other.capacity_;
        compare_ = other.compare_;
        other.header_ = nullptr;
        other.words_ = nullptr;
        other.capacity_ = 0;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Enters a word, evicting the worst entry it beats once full.
 * @param word The packed entry.
 * @return EMPTY when inserted, the evicted word, or `word` itself when rejected.
 */
uint64_t HeapSlice::push(uint64_t word) noexcept {
    if (compare_ == nullptr) {
        Heap<NaturalLess> heap{words_, header_[COUNT], {}};
        const uint64_t displaced = heap.push(word, capacity_);
        header_[COUNT] = heap.count;
        return displaced;
    }
    Heap<OrderedLess> heap{words_, header_[COUNT], {compare_}};
    const uint64_t displaced = heap.push(word, capacity_);
    header_[COUNT] = heap.count;
    return displaced;
}
/** --------------------------------------------------------------------------------------------------------- Pop
 * @brief Removes and returns the best entry, or EMPTY when the heap is empty.
 * @return The popped word.
 */
uint64_t HeapSlice::pop() noexcept {
    if (capacity_ == 0) return EMPTY;
    if (compare_ == nullptr) {
        Heap<NaturalLess> heap{words_, header_[COUNT], {}};
        const uint64_t best = heap.pop();
        header_[COUNT] = heap.count;
        return best;
    }
    Heap<OrderedLess> heap{words_, header_[COUNT], {compare_}};
    const uint64_t best = heap.pop();
    header_[COUNT] = heap.count;
    return best;
}
/** --------------------------------------------------------------------------------------------------------- Take
 * @brief Removes the word at a heap position, EMPTY when the position is past the count.
 * @param index The heap position.
 * @return The removed word.
 */
uint64_t HeapSlice::take(size_t index) noexcept {
    if (compare_ == nullptr) {
        Heap<NaturalLess> heap{words_, header_[COUNT], {}};
        const uint64_t word = heap.take(index);
        header_[COUNT] = heap.count;
        return word;
    }
    Heap<OrderedLess> heap{words_, header_[COUNT], {compare_}};
    const uint64_t word = heap.take(index);
    header_[COUNT] = heap.count;
    return word;
}
/** --------------------------------------------------------------------------------------------------------- Best
 * @brief Returns the best entry without removing it, or EMPTY when the heap is empty.
 * @return The best word.
 */
uint64_t HeapSlice::best() const noexcept {
    return header_[COUNT] == 0 ? EMPTY : words_[0];
}
/** --------------------------------------------------------------------------------------------------------- Worst
 * @brief Returns the entry the next eviction removes, or EMPTY while room remains.
 * @return The worst word.
 */
uint64_t HeapSlice::worst() const noexcept {
    if (header_[COUNT] < capacity_) return EMPTY;
    if (compare_ == nullptr) return Heap<NaturalLess>{words_, header_[COUNT], {}}.worst();
    return Heap<OrderedLess>{words_, header_[COUNT], {compare_}}.worst();
}
/** --------------------------------------------------------------------------------------------------------- Accepts
 * @brief Reports whether pushing this word would enter the heap.
 * @param word The packed entry.
 * @return True while room remains or when the word beats the worst entry.
 */
bool HeapSlice::accepts(uint64_t word) const noexcept {
    if (header_[COUNT] < capacity_) return true;
    if (compare_ == nullptr) return word < Heap<NaturalLess>{words_, header_[COUNT], {}}.worst();
    const Heap<OrderedLess> heap{words_, header_[COUNT], {compare_}};
    return OrderedLess{compare_}(word, heap.worst());
}
/** --------------------------------------------------------------------------------------------------------- Clear
 * @brief Forgets every entry without releasing the values they held.
 */
void HeapSlice::clear() noexcept {
    if (capacity_ != 0) header_[COUNT] = 0;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief The entry count.
 * @return The entry count.
 */
size_t HeapSlice::size() const noexcept {
    return header_[COUNT];
}
} // namespace buffetalligator
