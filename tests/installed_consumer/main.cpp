/** --------------------------------------------------------------------------------------------------------- Installed Consumer
 * @file main.cpp
 * @brief Exercises installed Slice ownership and Kitchen submission with assertions disabled.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>

#ifndef NDEBUG
#error The installed consumer must exercise checks with NDEBUG defined
#endif
/** --------------------------------------------------------------------------------------------------------- Read Payload
 * @brief Returns a retained Slice value from an installed Kitchen Order.
 */
uint64_t read_payload(buffetalligator::Slice payload) {
    return payload.get_as<uint64_t>();
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Verifies the relocated library creates and retains a production Slice.
 */
int main() {
    LOG_INFO_STREAM << "Checking the relocated Alligator package";
    const buffetalligator::BuffetDescriptor* placement =
        buffetalligator::BuffetDescriptors::descriptor_for(
            static_cast<buffetalligator::AlignedHeapBuffer*>(nullptr));
    buffetalligator::Slice payload(size_t{128}, true, placement);
    if (!payload.valid() || payload.size_bytes() != 128) {
        LOG_ERROR_STREAM << "The installed Slice has an incorrect byte length";
        return 1;
    }
    payload.get_as<uint64_t>() = 0xA115A70Full;
    buffetalligator::Slice retained = payload.slice();
    payload.free();
    if (retained.size_bytes() != 128 || retained.get_as<uint64_t>() != 0xA115A70Full) {
        LOG_ERROR_STREAM << "The installed Slice lost its retained allocation";
        return 1;
    }
    std::unique_ptr<std::future<uint64_t>> result;
    buffetalligator::Kitchen::submit(&read_payload, result, retained);
    if (!result || result->wait_for(std::chrono::seconds(5)) != std::future_status::ready
        || result->get() != 0xA115A70Full) {
        LOG_ERROR_STREAM << "The installed Kitchen lost its owned Slice argument or future result";
        return 1;
    }
    LOG_INFO_STREAM << "The relocated Alligator package passed";
    return 0;
}
