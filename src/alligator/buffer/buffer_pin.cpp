#include <alligator/buffer/chain_buffer.hpp>

namespace alligator {

BufferPin::~BufferPin() {
    if (buffer_) {
        buffer_->pinned_.store(false, std::memory_order_release);
        buffer_ = nullptr;  // Clear the pointer to avoid dangling reference
    }
}

}