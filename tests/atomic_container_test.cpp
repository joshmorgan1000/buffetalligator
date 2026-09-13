/** --------------------------------------------------------------------------------------------------------- Atomic Container Test
 * @file atomic_container_test.cpp
 * @brief Checks AtomicContainer storage lifecycle and value round-tripping for native, pointer, and
 * emulated (non-native) types, and the same through AtomicRegistry. Run under AddressSanitizer this
 * pins the storage ledger: a pointer container must free the std::atomic<T*> wrapper it allocated
 * without touching the pointee, and every container must free its storage on destruction.
 */
#include <alligator.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using buffetalligator::AtomicContainer;
using buffetalligator::AtomicRegistry;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Native
 * @brief A native-atomic container stores, loads, and frees its wrapper.
 */
static void native() {
    AtomicContainer c(uint64_t{5});
    require(c.load<uint64_t>() == 5, "native load returned the wrong value");
    require(c.fetch_add<uint64_t>(3) == 5, "native fetch_add returned the wrong prior value");
    require(c.load<uint64_t>() == 8, "native fetch_add did not apply");
}
/** --------------------------------------------------------------------------------------------------------- Pointer
 * @brief A pointer container stores and loads the pointer and frees only its wrapper, never the
 * pointee. The pointee lives on the stack here, so a wrongful delete would be a sanitizer error.
 */
static void pointer() {
    int target = 11;
    {
        AtomicContainer c(&target);
        require(c.load<int*>() == &target, "pointer load returned the wrong address");
        require(*c.load<int*>() == 11, "pointer load does not see the pointee");
    }
    require(target == 11, "pointer container disturbed its pointee");
}
/** --------------------------------------------------------------------------------------------------------- Emulated
 * @brief A non-native container heap-allocates its value and frees it on destruction.
 */
static void emulated() {
    AtomicContainer c(std::string("initial-value-long-enough-to-heap-allocate"));
    require(c.load<std::string>().rfind("initial", 0) == 0, "emulated load returned the wrong value");
    c.store(std::string("replacement-value-also-long-enough-to-heap"));
    require(c.load<std::string>().rfind("replacement", 0) == 0, "emulated store did not apply");
}
/** --------------------------------------------------------------------------------------------------------- Registry
 * @brief The registry owns its containers and frees them on remove and on clear.
 */
static void registry() {
    AtomicRegistry reg;
    int target = 3;
    reg.create<uint64_t>("count", uint64_t{1});
    reg.create<int*>("ptr", &target);
    reg.create<std::string>("name", std::string("value-long-enough-to-heap-allocate"));
    require(reg["count"]->load<uint64_t>() == 1, "registry native round-trip failed");
    require(reg["ptr"]->load<int*>() == &target, "registry pointer round-trip failed");
    std::unique_ptr<AtomicContainer> removed = reg.remove("count");
    require(removed && removed->load<uint64_t>() == 1, "remove did not hand back the container");
    reg.clear();
    require(reg.empty(), "clear left entries behind");
    require(target == 3, "registry pointer container disturbed its pointee");
}
/** --------------------------------------------------------------------------------------------------------- Main */
int main() {
    native();
    pointer();
    emulated();
    registry();
    std::printf("atomic container tests passed\n");
    return 0;
}
