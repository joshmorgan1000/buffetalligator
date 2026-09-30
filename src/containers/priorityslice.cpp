/** --------------------------------------------------------------------------------------------------------- Priority Slice
 * @file priorityslice.cpp
 * @brief Bounded lock-free priority queue over packed slot words: constant-time rejection from
 * the header, and in natural order a per-block cache that confines every scan to one block.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <simd.hpp>

namespace buffetalligator {
namespace {
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
/// @brief Header word caching the current worst word, EMPTY whenever a slot may be free.
constexpr size_t THRESHOLD = 0;
/// @brief Header word counting free slots.
constexpr size_t FREE_SLOTS = 1;
/// @brief Header word identifying the last slot freed by a pop or take.
constexpr size_t FREE_HINT = 2;
/// @brief First of four header words remembering recently freed slots.
constexpr size_t FREE_RING = 3;
/// @brief Mask selecting a ring word from a slot index.
constexpr size_t RING_MASK = 3;
/// @brief Words in the fixed header.
constexpr size_t HEADER_WORDS = PrioritySlice::HEADER_BYTES / sizeof(uint64_t);
/// @brief Slot words per cached block.
constexpr size_t BLOCK = PrioritySlice::BLOCK_WORDS;
/** --------------------------------------------------------------------------------------------------------- Slot
 * @brief The atomic view of one Slice-backed word.
 * @param words The words.
 * @param index The word.
 * @return The word's atomic.
 */
inline std::atomic_ref<uint64_t> slot(uint64_t* words, size_t index) noexcept {
    return std::atomic_ref<uint64_t>(words[index]);
}
/** --------------------------------------------------------------------------------------------------------- Atomic Find
 * @brief Finds a word in atomic snapshots using the existing SIMD search.
 * @param words The shared words.
 * @param size The number of words.
 * @param value The sought word.
 * @return The matching index, or -1.
 */
int64_t atomic_find(uint64_t* words, size_t size, uint64_t value) noexcept {
    uint64_t snapshot[PrioritySlice::BLOCK_WORDS];
    for (size_t base = 0; base < size; base += PrioritySlice::BLOCK_WORDS) {
        const size_t length = std::min(PrioritySlice::BLOCK_WORDS, size - base);
        for (size_t offset = 0; offset < length; ++offset) {
            snapshot[offset] = slot(words, base + offset).load(std::memory_order_relaxed);
        }
        const int64_t found = SIMDMisc::find_id(snapshot, length, static_cast<int64_t>(value));
        if (found >= 0) return static_cast<int64_t>(base) + found;
    }
    return -1;
}
/** --------------------------------------------------------------------------------------------------------- Atomic Max Index
 * @brief Finds the largest and second largest words in atomic snapshots with SIMD reductions.
 * @param words The shared words.
 * @param size The number of words, at least one.
 * @param maximum Receives the largest word.
 * @param runner_up Receives the second largest word.
 * @return The largest word's index.
 */
