/** --------------------------------------------------------------------------------------------------------- Priority Slice
 * @file priorityslice.cpp
 * @brief Bounded lock-free priority queue over packed slot words: constant-time rejection from
 * the header, and in natural order a per-block cache that confines every scan to one block.
 */
#include <alligator.hpp>
#include "simd.hpp"

namespace buffetalligator {
namespace {
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
 * @brief The atomic view of one word.
 * @param words The words.
 * @param index The word.
 * @return The word's atomic.
 */
inline std::atomic<uint64_t>& slot(uint64_t* words, size_t index) noexcept {
    return *reinterpret_cast<std::atomic<uint64_t>*>(words + index);
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
inline void raise(std::atomic<uint64_t>& cache, uint64_t word) noexcept {
    uint64_t current = cache.load(std::memory_order_relaxed);
    while (current < word && !cache.compare_exchange_weak(
        current, word, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
/** --------------------------------------------------------------------------------------------------------- Lower
 * @brief Drops a cache word to at most the given word.
 * @param cache The cache word.
 * @param word The ceiling.
 */
inline void lower(std::atomic<uint64_t>& cache, uint64_t word) noexcept {
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
    const Placemat* placement
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
    capacity_ = (bytes - HEADER_BYTES) / sizeof(uint64_t);
    while (header_bytes(capacity_) + capacity_ * sizeof(uint64_t) > bytes) --capacity_;
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
    std::atomic<uint64_t>& threshold = slot(header_, THRESHOLD);
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
    (void)SIMDMisc::max_index(max_cache_, blocks_, threshold, runner_up);
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
    const int64_t ahead =
        SIMDMisc::find_id(words_ + hint, capacity_ - hint, static_cast<int64_t>(EMPTY));
    if (ahead >= 0) return ahead + static_cast<int64_t>(hint);
    return SIMDMisc::find_id(words_, hint, static_cast<int64_t>(EMPTY));
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
    std::atomic<uint64_t>& threshold = slot(header_, THRESHOLD);
    std::atomic<uint64_t>& free_slots = slot(header_, FREE_SLOTS);
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
            block = SIMDMisc::max_index(max_cache_, blocks_, cached, cached_runner_up);
        }
        uint64_t* block_words = words_ + block * BLOCK;
        uint64_t worst;
        uint64_t runner_up;
        const size_t offset =
            SIMDMisc::max_index(block_words, block_length(block), worst, runner_up);
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
    std::atomic<uint64_t>& threshold = slot(header_, THRESHOLD);
    std::atomic<uint64_t>& free_slots = slot(header_, FREE_SLOTS);
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
            block = SIMDMisc::min_index(min_cache_, blocks_, cached);
            if (cached == EMPTY) {
                const size_t index = SIMDMisc::min_index(words_, capacity_, best);
                if (best == EMPTY) return EMPTY;
                lower(slot(min_cache_, index / BLOCK), best);
                continue;
            }
        }
        uint64_t* block_words = words_ + block * BLOCK;
        uint64_t runner_up;
        const size_t offset =
            SIMDMisc::min_index(block_words, block_length(block), best, runner_up);
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
            std::atomic<uint64_t>& block_max = slot(max_cache_, block);
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
        (void)SIMDMisc::min_index(words_ + block * BLOCK, block_length(block), next);
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
        (void)SIMDMisc::min_index(words_, capacity_, best);
    } else {
        (void)best_ordered(best);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
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
        (void)SIMDMisc::max_index(words_, capacity_, worst, runner_up);
    } else {
        (void)worst_ordered(worst, runner_up);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
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
    for (size_t block = 0; block < blocks_; ++block) {
        slot(max_cache_, block).store(0, std::memory_order_relaxed);
        slot(min_cache_, block).store(EMPTY, std::memory_order_relaxed);
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
    for (size_t index = 0; index < capacity_; ++index) count += words_[index] != EMPTY;
    return count;
}
} // namespace buffetalligator
