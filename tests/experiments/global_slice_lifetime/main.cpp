/** --------------------------------------------------------------------------------------------------------- Global Slice Lifetime
 * @file main.cpp
 * @brief Reproduces teardown of a pre-main null Slice first populated during main.
 */
#include <alligator.hpp>
#include <cstddef>
#include <cstdint>

buffetalligator::Slice retained;
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Leaves one public Slice owner alive for its static destructor.
 */
int main() {
    retained = buffetalligator::Slice(size_t{64}, true,
        buffetalligator::BuffetDescriptors::get(buffetalligator::AlignedHeapBuffer::type_idx()));
    retained.get_as<uint64_t>() = 73;
    LOG_INFO_STREAM << "Leaving one pre-main Slice populated for static teardown";
    return retained.get_as<uint64_t>() == 73 ? 0 : 1;
}