size_t atomic_max_index(
    uint64_t* words,
    size_t size,
    uint64_t& maximum,
    uint64_t& runner_up
) noexcept {
    uint64_t snapshot[PrioritySlice::BLOCK_WORDS];
    const size_t first_length = std::min(PrioritySlice::BLOCK_WORDS, size);
    for (size_t offset = 0; offset < first_length; ++offset) {
        snapshot[offset] = slot(words, offset).load(std::memory_order_relaxed);
    }
    size_t index = SIMDMisc::max_index(snapshot, first_length, maximum, runner_up);
    for (size_t base = PrioritySlice::BLOCK_WORDS; base < size;
         base += PrioritySlice::BLOCK_WORDS) {
        const size_t length = std::min(PrioritySlice::BLOCK_WORDS, size - base);
        for (size_t offset = 0; offset < length; ++offset) {
            snapshot[offset] = slot(words, base + offset).load(std::memory_order_relaxed);
        }
        uint64_t local_maximum;
        uint64_t local_runner_up;
        const size_t local_index = SIMDMisc::max_index(
            snapshot, length, local_maximum, local_runner_up);
        if (local_maximum > maximum) {
            runner_up = std::max(maximum, local_runner_up);
            maximum = local_maximum;
            index = base + local_index;
        } else if (local_maximum > runner_up) {
            runner_up = local_maximum;
        }
    }
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Atomic Min Index
 * @brief Finds the smallest word in atomic snapshots with SIMD reductions.
 * @param words The shared words.
 * @param size The number of words, at least one.
 * @param minimum Receives the smallest word.
 * @return The smallest word's index.
 */
size_t atomic_min_index(uint64_t* words, size_t size, uint64_t& minimum) noexcept {
    uint64_t snapshot[PrioritySlice::BLOCK_WORDS];
    const size_t first_length = std::min(PrioritySlice::BLOCK_WORDS, size);
    for (size_t offset = 0; offset < first_length; ++offset) {
        snapshot[offset] = slot(words, offset).load(std::memory_order_relaxed);
    }
    size_t index = SIMDMisc::min_index(snapshot, first_length, minimum);
    for (size_t base = PrioritySlice::BLOCK_WORDS; base < size;
         base += PrioritySlice::BLOCK_WORDS) {
        const size_t length = std::min(PrioritySlice::BLOCK_WORDS, size - base);
        for (size_t offset = 0; offset < length; ++offset) {
            snapshot[offset] = slot(words, base + offset).load(std::memory_order_relaxed);
        }
        uint64_t local_minimum;
        const size_t local_index = SIMDMisc::min_index(snapshot, length, local_minimum);
        if (local_minimum < minimum) {
            minimum = local_minimum;
            index = base + local_index;
        }
    }
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Atomic Min Index With Runner-Up
 * @brief Finds the two smallest words in atomic snapshots with SIMD reductions.
 * @param words The shared words.
 * @param size The number of words, at least one.
 * @param minimum Receives the smallest word.
 * @param runner_up Receives the second smallest word.
 * @return The smallest word's index.
 */
size_t atomic_min_index(
    uint64_t* words,
    size_t size,
    uint64_t& minimum,
    uint64_t& runner_up
) noexcept {
    uint64_t snapshot[PrioritySlice::BLOCK_WORDS];
    const size_t first_length = std::min(PrioritySlice::BLOCK_WORDS, size);
    for (size_t offset = 0; offset < first_length; ++offset) {
        snapshot[offset] = slot(words, offset).load(std::memory_order_relaxed);
    }
    size_t index = SIMDMisc::min_index(snapshot, first_length, minimum, runner_up);
    for (size_t base = PrioritySlice::BLOCK_WORDS; base < size;
         base += PrioritySlice::BLOCK_WORDS) {
        const size_t length = std::min(PrioritySlice::BLOCK_WORDS, size - base);
        for (size_t offset = 0; offset < length; ++offset) {
            snapshot[offset] = slot(words, base + offset).load(std::memory_order_relaxed);
        }
        uint64_t local_minimum;
        uint64_t local_runner_up;
        const size_t local_index = SIMDMisc::min_index(
            snapshot, length, local_minimum, local_runner_up);
        if (local_minimum < minimum) {
            runner_up = std::min(minimum, local_runner_up);
            minimum = local_minimum;
            index = base + local_index;
        } else if (local_minimum < runner_up) {
            runner_up = local_minimum;
        }
    }
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Cache Words
 * @brief Words in each block cache, padded to a cache line.
 * @param capacity The slot count.
 * @return The words per cache.
 */
inline size_t cache_words(size_t capacity) noexcept {
    const size_t caches = PrioritySlice::header_bytes(capacity) - PrioritySlice::HEADER_BYTES;
    return caches / 2 / sizeof(uint64_t);
}
/** --------------------------------------------------------------------------------------------------------- Checked Bytes
 * @brief Sizes the storage for a capacity that must hold at least one entry.
 * @param capacity The entry count.
 * @return The storage size in bytes.
 */
size_t checked_bytes(size_t capacity) {
    if (capacity == 0) ALLIGATOR_THROW("PrioritySlice: capacity must hold at least one entry");
    return PrioritySlice::header_bytes(capacity) + capacity * sizeof(uint64_t);
}
/** --------------------------------------------------------------------------------------------------------- Raise
 * @brief Lifts a cache word to at least the given word.
 * @param cache The cache word.
 * @param word The floor.
 */
inline void raise(std::atomic_ref<uint64_t> cache, uint64_t word) noexcept {
    uint64_t current = cache.load(std::memory_order_relaxed);
    while (current < word && !cache.compare_exchange_weak(
        current, word, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
/** --------------------------------------------------------------------------------------------------------- Lower
 * @brief Drops a cache word to at most the given word.
 * @param cache The cache word.
 * @param word The ceiling.
 */
inline void lower(std::atomic_ref<uint64_t> cache, uint64_t word) noexcept {
    uint64_t current = cache.load(std::memory_order_relaxed);
    while (word < current && !cache.compare_exchange_weak(
        current, word, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Claims storage for a fixed number of entries and marks every slot free.
 * @param capacity The entry count the queue trims itself to.
 * @param compare The key order, or nullptr for the natural unsigned word order.
 * @param novel_buffer Whether the storage is its own buffer rather than a slab claim.
 * @param placement The placement the storage is claimed from.
 */
PrioritySlice::PrioritySlice(
    size_t capacity,
    Compare compare,
    bool novel_buffer,
    const BuffetDescriptor* placement
) : storage_(checked_bytes(capacity), novel_buffer, placement)
, header_(storage_.data<uint64_t>())
, max_cache_(header_ + HEADER_WORDS)
, min_cache_(max_cache_ + cache_words(capacity))
, words_(min_cache_ + cache_words(capacity))
, capacity_(capacity)
, blocks_((capacity + BLOCK - 1) / BLOCK)
, compare_(compare) {
    std::fill_n(words_, capacity_, EMPTY);
    std::fill_n(max_cache_, blocks_, uint64_t(0));
    std::fill_n(min_cache_, blocks_, EMPTY);
    header_[THRESHOLD] = EMPTY;
    header_[FREE_SLOTS] = capacity_;
    header_[FREE_HINT] = 0;
    header_[CAPACITY_WORD] = capacity_;
    for (size_t entry = 0; entry <= RING_MASK; ++entry) header_[FREE_RING + entry] = 0;
}
/** --------------------------------------------------------------------------------------------------------- Constructor - Adopt
 * @brief Views existing slot words behind the header and rebuilds the header and caches.
 * @param storage The Slice holding the header, the block caches, and the slot words.
 * @param compare The key order, or nullptr for the natural unsigned word order.
 */
PrioritySlice::PrioritySlice(Slice storage, Compare compare)
: storage_(std::move(storage))
, header_(storage_.data<uint64_t>())
, compare_(compare) {
    const size_t bytes = storage_.size_bytes();
    if (bytes < header_bytes(1) + sizeof(uint64_t)) {
        ALLIGATOR_THROW("PrioritySlice: storage must hold the header and at least one slot word");
    }
    capacity_ = header_[CAPACITY_WORD];
    if (capacity_ == 0 || capacity_ > (bytes - HEADER_BYTES) / sizeof(uint64_t)
        || header_bytes(capacity_) + capacity_ * sizeof(uint64_t) > bytes) {
        ALLIGATOR_THROW("PrioritySlice: stored capacity exceeds its represented storage");
    }
    blocks_ = (capacity_ + BLOCK - 1) / BLOCK;
    max_cache_ = header_ + HEADER_WORDS;
    min_cache_ = max_cache_ + cache_words(capacity_);
    words_ = min_cache_ + cache_words(capacity_);
    size_t free_slots = 0;
    size_t free_hint = 0;
    for (size_t index = 0; index < capacity_; ++index) {
        if (words_[index] == EMPTY) {
            ++free_slots;
            free_hint = index;
        }
    }
    for (size_t block = 0; block < blocks_; ++block) {
        uint64_t runner_up;
        uint64_t* block_words = words_ + block * BLOCK;
        (void)SIMDMisc::max_index(block_words, block_length(block), max_cache_[block], runner_up);
        (void)SIMDMisc::min_index(block_words, block_length(block), min_cache_[block]);
    }
    header_[THRESHOLD] = EMPTY;
    header_[FREE_SLOTS] = free_slots;
    header_[FREE_HINT] = free_hint;
    for (size_t entry = 0; entry <= RING_MASK; ++entry) header_[FREE_RING + entry] = 0;
}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Takes the storage and leaves the source with no slots.
 * @param other The queue to move from.
 */
PrioritySlice::PrioritySlice(PrioritySlice&& other) noexcept
: storage_(std::move(other.storage_))
, header_(other.header_)
, max_cache_(other.max_cache_)
, min_cache_(other.min_cache_)
, words_(other.words_)
, capacity_(other.capacity_)
, blocks_(other.blocks_)
, compare_(other.compare_) {
    other.header_ = nullptr;
    other.max_cache_ = nullptr;
    other.min_cache_ = nullptr;
    other.words_ = nullptr;
    other.capacity_ = 0;
    other.blocks_ = 0;
}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases this storage, takes the source's, and leaves the source with no slots.
 * @param other The queue to move from.
 * @return This queue.
 */
PrioritySlice& PrioritySlice::operator=(PrioritySlice&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        header_ = other.header_;
        max_cache_ = other.max_cache_;
        min_cache_ = other.min_cache_;
        words_ = other.words_;
        capacity_ = other.capacity_;
        blocks_ = other.blocks_;
        compare_ = other.compare_;
        other.header_ = nullptr;
        other.max_cache_ = nullptr;
        other.min_cache_ = nullptr;
        other.words_ = nullptr;
        other.capacity_ = 0;
        other.blocks_ = 0;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Block Length
 * @brief Slot words in a block, shorter only for the last one.
 * @param block The block.
 * @return The block's slot count.
 */
size_t PrioritySlice::block_length(size_t block) const noexcept {
    return block + 1 == blocks_ ? capacity_ - block * BLOCK : BLOCK;
}
/** --------------------------------------------------------------------------------------------------------- Better
 * @brief Decides whether a word pops before another, with a free slot losing to anything.
 * @param word The candidate word.
 * @param than The word it is measured against.
 * @return True when the candidate pops first.
 */
bool PrioritySlice::better(uint64_t word, uint64_t than) const noexcept {
    if (compare_ == nullptr) return word < than;
    if (than == EMPTY) return true;
    const uint32_t left = key_of(word);
    const uint32_t right = key_of(than);
    return compare_(&left, &right) < 0;
}
/** --------------------------------------------------------------------------------------------------------- Worse Of
 * @brief Picks the word that pops later, with EMPTY standing for no word at all.
 * @param left One word, or EMPTY.
 * @param right The other word.
 * @return The word evicted first of the two.
 */
uint64_t PrioritySlice::worse_of(uint64_t left, uint64_t right) const noexcept {
    if (compare_ == nullptr) return left > right ? left : right;
    if (left == EMPTY) return right;
    return better(left, right) ? right : left;
}
/** --------------------------------------------------------------------------------------------------------- Arm Ordered
 * @brief Loosens the comparator-order threshold to a word that took a free slot, so the threshold
 * never claims a better worst entry than the queue holds.
 * @param word The word that entered.
 */
void PrioritySlice::arm_ordered(uint64_t word) noexcept {
    std::atomic_ref<uint64_t> threshold = slot(header_, THRESHOLD);
    uint64_t current = threshold.load(std::memory_order_relaxed);
    while (current != EMPTY && better(current, word) && !threshold.compare_exchange_weak(
        current, word, std::memory_order_release, std::memory_order_relaxed)) {}
}
/** --------------------------------------------------------------------------------------------------------- Arm Natural
 * @brief Publishes the largest block maximum as the threshold.
 */
void PrioritySlice::arm_natural() noexcept {
    uint64_t threshold;
    uint64_t runner_up;
    (void)atomic_max_index(max_cache_, blocks_, threshold, runner_up);
    arm_natural(threshold);
}
/** --------------------------------------------------------------------------------------------------------- Arm Natural With
 * @brief Publishes a known largest block maximum as the threshold.
 * @param threshold The largest word any block cache holds.
 */
void PrioritySlice::arm_natural(uint64_t threshold) noexcept {
    slot(header_, THRESHOLD).store(threshold, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Advance Hint
 * @brief Points the free hint past a slot that was just filled, so a run of fills walks forward.
 * @param filled The slot just filled.
 */
void PrioritySlice::advance_hint(size_t filled) noexcept {
    const size_t next = filled + 1 == capacity_ ? 0 : filled + 1;
    slot(header_, FREE_HINT).store(next, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Note Free
 * @brief Records a freed slot in the hint and the ring, then publishes the free count.
 * @param index The slot just freed.
 */
void PrioritySlice::note_free(size_t index) noexcept {
    slot(header_, FREE_HINT).store(index, std::memory_order_relaxed);
    slot(header_, FREE_RING + (index & RING_MASK)).store(index, std::memory_order_relaxed);
    slot(header_, FREE_SLOTS).fetch_add(1, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Free From Ring
 * @brief Returns a recently freed slot that still reads free, or -1.
 * @return The free slot.
 */
int64_t PrioritySlice::free_from_ring() const noexcept {
    for (size_t entry = 0; entry <= RING_MASK; ++entry) {
        const size_t index = slot(header_, FREE_RING + entry).load(std::memory_order_relaxed);
        if (slot(words_, index).load(std::memory_order_relaxed) == EMPTY) {
            return static_cast<int64_t>(index);
        }
    }
    return -1;
}
/** --------------------------------------------------------------------------------------------------------- Free Slot After
 * @brief Finds a free slot starting at the hint and wrapping around, or -1 when none is free.
 * @param hint The slot to start from.
 * @return The free slot.
 */
int64_t PrioritySlice::free_slot_after(size_t hint) const noexcept {
    const int64_t ahead = atomic_find(words_ + hint, capacity_ - hint, EMPTY);
    if (ahead >= 0) return ahead + static_cast<int64_t>(hint);
    return atomic_find(words_, hint, EMPTY);
}
/** --------------------------------------------------------------------------------------------------------- Fill Caches
 * @brief Folds a word that took a free slot into its block's caches and arms the threshold when
 * it took the last one.
 * @param index The slot the word took.
 * @param word The word.
 */
void PrioritySlice::fill_caches(size_t index, uint64_t word) noexcept {
    if (blocks_ != 1) {
        const size_t block = index / BLOCK;
        raise(slot(max_cache_, block), word);
        lower(slot(min_cache_, block), word);
    }
    if (slot(header_, FREE_SLOTS).fetch_sub(1, std::memory_order_release) == 1 && blocks_ != 1) {
        arm_natural();
    }
}
/** --------------------------------------------------------------------------------------------------------- Worst Ordered
 * @brief Finds the slot the next comparator-order eviction takes: a free slot, or the entry that
 * pops last, along with the entry that pops second to last.
 * @param worst Receives the slot's word.
 * @param runner_up Receives the second worst word, EMPTY when there is none.
 * @return The slot.
 */
int64_t PrioritySlice::worst_ordered(uint64_t& worst, uint64_t& runner_up) const noexcept {
    worst = slot(words_, 0).load(std::memory_order_relaxed);
    int64_t index = 0;
    runner_up = EMPTY;
    if (worst == EMPTY) return 0;
    uint32_t worst_key = key_of(worst);
    uint32_t runner_up_key = 0;
    for (size_t current = 1; current < capacity_; ++current) {
        const uint64_t word = slot(words_, current).load(std::memory_order_relaxed);
        if (word == EMPTY) {
            worst = EMPTY;
            return static_cast<int64_t>(current);
        }
        const uint32_t key = key_of(word);
        if (compare_(&key, &worst_key) > 0) {
            runner_up = worst;
            runner_up_key = worst_key;
            worst = word;
            worst_key = key;
            index = static_cast<int64_t>(current);
        } else if (runner_up == EMPTY || compare_(&key, &runner_up_key) > 0) {
            runner_up = word;
            runner_up_key = key;
        }
    }
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Best Ordered
 * @brief Finds the comparator-order entry that pops first, leaving EMPTY and -1 when none.
 * @param best Receives the slot's word.
 * @return The slot.
 */
int64_t PrioritySlice::best_ordered(uint64_t& best) const noexcept {
    best = EMPTY;
    int64_t index = -1;
    uint32_t best_key = 0;
    for (size_t current = 0; current < capacity_; ++current) {
        const uint64_t word = slot(words_, current).load(std::memory_order_relaxed);
        if (word == EMPTY) continue;
        const uint32_t key = key_of(word);
        if (index < 0 || compare_(&key, &best_key) < 0) {
            best = word;
            best_key = key;
            index = static_cast<int64_t>(current);
        }
    }
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Enters a word by taking a free slot or evicting the worst entry it beats, rejecting
 * without a scan whenever the queue is full and the word does not beat the cached threshold.
 * @param word The packed entry.
 * @return EMPTY when a free slot was taken, the evicted word, or `word` itself when rejected.
 */
uint64_t PrioritySlice::push(uint64_t word) noexcept {
    return compare_ == nullptr ? push_natural(word) : push_ordered(word);
}
/** --------------------------------------------------------------------------------------------------------- Push Natural
 * @brief Natural-order push: the free hint, then a free slot, then the block the caches name.
 * @param word The packed entry.
 * @return EMPTY when a free slot was taken, the evicted word, or `word` itself when rejected.
 */
uint64_t PrioritySlice::push_natural(uint64_t word) noexcept {
    std::atomic_ref<uint64_t> threshold = slot(header_, THRESHOLD);
    std::atomic_ref<uint64_t> free_slots = slot(header_, FREE_SLOTS);
    if (free_slots.load(std::memory_order_relaxed) == 0
        && word >= threshold.load(std::memory_order_relaxed)) return word;
    while (true) {
        if (free_slots.load(std::memory_order_acquire) != 0) [[unlikely]] {
            const size_t hint = slot(header_, FREE_HINT).load(std::memory_order_relaxed);
            uint64_t expected = EMPTY;
            if (slot(words_, hint).compare_exchange_strong(
                expected, word, std::memory_order_acq_rel, std::memory_order_relaxed
            )) {
                advance_hint(hint);
                fill_caches(hint, word);
                return EMPTY;
            }
            int64_t found = free_from_ring();
            if (found < 0) found = free_slot_after(hint);
            if (found >= 0) {
                expected = EMPTY;
                if (slot(words_, static_cast<size_t>(found)).compare_exchange_strong(
                    expected, word, std::memory_order_acq_rel, std::memory_order_relaxed
                )) {
                    advance_hint(static_cast<size_t>(found));
                    fill_caches(static_cast<size_t>(found), word);
                    return EMPTY;
                }
                continue;
            }
        }
        size_t block = 0;
        uint64_t cached = EMPTY;
        uint64_t cached_runner_up = 0;
        if (blocks_ != 1) {
            block = atomic_max_index(max_cache_, blocks_, cached, cached_runner_up);
        }
        uint64_t* block_words = words_ + block * BLOCK;
        uint64_t worst;
        uint64_t runner_up;
        const size_t offset =
            atomic_max_index(block_words, block_length(block), worst, runner_up);
        if (blocks_ != 1 && worst != cached) {
            slot(max_cache_, block).compare_exchange_strong(
                cached, worst, std::memory_order_relaxed, std::memory_order_relaxed);
            continue;
        }
        if (word >= worst) {
            arm_natural(worst);
            return word;
        }
        if (!slot(block_words, offset).compare_exchange_strong(
            worst, word, std::memory_order_acq_rel, std::memory_order_relaxed
        )) continue;
        const uint64_t raised = runner_up > word ? runner_up : word;
        const uint64_t block_max = runner_up == EMPTY ? EMPTY : raised;
        if (blocks_ != 1) {
            slot(max_cache_, block).compare_exchange_strong(
                cached, block_max, std::memory_order_relaxed, std::memory_order_relaxed);
            lower(slot(min_cache_, block), word);
        }
        if (worst == EMPTY) {
            if (free_slots.fetch_sub(1, std::memory_order_release) == 1 && blocks_ != 1) {
                arm_natural();
            }
            return EMPTY;
        }
        arm_natural(cached_runner_up > block_max ? cached_runner_up : block_max);
        return worst;
    }
}
/** --------------------------------------------------------------------------------------------------------- Push Ordered
 * @brief Comparator-order push: the free hint, then a free slot, then a full scan.
 * @param word The packed entry.
 * @return EMPTY when a free slot was taken, the evicted word, or `word` itself when rejected.
 */
uint64_t PrioritySlice::push_ordered(uint64_t word) noexcept {
    std::atomic_ref<uint64_t> threshold = slot(header_, THRESHOLD);
    std::atomic_ref<uint64_t> free_slots = slot(header_, FREE_SLOTS);
    if (free_slots.load(std::memory_order_relaxed) == 0
        && !better(word, threshold.load(std::memory_order_relaxed))) return word;
    while (true) {
        uint64_t worst = EMPTY;
        uint64_t runner_up = EMPTY;
        int64_t index = -1;
        if (free_slots.load(std::memory_order_acquire) != 0) [[unlikely]] {
            const size_t hint = slot(header_, FREE_HINT).load(std::memory_order_relaxed);
            if (slot(words_, hint).compare_exchange_strong(
                worst, word, std::memory_order_acq_rel, std::memory_order_relaxed
            )) {
                advance_hint(hint);
                arm_ordered(word);
                free_slots.fetch_sub(1, std::memory_order_release);
                return EMPTY;
            }
            worst = EMPTY;
            index = free_from_ring();
            if (index < 0) index = free_slot_after(hint);
        }
        if (index < 0) index = worst_ordered(worst, runner_up);
        if (!better(word, worst)) {
            threshold.store(worst, std::memory_order_release);
            return word;
        }
        if (!slot(words_, static_cast<size_t>(index)).compare_exchange_strong(
            worst, word, std::memory_order_acq_rel, std::memory_order_relaxed
        )) continue;
        if (worst == EMPTY) {
            advance_hint(static_cast<size_t>(index));
            arm_ordered(word);
            free_slots.fetch_sub(1, std::memory_order_release);
            return EMPTY;
        }
        threshold.store(worse_of(runner_up, word), std::memory_order_release);
        return worst;
    }
}
/** --------------------------------------------------------------------------------------------------------- Pop
 * @brief Removes and returns the best entry, or EMPTY when no slot holds one.
 * @return The popped word.
 */
uint64_t PrioritySlice::pop() noexcept {
    return compare_ == nullptr ? pop_natural() : pop_ordered();
}
/** --------------------------------------------------------------------------------------------------------- Pop Natural
 * @brief Natural-order pop: the block the caches name, then one block scan and one CAS.
 * @return The popped word.
 */
uint64_t PrioritySlice::pop_natural() noexcept {
    if (capacity_ == 0) return EMPTY;
    while (true) {
        if (slot(header_, FREE_SLOTS).load(std::memory_order_acquire) == capacity_) return EMPTY;
        size_t block = 0;
        uint64_t cached = EMPTY;
        uint64_t best;
        if (blocks_ != 1) {
            block = atomic_min_index(min_cache_, blocks_, cached);
            if (cached == EMPTY) {
                const size_t index = atomic_min_index(words_, capacity_, best);
                if (best == EMPTY) return EMPTY;
                lower(slot(min_cache_, index / BLOCK), best);
                continue;
            }
        }
        uint64_t* block_words = words_ + block * BLOCK;
        uint64_t runner_up;
        const size_t offset =
            atomic_min_index(block_words, block_length(block), best, runner_up);
        if (blocks_ != 1 && best != cached) {
            slot(min_cache_, block).compare_exchange_strong(
                cached, best, std::memory_order_relaxed, std::memory_order_relaxed);
            continue;
        }
        if (best == EMPTY) return EMPTY;
        if (!slot(block_words, offset).compare_exchange_strong(
            best, EMPTY, std::memory_order_acq_rel, std::memory_order_relaxed
        )) continue;
        if (blocks_ != 1) {
            slot(min_cache_, block).store(runner_up, std::memory_order_relaxed);
            std::atomic_ref<uint64_t> block_max = slot(max_cache_, block);
            if (block_max.load(std::memory_order_relaxed) == best) {
                block_max.store(0, std::memory_order_relaxed);
            }
        } else {
            slot(header_, THRESHOLD).store(EMPTY, std::memory_order_release);
        }
        note_free(block * BLOCK + offset);
        return best;
    }
}
/** --------------------------------------------------------------------------------------------------------- Pop Ordered
 * @brief Comparator-order pop: one full scan and one CAS.
 * @return The popped word.
 */
uint64_t PrioritySlice::pop_ordered() noexcept {
    if (capacity_ == 0) return EMPTY;
    while (true) {
        if (slot(header_, FREE_SLOTS).load(std::memory_order_acquire) == capacity_) return EMPTY;
        uint64_t best;
        const int64_t index = best_ordered(best);
        if (best == EMPTY) return EMPTY;
        if (!slot(words_, static_cast<size_t>(index)).compare_exchange_strong(
            best, EMPTY, std::memory_order_acq_rel, std::memory_order_relaxed
        )) continue;
        note_free(static_cast<size_t>(index));
        return best;
    }
}
/** --------------------------------------------------------------------------------------------------------- Take
 * @brief Frees one slot and returns the word it held, EMPTY when it was already free.
 * @param index The slot.
 * @return The word the slot held.
 */
uint64_t PrioritySlice::take(size_t index) noexcept {
    const uint64_t word = slot(words_, index).exchange(EMPTY, std::memory_order_acq_rel);
    if (word == EMPTY) return EMPTY;
    if (compare_ == nullptr && blocks_ != 1) {
        const size_t block = index / BLOCK;
        uint64_t next;
        (void)atomic_min_index(words_ + block * BLOCK, block_length(block), next);
        slot(min_cache_, block).store(next, std::memory_order_relaxed);
        uint64_t cached_max = word;
        slot(max_cache_, block).compare_exchange_strong(
            cached_max, uint64_t(0), std::memory_order_relaxed, std::memory_order_relaxed);
    } else if (compare_ == nullptr) {
        slot(header_, THRESHOLD).store(EMPTY, std::memory_order_release);
    }
    note_free(index);
    return word;
}
/** --------------------------------------------------------------------------------------------------------- Best
 * @brief Returns the best entry without removing it, or EMPTY when no slot holds one.
 * @return The best word.
 */
uint64_t PrioritySlice::best() const noexcept {
    uint64_t best;
    if (compare_ == nullptr) {
        (void)atomic_min_index(words_, capacity_, best);
    } else {
        (void)best_ordered(best);
    }
    return best;
}
/** --------------------------------------------------------------------------------------------------------- Worst
 * @brief Returns the entry the next eviction removes, or EMPTY while a slot is free.
 * @return The worst word.
 */
uint64_t PrioritySlice::worst() const noexcept {
    uint64_t worst;
    uint64_t runner_up;
    if (compare_ == nullptr) {
        (void)atomic_max_index(words_, capacity_, worst, runner_up);
    } else {
        (void)worst_ordered(worst, runner_up);
    }
    return worst;
}
/** --------------------------------------------------------------------------------------------------------- Accepts
 * @brief Reports from the header, without a scan, whether pushing this word would enter.
 * @param word The packed entry.
 * @return True when a slot is free or the word beats the cached threshold.
 */
bool PrioritySlice::accepts(uint64_t word) const noexcept {
    return slot(header_, FREE_SLOTS).load(std::memory_order_relaxed) != 0
        || better(word, slot(header_, THRESHOLD).load(std::memory_order_relaxed));
}
/** --------------------------------------------------------------------------------------------------------- Clear
 * @brief Frees every slot without releasing the values they held.
 */
void PrioritySlice::clear() noexcept {
    if (capacity_ == 0) return;
    for (size_t index = 0; index < capacity_; ++index) {
        slot(words_, index).store(EMPTY, std::memory_order_relaxed);
    }
    if (blocks_ != 1) {
        for (size_t block = 0; block < blocks_; ++block) {
            slot(max_cache_, block).store(0, std::memory_order_relaxed);
            slot(min_cache_, block).store(EMPTY, std::memory_order_relaxed);
        }
    }
    slot(header_, FREE_HINT).store(0, std::memory_order_relaxed);
    for (size_t entry = 0; entry <= RING_MASK; ++entry) {
        slot(header_, FREE_RING + entry).store(0, std::memory_order_relaxed);
    }
    slot(header_, FREE_SLOTS).store(capacity_, std::memory_order_seq_cst);
    slot(header_, THRESHOLD).store(EMPTY, std::memory_order_seq_cst);
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Counts the slots holding an entry.
 * @return The entry count.
 */
size_t PrioritySlice::size() const noexcept {
    size_t count = 0;
    for (size_t index = 0; index < capacity_; ++index) {
        count += slot(words_, index).load(std::memory_order_relaxed) != EMPTY;
    }
    return count;
}
/** --------------------------------------------------------------------------------------------------------- Heap Constructor
 * @brief Allocates the min-max heap's count header and packed positions.
 */
HeapSlice::HeapSlice(
    size_t capacity, Compare compare, bool novel_buffer, const BuffetDescriptor* placement
) : capacity_(capacity), compare_(compare) {
    if (capacity == 0 || capacity > (SIZE_MAX - HEADER_BYTES) / sizeof(uint64_t)) {
        ALLIGATOR_THROW("HeapSlice: capacity must fit at least one packed position");
    }
    storage_ = Slice(HEADER_BYTES + capacity * sizeof(uint64_t), novel_buffer, placement);
    header_ = storage_.data<uint64_t>();
    words_ = header_ + HEADER_BYTES / sizeof(uint64_t);
    *header_ = 0;
    header_[CAPACITY_WORD] = capacity_;
    std::fill_n(words_, capacity_, EMPTY);
}
/** --------------------------------------------------------------------------------------------------------- Heap Adopt
 * @brief Adopts the represented positions after a validated count header.
 */
HeapSlice::HeapSlice(Slice storage, Compare compare)
: storage_(std::move(storage)), compare_(compare) {
    if (storage_.size_bytes() < HEADER_BYTES + sizeof(uint64_t)) {
        ALLIGATOR_THROW("HeapSlice: storage must hold its header and at least one position");
    }
    header_ = storage_.data<uint64_t>();
    words_ = header_ + HEADER_BYTES / sizeof(uint64_t);
    capacity_ = header_[CAPACITY_WORD];
    if (capacity_ == 0
        || capacity_ > (storage_.size_bytes() - HEADER_BYTES) / sizeof(uint64_t)
        || *header_ > capacity_) {
        ALLIGATOR_THROW("HeapSlice: stored count or capacity exceeds its positions");
    }
}
/** --------------------------------------------------------------------------------------------------------- Heap Move Constructor
 * @brief Transfers heap backing while leaving the source empty.
 */
HeapSlice::HeapSlice(HeapSlice&& other) noexcept
: storage_(std::move(other.storage_)), header_(std::exchange(other.header_, nullptr)),
  words_(std::exchange(other.words_, nullptr)), capacity_(std::exchange(other.capacity_, 0)),
  compare_(other.compare_) {}
/** --------------------------------------------------------------------------------------------------------- Heap Move Assignment
 * @brief Replaces heap backing with the source's positions.
 */
HeapSlice& HeapSlice::operator=(HeapSlice&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        header_ = std::exchange(other.header_, nullptr);
        words_ = std::exchange(other.words_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        compare_ = other.compare_;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Heap Push
 * @brief Inserts through alternating min-max ancestors and evicts a full heap's worst entry.
 */
uint64_t HeapSlice::push(uint64_t word) noexcept {
    uint64_t displaced = EMPTY;
    if (*header_ == capacity_) {
        size_t worst_index = 0;
        if (capacity_ > 1) {
            worst_index = 1;
            if (capacity_ > 2) {
                const uint32_t left_key = key_of(words_[1]);
                const uint32_t right_key = key_of(words_[2]);
                if (compare_ ? compare_(&left_key, &right_key) < 0 : words_[1] < words_[2]) {
                    worst_index = 2;
                }
            }
        }
        const uint32_t incoming_key = key_of(word);
        const uint32_t worst_key = key_of(words_[worst_index]);
        if (compare_ ? compare_(&incoming_key, &worst_key) >= 0
                     : word >= words_[worst_index]) return word;
        displaced = take(worst_index);
    }
    size_t position = (*header_)++;
    words_[position] = word;
    if (position == 0) return displaced;
    bool minimum_level = ((std::bit_width(position + 1) - 1) & 1u) == 0;
    const size_t parent = (position - 1) / 2;
    uint32_t word_key = key_of(word);
    uint32_t parent_key = key_of(words_[parent]);
    const int order = compare_ ? compare_(&word_key, &parent_key)
        : (word > words_[parent]) - (word < words_[parent]);
    if ((minimum_level && order > 0) || (!minimum_level && order < 0)) {
        std::swap(words_[position], words_[parent]);
        position = parent;
        minimum_level = !minimum_level;
    }
    while (position >= 3) {
        const size_t ancestor = (position - 3) / 4;
        word_key = key_of(words_[position]);
        const uint32_t ancestor_key = key_of(words_[ancestor]);
        const int ancestor_order = compare_ ? compare_(&word_key, &ancestor_key)
            : (words_[position] > words_[ancestor]) - (words_[position] < words_[ancestor]);
        if (minimum_level ? ancestor_order >= 0 : ancestor_order <= 0) break;
        std::swap(words_[position], words_[ancestor]);
        position = ancestor;
    }
    return displaced;
}
/** --------------------------------------------------------------------------------------------------------- Heap Pop
 * @brief Removes the min-max heap's best packed entry.
 */
uint64_t HeapSlice::pop() noexcept { return take(0); }
/** --------------------------------------------------------------------------------------------------------- Heap Take
 * @brief Repairs alternating ancestors and descendants after removing one heap position.
 */
uint64_t HeapSlice::take(size_t index) noexcept {
    if (index >= size()) return EMPTY;
    const uint64_t removed = words_[index];
    const size_t count = --(*header_);
    const uint64_t replacement = words_[count];
    words_[count] = EMPTY;
    if (index == count) return removed;
    words_[index] = replacement;
    const bool minimum_level = ((std::bit_width(index + 1) - 1) & 1u) == 0;
    size_t position = index;
    bool ascending_minimum = minimum_level;
    if (position != 0) {
        const size_t parent = (position - 1) / 2;
        const uint32_t word_key = key_of(words_[position]);
        const uint32_t parent_key = key_of(words_[parent]);
        const int order = compare_ ? compare_(&word_key, &parent_key)
            : (words_[position] > words_[parent]) - (words_[position] < words_[parent]);
        if ((ascending_minimum && order > 0) || (!ascending_minimum && order < 0)) {
            std::swap(words_[position], words_[parent]);
            position = parent;
            ascending_minimum = !ascending_minimum;
        }
        while (position >= 3) {
            const size_t ancestor = (position - 3) / 4;
            const uint32_t candidate_key = key_of(words_[position]);
            const uint32_t ancestor_key = key_of(words_[ancestor]);
            const int ancestor_order = compare_ ? compare_(&candidate_key, &ancestor_key)
                : (words_[position] > words_[ancestor]) - (words_[position] < words_[ancestor]);
            if (ascending_minimum ? ancestor_order >= 0 : ancestor_order <= 0) break;
            std::swap(words_[position], words_[ancestor]);
            position = ancestor;
        }
    }
    position = index;
    while (position < count / 2) {
        const size_t first_child = 2 * position + 1;
        const size_t child_count = std::min(size_t(2), count - first_child);
        const size_t first_grandchild = 4 * position + 3;
        const size_t grandchild_count = first_grandchild < count
            ? std::min(size_t(4), count - first_grandchild) : 0;
        size_t selected = first_child;
        if (compare_ == nullptr) {
            uint64_t extreme = 0;
            uint64_t runner_up = 0;
            selected += minimum_level
                ? SIMDMisc::min_index(words_ + first_child, child_count, extreme)
                : SIMDMisc::max_index(words_ + first_child, child_count, extreme, runner_up);
            if (grandchild_count != 0) {
                uint64_t descendant_extreme = 0;
                const size_t descendant = first_grandchild + (minimum_level
                    ? SIMDMisc::min_index(words_ + first_grandchild,
                        grandchild_count, descendant_extreme)
                    : SIMDMisc::max_index(words_ + first_grandchild,
                        grandchild_count, descendant_extreme, runner_up));
                if (minimum_level ? descendant_extreme < extreme : descendant_extreme > extreme) {
                    selected = descendant;
                }
            }
        } else {
            for (size_t candidate = 1; candidate < child_count + grandchild_count; ++candidate) {
                const size_t descendant = candidate < child_count ? first_child + candidate
                    : first_grandchild + candidate - child_count;
                const uint32_t selected_key = key_of(words_[selected]);
                const uint32_t descendant_key = key_of(words_[descendant]);
                const int order = compare_(&descendant_key, &selected_key);
                if (minimum_level ? order < 0 : order > 0) selected = descendant;
            }
        }
        const uint32_t selected_key = key_of(words_[selected]);
        const uint32_t position_key = key_of(words_[position]);
        const int order = compare_ ? compare_(&selected_key, &position_key)
            : (words_[selected] > words_[position]) - (words_[selected] < words_[position]);
        if (minimum_level ? order >= 0 : order <= 0) break;
        std::swap(words_[selected], words_[position]);
        if (selected < first_grandchild) break;
        const size_t parent = (selected - 1) / 2;
        const uint32_t moved_key = key_of(words_[selected]);
        const uint32_t parent_key = key_of(words_[parent]);
        const int parent_order = compare_ ? compare_(&moved_key, &parent_key)
            : (words_[selected] > words_[parent]) - (words_[selected] < words_[parent]);
        if (minimum_level ? parent_order > 0 : parent_order < 0) {
            std::swap(words_[selected], words_[parent]);
        }
        position = selected;
    }
    return removed;
}
/** --------------------------------------------------------------------------------------------------------- Heap Best
 * @brief Reads the first position when the heap contains entries.
 */
uint64_t HeapSlice::best() const noexcept { return size() == 0 ? EMPTY : words_[0]; }
/** --------------------------------------------------------------------------------------------------------- Heap Worst
 * @brief Reads the largest root child only when the heap has no free positions.
 */
uint64_t HeapSlice::worst() const noexcept {
    if (capacity_ == 0 || size() != capacity_) return EMPTY;
    if (capacity_ == 1) return words_[0];
    if (capacity_ == 2) return words_[1];
    const uint32_t left_key = key_of(words_[1]);
    const uint32_t right_key = key_of(words_[2]);
    return (compare_ ? compare_(&left_key, &right_key) < 0 : words_[1] < words_[2])
        ? words_[2] : words_[1];
}
/** --------------------------------------------------------------------------------------------------------- Heap Accepts
 * @brief Checks the full heap's worst entry against a proposed packed word.
 */
bool HeapSlice::accepts(uint64_t word) const noexcept {
    if (size() < capacity_) return true;
    if (capacity_ == 0) return false;
    const uint64_t last = worst();
    const uint32_t candidate_key = key_of(word);
    const uint32_t last_key = key_of(last);
    return compare_ ? compare_(&candidate_key, &last_key) < 0 : word < last;
}
/** --------------------------------------------------------------------------------------------------------- Heap Clear
 * @brief Resets the count of this single-owner heap.
 */
void HeapSlice::clear() noexcept {
    if (capacity_ != 0) *header_ = 0;
}
/** --------------------------------------------------------------------------------------------------------- Heap Size
 * @brief Reports the count or zero for a moved-from heap.
 */
size_t HeapSlice::size() const noexcept { return capacity_ == 0 ? 0 : *header_; }
} // namespace buffetalligator
