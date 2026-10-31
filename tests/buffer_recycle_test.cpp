/** --------------------------------------------------------------------------------------------------------- Buffer Recycle Test
 * @file buffer_recycle_test.cpp
 * @brief Checks that a spent chain buffer comes back cleaned when fresh has no spare, is reused from its start,
 * and is deleted when fresh already has one.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <cstdint>
#include <memory>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
/// @brief The size of each claim, so 64 of them fill one chain buffer.
constexpr size_t PIECE = 1ull << 20;
/** --------------------------------------------------------------------------------------------------------- Fill
 * @brief Claims pieces into the current buffer until one lands in the next buffer.
 * @param kept Receives every piece claimed in the current buffer.
 * @return The first piece claimed in the next buffer.
 */
Slice fill(std::vector<Slice>& kept) {
    IBuffet* current = IBuffet::current().get();
    while (true) {
        Slice piece(PIECE);
        if (piece.buffer().get() != current) return piece;
        kept.push_back(std::move(piece));
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Rolls two chain buffers over and follows the first one through recycle and reuse.
 */
int main() {
    IBuffet::Allocator& alloc = IBuffet::allocator();
    std::vector<Slice> spent_pieces;
    Slice opener(PIECE);
    IBuffet* spent = opener.buffer().get();
    opener.data<uint8_t>()[0] = 0xAB;
    spent_pieces.push_back(std::move(opener));
    Slice next_first = fill(spent_pieces);
    require(next_first.buffer().get() != spent, "filling a chain buffer did not roll over to another");
    LOG_INFO_STREAM << "Rolled over after " << spent_pieces.size() << " pieces; emptying fresh before release";
    std::vector<std::shared_ptr<IBuffet>> spares;
    std::shared_ptr<IBuffet> spare;
    while (alloc.fresh.try_dequeue(spare)) spares.push_back(std::move(spare));
    spent_pieces.clear();
    require(alloc.fresh.size_approx() == 1, "a spent chain buffer did not come back to an empty fresh queue");
    require(alloc.fresh.try_dequeue(spare) && spare.get() == spent, "fresh did not receive the spent buffer");
    require(static_cast<const uint8_t*>(spare->host_ptr())[0] == 0, "the recycled buffer was not zeroed");
    alloc.fresh.enqueue(std::move(spare));
    std::vector<Slice> next_pieces;
    next_pieces.push_back(std::move(next_first));
    Slice reused = fill(next_pieces);
    require(reused.buffer().get() == spent && reused.raw() == spent->host_ptr(),
        "the recycled buffer was not reused from its start");
    const size_t spares_before = alloc.fresh.size_approx();
    next_pieces.clear();
    require(alloc.fresh.size_approx() == spares_before,
        "a spent buffer was kept although fresh already had a spare");
    LOG_INFO_STREAM << "A spent chain buffer was cleaned, reused from its start, and deleted when a spare existed";
    return 0;
}
