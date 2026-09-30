/** --------------------------------------------------------------------------------------------------------- Vulkan Interoperability Functional Test
 * @file vulkan_interop_functional_test.cpp
 * @brief Exercises mapped typed data and nested Slice identities through prepared Vulkan shaders.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <alligator/kitchen.hpp>
#include <vulkan/shader_state.hpp>
#include "functional_support.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Typed Payload
 * @brief Places scalar, vector, and packed fields at the alignment required by shader helpers.
 */
struct alignas(16) TypedPayload {
    uint32_t unsigned_word;
    int32_t signed_word;
    float float_word;
    uint32_t guard;
    uint64_t unsigned_wide;
    int64_t signed_wide;
    std::array<uint32_t, 4> unsigned_vector;
    std::array<int32_t, 4> signed_vector;
    std::array<float, 4> float_vector;
    uint32_t packed_bytes;
    uint32_t packed_halfwords;
    uint32_t byte_readback;
    uint32_t halfword_readback;
};
static_assert(sizeof(TypedPayload) == 96);
static_assert(offsetof(TypedPayload, unsigned_wide) == 16);
static_assert(offsetof(TypedPayload, signed_wide) == 24);
static_assert(offsetof(TypedPayload, unsigned_vector) == 32);
static_assert(offsetof(TypedPayload, signed_vector) == 48);
static_assert(offsetof(TypedPayload, float_vector) == 64);
static_assert(offsetof(TypedPayload, packed_bytes) == 80);
static_assert(offsetof(TypedPayload, packed_halfwords) == 84);
/** --------------------------------------------------------------------------------------------------------- Typed Shader
 * @brief Transforms mapped scalar and vector values while preserving adjacent packed fields.
 */
static constexpr std::string_view TYPED_SHADER = R"glsl(
void alligator_main(Slice payload) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32(payload, 0u, slice_load_u32(payload, 0u) ^ 0xA5A5A5A5u);
    slice_store_i32(payload, 1u, slice_load_i32(payload, 1u) - 17);
    slice_store_f32(payload, 2u, slice_load_f32(payload, 2u) * 2.0 + 0.5);
    slice_store_u64(payload, 2u, slice_load_u64(payload, 2u) ^ 0xFEDCBA9876543210ul);
    slice_store_i64(payload, 3u, slice_load_i64(payload, 3u) - 4294967301l);
    slice_store_u32x4(payload, 2u,
        slice_load_u32x4(payload, 2u) + uvec4(1u, 10u, 100u, 1000u));
    slice_store_i32x4(payload, 3u,
        slice_load_i32x4(payload, 3u) * ivec4(-1, 2, -3, 4));
    slice_store_f32x4(payload, 4u,
        slice_load_f32x4(payload, 4u) * vec4(2.0, -4.0, 0.5, -0.25));
    uint selected_byte = slice_load_u8(payload, 82u);
    uint selected_halfword = slice_load_u16(payload, 42u);
    slice_store_u32(payload, 22u, selected_byte);
    slice_store_u32(payload, 23u, selected_halfword);
    slice_store_u8(payload, 81u, selected_byte ^ 0xFFu);
    slice_store_u16(payload, 43u, selected_halfword ^ 0xFFFFu);
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Vulkan Typed IO
 * @brief Checks host-to-shader-to-host typed access across repeated prepared dispatches.
 */
