#pragma once
/** --------------------------------------------------------------------------------------------------------- Dispatch Record
 * @file dispatch_record.hpp
 * @brief Describes borrowed work for experimental executors independently of production Orders.
 */
#include <cstddef>

namespace experiments {
/** --------------------------------------------------------------------------------------------------------- Dispatch Record
 * @brief Holds the callbacks compared by experimental executor strategies.
 */
struct DispatchRecord {
    void (*action)(void*) = nullptr;
    void* context = nullptr;
    void (*callback)(void*) = nullptr;
    void* callback_context = nullptr;
    /** ------------------------------------------------------------------------------------------- Execute
     * @brief Executes the experiment's work and optional completion callback.
     */
    void execute() {
        action(context);
        if (callback) callback(callback_context);
    }
    /** ------------------------------------------------------------------------------------------- Present
     * @brief Identifies a work record rather than an executor shutdown sentinel.
     */
    explicit operator bool() const { return action != nullptr; }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Adapts a borrowed experiment record to a concrete production Order handler.
     */
    static void run(DispatchRecord record) { record.execute(); }
};
}
