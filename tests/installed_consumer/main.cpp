/** --------------------------------------------------------------------------------------------------------- Installed Consumer
 * @file main.cpp
 * @brief Exercises installed Slice ownership and Kitchen submission with assertions disabled.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <alligator/dispatch.hpp>
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
/** --------------------------------------------------------------------------------------------------------- Verify Identifier
 * @brief Checks installed parallel dispatch and iteration link through the exported target.
 */
void verify_identifier(int64_t identifier, buffetalligator::Slice* slice, void* context) {
    *static_cast<bool*>(context) = identifier == 17 && slice->get_as<uint64_t>() == 0xA115A70Full;
}
/** --------------------------------------------------------------------------------------------------------- Warmup
 * @brief Leaves the installed task team's borrowed value ready for its task phase.
 */
void warmup(void*, void*) {}
/** --------------------------------------------------------------------------------------------------------- Finish Task
 * @brief Records completion of the installed task team's single worker.
 */
void finish_task(void* item, void*) { *static_cast<bool*>(item) = true; }
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
    buffetalligator::SliceMap rows(1);
    rows.add_slice(17, retained);
    if (rows.ids() != std::vector<int64_t>{17}) return 1;
    bool visited = false;
    rows.for_each(&verify_identifier, &visited);
    if (!visited) return 1;
    buffetalligator::PackagedFunction invocation(&warmup, std::make_tuple(nullptr, nullptr));
    buffetalligator::dispatch_parallel(invocation);
    bool completed = false;
    buffetalligator::TaskForce team(1, &warmup, &finish_task, nullptr);
    if (team.enqueue(&completed).get() != &completed || !completed) return 1;
    LOG_INFO_STREAM << "The relocated Alligator package passed";
    return 0;
}