static void vulkan_typed_io() {
    Slice storage(256, false, VulkanContext::buffer_placement());
    Slice payload = storage.slice(64, sizeof(TypedPayload));
    uint32_t* storage_words = storage.data<uint32_t>();
    storage_words[15] = 0x13579BDFu;
    storage_words[48] = 0x2468ACE0u;
    TypedPayload& values = payload.get_as<TypedPayload>();
    values = {
        0x12345678u, -100, 1.25f, 0xCAFEBABEu,
        0x0123456789ABCDEFull, -0x0000000200000003ll,
        {2u, 3u, 5u, 7u}, {-2, 3, -5, 7}, {1.5f, -2.0f, 8.0f, -16.0f},
        0xD4C3B2A1u, 0x98761234u, 0u, 0u
    };
    require(VulkanKernel::device_address(payload) % alignof(TypedPayload) == 0,
        "typed shader payload is not aligned for vector4 helpers");
    Shader shader(TYPED_SHADER, "vulkan_typed_io");
    TaskCountdown complete;
    ShaderResult result;
    shader(payload, result, &TaskCountdown::arrive, &complete);
    complete.wait();
    result.rethrow();
    require(values.unsigned_word == 0xB791F3DDu && values.signed_word == -117,
        "32-bit signed or unsigned shader access changed the wrong value");
    require(values.float_word == 3.0f, "32-bit floating-point shader access differs");
    require(values.unsigned_wide == 0xFFFFFFFFFFFFFFFFull
        && values.signed_wide == -0x0000000300000008ll,
        "64-bit shader access lost signedness or upper bits");
    require(values.unsigned_vector == std::array<uint32_t, 4>{3u, 13u, 105u, 1007u},
        "unsigned vector4 shader access used the wrong element stride");
    require(values.signed_vector == std::array<int32_t, 4>{2, 6, 15, 28},
        "signed vector4 shader access differs");
    require(values.float_vector == std::array<float, 4>{3.0f, 8.0f, 4.0f, 4.0f},
        "floating-point vector4 shader access differs");
    require(values.byte_readback == 0xC3u && values.packed_bytes == 0xD4C33CA1u,
        "packed byte access selected the wrong byte or overwrote its neighbors");
    require(values.halfword_readback == 0x1234u && values.packed_halfwords == 0xEDCB1234u,
        "packed halfword access selected the wrong half or overwrote its neighbor");
    require(values.guard == 0xCAFEBABEu && storage_words[15] == 0x13579BDFu
        && storage_words[48] == 0x2468ACE0u,
        "typed shader access overwrote alignment padding or view boundaries");
    values.unsigned_word = 0x01020304u;
    values.signed_word = 2147483647;
    values.float_word = -4.25f;
    values.unsigned_wide = 0xFEDCBA9876543210ull;
    values.signed_wide = 0x0000000400000007ll;
    values.unsigned_vector = {11u, 13u, 17u, 19u};
    values.signed_vector = {11, -13, 17, -19};
    values.float_vector = {-0.5f, 0.25f, -8.0f, 32.0f};
    values.packed_bytes = 0x1728394Au;
    values.packed_halfwords = 0x2468ABCDu;
    complete.rearm(1);
    shader(payload, result, &TaskCountdown::arrive, &complete);
    complete.wait();
    result.rethrow();
    require(values.unsigned_word == 0xA4A7A6A1u && values.signed_word == 2147483630,
        "reused typed shader did not read updated mapped 32-bit inputs");
    require(values.float_word == -8.0f && values.unsigned_wide == 0
        && values.signed_wide == 0x0000000300000002ll,
        "reused typed shader did not read updated mapped scalar inputs");
    require(values.unsigned_vector == std::array<uint32_t, 4>{12u, 23u, 117u, 1019u}
        && values.signed_vector == std::array<int32_t, 4>{-11, -26, -51, -76}
        && values.float_vector == std::array<float, 4>{-1.0f, -1.0f, -4.0f, -8.0f},
        "reused typed shader did not read updated mapped vector inputs");
    require(values.byte_readback == 0x28u && values.packed_bytes == 0x1728D74Au
        && values.halfword_readback == 0xABCDu && values.packed_halfwords == 0x5432ABCDu,
        "reused packed stores lost updated inputs or neighboring bytes");
    require(values.guard == 0xCAFEBABEu && storage_words[15] == 0x13579BDFu
        && storage_words[48] == 0x2468ACE0u,
        "reused typed shader wrote outside its fields");
}
/** --------------------------------------------------------------------------------------------------------- Boundary Payload
 * @brief Separates two shader views with guard vectors and an untouched sibling view.
 */
