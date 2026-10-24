/** --------------------------------------------------------------------------------------------------------- Backend Shader Test
 * @file backend_shader_test.cpp
 * @brief Checks the GLSL Chomp helpers through prepared dispatches: typed, packed, identity, and large buffers.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/easyvulkan.hpp>
#include "functional_support.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Identity Body
 * @brief Exposes Chomp identity, granule bounds, and device address through the GLSL helpers.
 */
constexpr std::string_view identity_body = R"glsl(
void alligator_main(Chomp stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    chomp_store_u32(stream, 1u, chomp_load_u32(stream, 0u) ^ 0x9e3779b9u);
    chomp_store_u32(stream, 2u, stream.id);
    chomp_store_u32(stream, 3u, gl_NumWorkGroups.y);
    chomp_store_u64(stream, 2u, chomp_size(stream));
    chomp_store_u64(stream, 3u, chomp_address(stream));
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Typed Payload
 * @brief Fixed scalar and vector offsets the typed shader reads and writes.
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
constexpr std::string_view typed_body = R"glsl(
void alligator_main(Chomp payload) {
    if (gl_LocalInvocationIndex != 0u) return;
    chomp_store_u32(payload, 0u, chomp_load_u32(payload, 0u) ^ 0xA5A5A5A5u);
    chomp_store_i32(payload, 1u, chomp_load_i32(payload, 1u) - 17);
    chomp_store_f32(payload, 2u, chomp_load_f32(payload, 2u) * 2.0 + 0.5);
    chomp_store_u64(payload, 2u, chomp_load_u64(payload, 2u) ^ 0xFEDCBA9876543210ul);
    chomp_store_i64(payload, 3u, chomp_load_i64(payload, 3u) - 4294967301l);
    chomp_store_u32x4(payload, 2u,
        chomp_load_u32x4(payload, 2u) + uvec4(1u, 10u, 100u, 1000u));
    chomp_store_i32x4(payload, 3u,
        chomp_load_i32x4(payload, 3u) * ivec4(-1, 2, -3, 4));
    chomp_store_f32x4(payload, 4u,
        chomp_load_f32x4(payload, 4u) * vec4(2.0, -4.0, 0.5, -0.25));
    uint selected_byte = chomp_load_u8(payload, 82u);
    uint selected_halfword = chomp_load_u16(payload, 42u);
    chomp_store_u32(payload, 22u, selected_byte);
    chomp_store_u32(payload, 23u, selected_halfword);
    chomp_store_u8(payload, 81u, selected_byte ^ 0xFFu);
    chomp_store_u16(payload, 43u, selected_halfword ^ 0xFFFFu);
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Typed IO
 * @brief Checks host-to-shader-to-host typed access across repeated prepared dispatches.
 */
void typed_io(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking typed scalar, vector, and packed-neighbor parity";
    Chomp storage(256, false, placement);
    Chomp payload = storage.chomp(64, sizeof(TypedPayload));
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
    Shader shader(typed_body, "backend_typed_io", 1);
    shader(payload).get();
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
    shader(payload).get();
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
/** --------------------------------------------------------------------------------------------------------- Packed Shader
 * @brief Separates encoded inputs, decoded outputs, and simultaneous neighboring scalar writes.
 */
constexpr std::string_view packed_body = R"glsl(
void alligator_main(Chomp payload) {
    uint lane = gl_LocalInvocationIndex;
    if (lane >= 4u) return;
    chomp_store_f32(payload, 16u + lane, chomp_load_f16(payload, 8u + lane));
    chomp_store_f32(payload, 20u + lane, chomp_load_bf16(payload, 12u + lane));
    chomp_store_f32(payload, 24u + lane, chomp_load_e5m2(payload, 32u + lane));
    chomp_store_f32(payload, 28u + lane, chomp_load_e4m3(payload, 36u + lane));
    chomp_store_f32(payload, 32u + lane, chomp_load_e3m4(payload, 40u + lane));
    if (lane < 3u) {
        float value = chomp_load_f32(payload, lane);
        chomp_store_f16(payload, 160u + lane, value);
        chomp_store_bf16(payload, 164u + lane, value);
        chomp_store_e5m2(payload, 336u + lane, value);
        chomp_store_e4m3(payload, 340u + lane, value);
        chomp_store_e3m4(payload, 344u + lane, value);
        chomp_store_i8(payload, 432u + lane, chomp_load_i8(payload, 384u + lane));
    }
    chomp_store_i32(payload, 100u + lane, chomp_load_i8(payload, 384u + lane));
    if (lane < 2u) {
        int value = chomp_load_i16(payload, 194u + lane);
        chomp_store_i32(payload, 104u + lane, value);
        chomp_store_i16(payload, 218u + lane, value);
    }
    if (lane != 0u) return;
    chomp_store_f32x4(payload, 9u, chomp_load_f16x4(payload, 2u));
    chomp_store_f32x4(payload, 10u, chomp_load_bf16x4(payload, 3u));
    chomp_store_f32x4(payload, 11u, chomp_load_e5m2x4(payload, 8u));
    chomp_store_f32x4(payload, 12u, chomp_load_e4m3x4(payload, 9u));
    chomp_store_f32x4(payload, 13u, chomp_load_e3m4x4(payload, 10u));
#ifdef VULKAN_FLOAT16
    chomp_store_f32x4(payload, 14u, vec4(chomp_load_f32x4_half(payload, 0u)));
    chomp_store_f32x4(payload, 15u, vec4(chomp_load_f16x4_half(payload, 2u)));
    chomp_store_f32x4(payload, 16u, vec4(chomp_load_bf16x4_half(payload, 3u)));
    chomp_store_f32x4(payload, 17u, vec4(chomp_load_e5m2x4_half(payload, 8u)));
    chomp_store_f32x4(payload, 18u, vec4(chomp_load_e4m3x4_half(payload, 9u)));
    chomp_store_f32x4(payload, 19u, vec4(chomp_load_e3m4x4_half(payload, 10u)));
#endif
    vec4 values = chomp_load_f32x4(payload, 0u);
    chomp_store_f16x4(payload, 44u, values);
    chomp_store_bf16x4(payload, 45u, values);
    chomp_store_e5m2x4(payload, 92u, values);
    chomp_store_e4m3x4(payload, 93u, values);
    chomp_store_e3m4x4(payload, 94u, values);
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Packed Case
 * @brief Exact encoded representations, independent of the production conversion helpers.
 */
struct PackedCase {
    std::array<float, 4> values;
    uint64_t f16;
    uint64_t bf16;
    uint32_t e5m2;
    uint32_t e4m3;
    uint32_t e3m4;
};
/** --------------------------------------------------------------------------------------------------------- Packed IO
 * @brief Checks scalar, packed, and half-vector conversion parity across two prepared dispatches.
 */
void packed_io(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking f16, bf16, fp8, signed subword, and neighboring-write parity";
    constexpr std::array<PackedCase, 2> cases{{
        {{1.0f, -2.0f, 0.5f, -0.25f}, 0xB4003800C0003C00ull, 0xBE803F00C0003F80ull,
            0xB438C03Cu, 0xA830C038u, 0x9020C030u},
        {{2.0f, -1.0f, 0.25f, -0.5f}, 0xB8003400BC004000ull, 0xBF003E80BF804000ull,
            0xB834BC40u, 0xB028B840u, 0xA010B040u}
    }};
    Shader shader(packed_body, "backend_packed_io", 1);
    Chomp storage(640, placement);
    Chomp payload = storage.chomp(64, 512);
    uint32_t* words = payload.data<uint32_t>();
    uint64_t* wide = payload.data<uint64_t>();
    auto* vectors = payload.data<std::array<float, 4>>();
    storage.data<uint32_t>()[15] = 0x13579BDFu;
    storage.data<uint32_t>()[144] = 0x2468ACE0u;
    const bool half = VulkanContext::device_properties().supports_float16;
    for (size_t round = 0; round < cases.size(); ++round) {
        const PackedCase& expected = cases[round];
        std::fill_n(words, 128, 0xA5A5A5A5u);
        vectors[0] = expected.values;
        wide[2] = expected.f16;
        wide[3] = expected.bf16;
        words[8] = expected.e5m2;
        words[9] = expected.e4m3;
        words[10] = expected.e3m4;
        words[96] = 0xD4C3B2A1u;
        words[97] = 0x98761234u;
        shader(payload).get();
        for (size_t index = 4; index < 14; ++index)
            require(vectors[index] == expected.values,
                "Scalar or packed floating-point decoding differs from the exact encoded input");
        if (half) {
            for (size_t index = 14; index < 20; ++index)
                require(vectors[index] == expected.values,
                    "Half-vector widening differs from the represented source values");
        }
        require(wide[40] == (0xA5A5000000000000ull | (expected.f16 & 0x0000FFFFFFFFFFFFull))
            && wide[41] == (0xA5A5000000000000ull | (expected.bf16 & 0x0000FFFFFFFFFFFFull)),
            "Concurrent scalar f16 or bf16 writes clobbered a neighbor or changed encoded bits");
        require(words[84] == (0xA5000000u | (expected.e5m2 & 0xFFFFFFu))
            && words[85] == (0xA5000000u | (expected.e4m3 & 0xFFFFFFu))
            && words[86] == (0xA5000000u | (expected.e3m4 & 0xFFFFFFu)),
            "Concurrent scalar fp8 writes clobbered a neighbor or changed encoded bits");
        require(wide[44] == expected.f16 && wide[45] == expected.bf16
            && words[92] == expected.e5m2 && words[93] == expected.e4m3
            && words[94] == expected.e3m4,
            "Packed floating-point stores differ from their exact scalar encoding");
        require(payload.data<std::array<int32_t, 4>>()[25]
                == std::array<int32_t, 4>{-95, -78, -61, -44}
            && payload.data<int32_t>()[104] == 4660 && payload.data<int32_t>()[105] == -26506,
            "Signed subword loads failed to preserve sign extension");
        require(words[108] == 0xA5C3B2A1u && words[109] == 0x98761234u,
            "Signed subword stores lost bits or overwrote their neighboring field");
        require(vectors[0] == expected.values && wide[2] == expected.f16 && wide[3] == expected.bf16
            && words[8] == expected.e5m2 && words[9] == expected.e4m3 && words[10] == expected.e3m4
            && words[96] == 0xD4C3B2A1u && words[97] == 0x98761234u,
            "Typed helpers modified an input-only field");
        for (size_t index : {11u, 12u, 13u, 14u, 15u, 87u, 95u, 98u, 99u, 106u, 107u, 110u, 127u})
            require(words[index] == 0xA5A5A5A5u, "Typed helpers wrote outside their output fields");
        require(storage.data<uint32_t>()[15] == 0x13579BDFu
            && storage.data<uint32_t>()[144] == 0x2468ACE0u,
            "Typed helper execution overwrote a Chomp view boundary");
    }
}
/** --------------------------------------------------------------------------------------------------------- Check Identity
 * @brief Verifies the identity shader's words against the dispatched Chomp.
 * @return The device address the shader resolved.
 */
uint64_t check_identity(const Chomp& stream, uint32_t rows) {
    const uint32_t* words = stream.data<uint32_t>();
    const uint64_t* values = stream.data<uint64_t>();
    require(words[1] == (words[0] ^ uint32_t(0x9e3779b9)) && words[2] == stream.id(),
        "The shader changed its marker or Chomp identity");
    require(words[3] == rows, "The shader saw the wrong live row count");
    require(values[2] == stream.size_bytes(), "The shader did not widen length granules to bytes");
    require(values[3] != 0, "The shader resolved a null device address");
    return values[3];
}
/** --------------------------------------------------------------------------------------------------------- Optimized Noop
 * @brief Accepts a valid empty program whose unused push block may disappear during optimization.
 */
void optimized_noop(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking optimized no-op preparation and completion";
    Shader shader("void alligator_main(Chomp stream) {}", "backend_noop", 1);
    Chomp payload(64, placement);
    payload.get_as<uint32_t>() = 73;
    shader(payload).get();
    require(payload.get_as<uint32_t>() == 73, "An optimized no-op shader changed its payload");
}
/** --------------------------------------------------------------------------------------------------------- Identity And Placement
 * @brief Checks rounded subviews and dedicated buffers bound together as rows of one dispatch.
 */
void identity_and_placement(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking identity, granules, and dedicated addresses across one dispatch";
    Shader shader(identity_body, "backend_identity", 1);
    Chomp backing(512, placement);
    std::array<Chomp, 4> streams{backing.chomp(65, 1), backing.chomp(63, 2),
        Chomp(65, true, placement), Chomp(placement->default_size_method() + 64, false, placement)};
    require(streams[0].size_bytes() == 64 && streams[1].size_bytes() == 128,
        "Outward subviews returned incorrect represented bounds");
    Chomp list(streams.size() * sizeof(uint32_t), placement);
    for (size_t index = 0; index < streams.size(); ++index) {
        streams[index].get_as<uint32_t>() = uint32_t(100 + index);
        list.data<uint32_t>()[index] = streams[index].id();
    }
    shader(list, streams.size()).get();
    const uint64_t first = check_identity(streams[0], 4);
    const uint64_t second = check_identity(streams[1], 4);
    require(first == second + 64, "Two views of one slab did not resolve to granule-aligned addresses");
    check_identity(streams[2], 4);
    check_identity(streams[3], 4);
}
/** --------------------------------------------------------------------------------------------------------- Embedded Ids
 * @brief A bound Chomp holding two other ids; the shader resolves both through the directory.
 */
void embedded_ids(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking embedded id resolution";
    constexpr std::string_view body = R"glsl(
void alligator_main(Chomp invocation) {
    if (gl_LocalInvocationIndex != 0u) return;
    Chomp source = gpu_chomp(chomp_load_u32(invocation, 0u));
    Chomp destination = gpu_chomp(chomp_load_u32(invocation, 1u));
    chomp_store_u32(destination, 0u, chomp_load_u32(source, 0u) + 1u);
}
)glsl";
    Chomp source(64, true, placement);
    Chomp destination(64, true, placement);
    source.get_as<uint32_t>() = 53;
    Chomp invocation(64, placement);
    invocation.data<uint32_t>()[0] = source.id();
    invocation.data<uint32_t>()[1] = destination.id();
    Shader shader(body, "backend_embedded", 1);
    shader(invocation).get();
    require(destination.get_as<uint32_t>() == 54, "An embedded id did not resolve to its Chomp");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Context
 * @brief One thread's payload against the shared prepared Shader.
 */
struct ConcurrentContext {
    Shader* shader;
    Chomp stream;
    static void submit(ConcurrentContext* context) { (*context->shader)(context->stream).get(); }
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Dispatch
 * @brief Requires shared prepared dispatch to preserve each caller's independent payload and identity.
 */
void concurrent_dispatch(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking shared Shader submission";
    Shader shader(identity_body, "backend_concurrent", 1);
    const size_t count = std::max(size_t(2), size_t(std::thread::hardware_concurrency())) + 2;
    std::vector<std::unique_ptr<ConcurrentContext>> contexts;
    std::vector<std::thread> threads;
    contexts.reserve(count);
    threads.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        auto context = std::make_unique<ConcurrentContext>();
        context->shader = &shader;
        context->stream = Chomp(64, placement);
        context->stream.get_as<uint32_t>() = uint32_t(index + 900);
        contexts.push_back(std::move(context));
    }
    for (auto& context : contexts) threads.emplace_back(&ConcurrentContext::submit, context.get());
    for (std::thread& thread : threads) thread.join();
    for (const auto& context : contexts) check_identity(context->stream, 1);
}
/** --------------------------------------------------------------------------------------------------------- Whole Buffer Size
 * @brief Resolves exact granule lengths of dedicated buffers and retained views beyond 64 MiB.
 */
void whole_buffer_size(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking exact buffer and view lengths beyond 64 MiB";
    constexpr size_t view_bytes = size_t{64} * 1024 * 1024 + 64;
    Chomp large(view_bytes + 128, true, placement);
    require(large.size_bytes() == view_bytes + 128, "A whole-buffer Chomp lost its exact length");
    Chomp view = large.chomp(64, view_bytes);
    require(view.size_bytes() == view_bytes && view.data() == large.data() + 64,
        "A large view rounded its granule length or changed its buffer offset");
    Shader shader(identity_body, "backend_whole_buffer", 1);
    shader(large).get();
    const uint64_t base = check_identity(large, 1);
    shader(view).get();
    require(check_identity(view, 1) == base + 64, "A large view resolved to the wrong offset");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the shader contracts against the frozen Vulkan device.
 */
int main() {
    try {
        const char* selected = std::getenv("ALLIGATOR_GPU_BACKEND");
        const std::string_view requested(selected == nullptr ? "auto" : selected);
        if (requested == "cpu") {
            require(!GPU::exists(), "CPU selection unexpectedly initialized a compute backend");
            LOG_INFO_STREAM << "Skipping backend shader contracts: CPU-only validation requested";
            return 77;
        }
        if (!VulkanContext::device_present()) {
            LOG_INFO_STREAM << "Skipping backend shader contracts: no Vulkan device";
            return 77;
        }
        Chomp cpu_first(64, false, BuffetDescriptors::get(*AlignedHeapBuffer::type_idx()));
        cpu_first.get_as<uint32_t>() = 41;
        require(GPU::exists() && GPU::backend() == GPUBackend::Vulkan,
            "The first CPU allocation changed the frozen GPU backend");
        const BuffetDescriptor* placement = VulkanContext::buffer_placement();
        LOG_INFO_STREAM << "Backend shader contracts: device=" << GPU::device_name();
        optimized_noop(placement);
        typed_io(placement);
        packed_io(placement);
        identity_and_placement(placement);
        embedded_ids(placement);
        concurrent_dispatch(placement);
        whole_buffer_size(placement);
        require(cpu_first.get_as<uint32_t>() == 41, "GPU execution corrupted an independent CPU allocation");
        LOG_INFO_STREAM << "Backend shader contracts passed";
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Backend shader contracts failed: " << error.what();
        return 1;
    }
}