struct alignas(64) BoundaryPayload {
    alignas(64) std::array<uint32_t, 4> prefix;
    alignas(64) std::array<uint32_t, 4> source_before;
    alignas(64) std::array<uint32_t, 4> source;
    alignas(64) std::array<uint32_t, 4> source_after;
    alignas(64) std::array<uint32_t, 4> middle;
    alignas(64) std::array<uint32_t, 4> target_before;
    alignas(64) std::array<uint32_t, 4> target;
    alignas(64) std::array<uint32_t, 4> target_after;
    alignas(64) std::array<uint32_t, 4> sibling;
    alignas(64) std::array<uint32_t, 4> suffix;
};
static_assert(sizeof(BoundaryPayload) == 640);
static_assert(offsetof(BoundaryPayload, source) == 128);
static_assert(offsetof(BoundaryPayload, target) == 384);
static_assert(offsetof(BoundaryPayload, sibling) == 512);
/** --------------------------------------------------------------------------------------------------------- Boundary Seed
 * @brief Gives each untouched region a distinct deterministic pattern.
 */
static constexpr BoundaryPayload BOUNDARY_SEED{
    {0x01010101u, 0x02020202u, 0x03030303u, 0x04040404u},
    {0x11111111u, 0x12121212u, 0x13131313u, 0x14141414u},
    {11u, 13u, 17u, 19u},
    {0x21212121u, 0x22222222u, 0x23232323u, 0x24242424u},
    {0x31313131u, 0x32323232u, 0x33333333u, 0x34343434u},
    {0x41414141u, 0x42424242u, 0x43434343u, 0x44444444u},
    {1001u, 1002u, 1003u, 1004u},
    {0x51515151u, 0x52525252u, 0x53535353u, 0x54545454u},
    {0x61616161u, 0x62626262u, 0x63636363u, 0x64646464u},
    {0x71717171u, 0x72727272u, 0x73737373u, 0x74747474u}
};
/** --------------------------------------------------------------------------------------------------------- Boundary Shader
 * @brief Resolves adjacent host Slice fields and updates both nested mapped views.
 */
static constexpr std::string_view BOUNDARY_SHADER = R"glsl(
void alligator_main(Slice invocation) {
    if (gl_LocalInvocationIndex != 0u) return;
    Slice source = slice_read(slice_address(invocation));
    Slice target = slice_read(slice_address(invocation) + 4ul);
    uvec4 values = slice_load_u32x4(source, 0u);
    slice_store_u32x4(target, 0u, values * uvec4(2u, 3u, 5u, 7u) + uvec4(1u, 2u, 3u, 4u));
    slice_store_u32x4(source, 0u, values + uvec4(10u, 20u, 30u, 40u));
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Verify Boundary Guards
 * @brief Checks every region outside the two shader output views against its initial pattern.
 */
static void verify_boundary_guards(const BoundaryPayload& values) {
    require(values.prefix == BOUNDARY_SEED.prefix && values.suffix == BOUNDARY_SEED.suffix,
        "nested shader views wrote outside their containing storage");
    require(values.source_before == BOUNDARY_SEED.source_before
        && values.source_after == BOUNDARY_SEED.source_after,
        "nested source view overwrote its adjacent guard words");
    require(values.target_before == BOUNDARY_SEED.target_before
        && values.target_after == BOUNDARY_SEED.target_after,
        "Slice target view overwrote its adjacent guard words");
    require(values.middle == BOUNDARY_SEED.middle && values.sibling == BOUNDARY_SEED.sibling,
        "nested shader views modified an untouched sibling or separating guard");
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Subslice Boundaries
 * @brief Checks nested Slice and Slice views at nonzero slab offsets during GPU writes.
 */
static void vulkan_subslice_boundaries() {
    Slice preceding(64, false, VulkanContext::buffer_placement());
    Slice storage(sizeof(BoundaryPayload), false, VulkanContext::buffer_placement());
    Slice source_parent = storage.slice(64, 192);
    Slice source = source_parent.slice(65, 16);
    Slice target_parent = storage.slice(320, 128);
    Slice identified_parent = target_parent.slice(64, 64);
    Slice target = identified_parent.slice(3, 16);
    Slice sibling = storage.slice(513, 15);
    Slice invocation_storage(128, false, VulkanContext::buffer_placement());
    Slice invocation = invocation_storage.slice(66, 2 * sizeof(uint32_t));
    BoundaryPayload& values = storage.get_as<BoundaryPayload>();
    values = BOUNDARY_SEED;
    preceding.data<uint32_t>()[0] = 0x89ABCDEFu;
    preceding.data<uint32_t>()[15] = 0x76543210u;
    invocation_storage.data<uint32_t>()[0] = 0xABCDABCDu;
    invocation_storage.data<uint32_t>()[3] = 0xDCBADCBAu;
    invocation.data<uint32_t>()[0] = source.id();
    invocation.data<uint32_t>()[1] = target.id();
    const GPUBuf& storage_record = *Alligator::gpubuf_for(storage);
    const GPUBuf& source_record = *Alligator::gpubuf_for(source);
    const GPUBuf& target_record = *Alligator::gpubuf_for(target);
    require(Alligator::gpubuf_for(preceding)->address == storage_record.address
        && storage_record.offset != 0,
        "boundary test storage did not occupy a nonzero Vulkan slab offset");
    require(source_record.address == storage_record.address
        && target_record.address == storage_record.address,
        "nested GPU views shifted their shared slab base address");
    require(source_record.offset == storage_record.offset + 2
        && target_record.offset == storage_record.offset + 6,
        "nested Slice and Slice views did not accumulate slab-relative offsets");
    require(source.data() == storage.data() + 128 && target.data() == storage.data() + 384,
        "nested host views did not resolve the intended vector fields");
    require(VulkanKernel::device_address(source) == VulkanKernel::device_address(storage) + 128
        && VulkanKernel::device_address(target) == VulkanKernel::device_address(storage) + 384,
        "nested GPU view addresses applied an offset more than once");
    ShaderState shader(BOUNDARY_SHADER, "vulkan_subslice_boundaries");
    shader.dispatch(&invocation, 1, 1);
    require(values.source == std::array<uint32_t, 4>{21u, 33u, 47u, 59u}
        && values.target == std::array<uint32_t, 4>{23u, 41u, 88u, 137u},
        "shader Slice IDs did not read and write the intended nested views");
    verify_boundary_guards(values);
    require(sibling.get_as<std::array<uint32_t, 4>>() == BOUNDARY_SEED.sibling,
        "sibling Slice view observed a write outside the target view");
    source.get_as<std::array<uint32_t, 4>>() = {101u, 103u, 107u, 109u};
    target.get_as<std::array<uint32_t, 4>>() = {9001u, 9002u, 9003u, 9004u};
    shader.dispatch(&invocation, 1, 1);
    require(values.source == std::array<uint32_t, 4>{111u, 123u, 137u, 149u}
        && values.target == std::array<uint32_t, 4>{203u, 311u, 538u, 767u},
        "reused prepared dispatch did not observe updated nested host data");
    verify_boundary_guards(values);
    require(invocation.data<uint32_t>()[0] == source.id()
        && invocation.data<uint32_t>()[1] == target.id()
        && invocation_storage.data<uint32_t>()[0] == 0xABCDABCDu
        && invocation_storage.data<uint32_t>()[3] == 0xDCBADCBAu,
        "shader writes corrupted compact Slice fields or their guards");
    require(preceding.data<uint32_t>()[0] == 0x89ABCDEFu
        && preceding.data<uint32_t>()[15] == 0x76543210u,
        "nested GPU writes escaped into the preceding slab allocation");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs isolated Vulkan interoperability cases when a compute device is available.
 */
int main(int count, char** arguments) {
    if (!VulkanKernel::available()) {
        LOG_INFO_STREAM << "Skipping Vulkan interoperability tests: no Vulkan compute device";
        return 77;
    }
    LOG_INFO_STREAM << "Vulkan interoperability device: " << VulkanKernel::device_name();
    return functional::run(count, arguments, {
        {"vulkan_typed_io", vulkan_typed_io},
        {"vulkan_subslice_boundaries", vulkan_subslice_boundaries}
    });
}
