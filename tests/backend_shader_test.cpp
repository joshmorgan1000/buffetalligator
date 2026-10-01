/** --------------------------------------------------------------------------------------------------------- Backend Shader Test
 * @file backend_shader_test.cpp
 * @brief Runs identical native and translated Shader contracts in a selected device address domain.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <alligator/kitchen.hpp>
#include <gpu/runtime.hpp>
#include <vulkan/shader_state.hpp>
#if defined(BUFFETALLIGATOR_HAS_METAL)
#include <metal/metal.hpp>
#endif
#include "functional_support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Identity Body
 * @brief Uses common GLSL/MSL helpers to expose Slice identity, granule bounds, and device address.
 */
constexpr std::string_view identity_body = R"shader(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32(stream, 1u, slice_load_u32(stream, 0u) ^ 0x9e3779b9u);
    slice_store_u32(stream, 2u, stream.id);
    slice_store_u32(stream, 3u, gl_NumWorkGroups.y);
    slice_store_u64(stream, 2u, slice_size(stream));
    slice_store_u64(stream, 3u, slice_address(stream));
}
)shader";
/** --------------------------------------------------------------------------------------------------------- Source
 * @brief Selects exactly one source member so successful native tests cannot use GLSL translation.
 */
ShaderSource source(std::string_view body, bool native) {
    return native ? ShaderSource{{}, body, {}} : ShaderSource{body, {}, {}};
}
/** --------------------------------------------------------------------------------------------------------- Typed Payload
 * @brief Matches the existing Vulkan typed-IO contract at fixed scalar and vector offsets.
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
/** --------------------------------------------------------------------------------------------------------- Typed IO
 * @brief Checks host-to-shader-to-host typed access across repeated prepared dispatches.
 */
void typed_io(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking backend typed scalar, vector, and packed-neighbor parity";
    Slice storage(256, false, placement);
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
    require((Alligator::gpubuf_for(payload)->address + (uint64_t(Alligator::gpubuf_for(payload)->offset) << 6)) % alignof(TypedPayload) == 0,
        "typed shader payload is not aligned for vector4 helpers");
    std::string body(native ? "#define uvec4 uint4\n#define ivec4 int4\n#define vec4 float4\n" : "");
    body += typed_body;
    Shader shader(source(body, native), "backend_typed_io");
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
/** --------------------------------------------------------------------------------------------------------- Packed Shader
 * @brief Separates encoded inputs, decoded outputs, and simultaneous neighboring scalar writes.
 */
constexpr std::string_view packed_body = R"shader(
void alligator_main(Slice payload) {
    uint lane = gl_LocalInvocationIndex;
    if (lane >= 4u) return;
    slice_store_f32(payload, 16u + lane, slice_load_f16(payload, 8u + lane));
    slice_store_f32(payload, 20u + lane, slice_load_bf16(payload, 12u + lane));
    slice_store_f32(payload, 24u + lane, slice_load_e5m2(payload, 32u + lane));
    slice_store_f32(payload, 28u + lane, slice_load_e4m3(payload, 36u + lane));
    slice_store_f32(payload, 32u + lane, slice_load_e3m4(payload, 40u + lane));
    if (lane < 3u) {
        float value = slice_load_f32(payload, lane);
        slice_store_f16(payload, 160u + lane, value);
        slice_store_bf16(payload, 164u + lane, value);
        slice_store_e5m2(payload, 336u + lane, value);
        slice_store_e4m3(payload, 340u + lane, value);
        slice_store_e3m4(payload, 344u + lane, value);
        slice_store_i8(payload, 432u + lane, slice_load_i8(payload, 384u + lane));
    }
    slice_store_i32(payload, 100u + lane, slice_load_i8(payload, 384u + lane));
    if (lane < 2u) {
        int value = slice_load_i16(payload, 194u + lane);
        slice_store_i32(payload, 104u + lane, value);
        slice_store_i16(payload, 218u + lane, value);
    }
    if (lane != 0u) return;
    slice_store_f32x4(payload, 9u, slice_load_f16x4(payload, 2u));
    slice_store_f32x4(payload, 10u, slice_load_bf16x4(payload, 3u));
    slice_store_f32x4(payload, 11u, slice_load_e5m2x4(payload, 8u));
    slice_store_f32x4(payload, 12u, slice_load_e4m3x4(payload, 9u));
    slice_store_f32x4(payload, 13u, slice_load_e3m4x4(payload, 10u));
#ifdef VULKAN_FLOAT16
    slice_store_f32x4(payload, 14u, vec4(slice_load_f32x4_half(payload, 0u)));
    slice_store_f32x4(payload, 15u, vec4(slice_load_f16x4_half(payload, 2u)));
    slice_store_f32x4(payload, 16u, vec4(slice_load_bf16x4_half(payload, 3u)));
    slice_store_f32x4(payload, 17u, vec4(slice_load_e5m2x4_half(payload, 8u)));
    slice_store_f32x4(payload, 18u, vec4(slice_load_e4m3x4_half(payload, 9u)));
    slice_store_f32x4(payload, 19u, vec4(slice_load_e3m4x4_half(payload, 10u)));
#endif
    vec4 values = slice_load_f32x4(payload, 0u);
    slice_store_f16x4(payload, 44u, values);
    slice_store_bf16x4(payload, 45u, values);
    slice_store_e5m2x4(payload, 92u, values);
    slice_store_e4m3x4(payload, 93u, values);
    slice_store_e3m4x4(payload, 94u, values);
}
)shader";
/** --------------------------------------------------------------------------------------------------------- Packed Case
 * @brief Supplies exact encoded representations independently of the production conversion helpers.
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
void packed_io(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking backend f16, bf16, fp8, signed subword, and neighboring-write parity";
    constexpr std::array<PackedCase, 2> cases{{
        {{1.0f, -2.0f, 0.5f, -0.25f}, 0xB4003800C0003C00ull, 0xBE803F00C0003F80ull,
            0xB438C03Cu, 0xA830C038u, 0x9020C030u},
        {{2.0f, -1.0f, 0.25f, -0.5f}, 0xB8003400BC004000ull, 0xBF003E80BF804000ull,
            0xB834BC40u, 0xB028B840u, 0xA010B040u}
    }};
    std::string body(native ? "#define vec4 float4\n#define VULKAN_FLOAT16 1\n" : "");
    body += packed_body;
    Shader shader(source(body, native), "backend_packed_io");
    Slice storage(640, placement);
    Slice payload = storage.slice(64, 512);
    uint32_t* words = payload.data<uint32_t>();
    uint64_t* wide = payload.data<uint64_t>();
    auto* vectors = payload.data<std::array<float, 4>>();
    storage.data<uint32_t>()[15] = 0x13579BDFu;
    storage.data<uint32_t>()[144] = 0x2468ACE0u;
    const bool half = native || gpu_device().kind == GPUBackend::Metal
        || VulkanContext::device_properties().supports_float16;
    TaskCountdown complete;
    ShaderResult result;
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
        if (round != 0) complete.rearm(1);
        shader(payload, result, &TaskCountdown::arrive, &complete);
        complete.wait();
        result.rethrow();
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
            "Typed helper execution overwrote a Slice view boundary");
    }
}
/** --------------------------------------------------------------------------------------------------------- Check Identity
 * @brief Verifies GPU results against the live production descriptor record.
 */
void check_identity(const Slice& stream) {
    const uint32_t* words = stream.data<uint32_t>();
    const uint64_t* values = stream.data<uint64_t>();
    const GPUBuf& record = *Alligator::gpubuf_for(stream);
    require(words[1] == (words[0] ^ uint32_t(0x9e3779b9)) && words[2] == stream.id(),
        "The shader changed its marker or Slice identity");
    require(values[2] == stream.size_bytes() && values[2] == (uint64_t(record.size) << 6),
        "The shader did not widen length granules to bytes");
    require(values[3] == record.address + (uint64_t(record.offset) << 6),
        "The shader resolved the wrong backing address or granule offset");
}
/** --------------------------------------------------------------------------------------------------------- Optimized Noop
 * @brief Accepts a valid empty program whose unused push block may disappear during optimization.
 */
void optimized_noop(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking optimized no-op backend preparation and completion";
    Shader shader(source("void alligator_main(Slice stream) {}", native), "backend_noop");
    Slice payload(64, placement);
    payload.get_as<uint32_t>() = 73;
    ShaderResult result;
    TaskCountdown finished;
    shader(payload, result, &TaskCountdown::arrive, &finished);
    finished.wait();
    result.rethrow();
    require(payload.get_as<uint32_t>() == 73, "An optimized no-op shader changed its payload");
}
/** --------------------------------------------------------------------------------------------------------- Identity And Placement
 * @brief Checks rounded subviews, dedicated buffers, ownership, and incompatible-domain rejection.
 */
void identity_and_placement(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking backend identity, granules, dedicated addresses, and placement rejection";
    Shader shader(source(identity_body, native), "backend_identity");
    Slice backing(512, placement);
    std::array<Slice, 4> streams{backing.slice(65, 1), backing.slice(63, 2),
        Slice(65, true, placement), Slice(placement->default_size + 64, false, placement)};
    require(streams[0].size_bytes() == 64 && streams[1].size_bytes() == 128,
        "Outward subviews returned incorrect represented bounds");
    require(streams[2].is_novel() && streams[3].is_novel(),
        "Dedicated claims lost their backing kind");
    for (size_t index = 0; index < streams.size(); ++index)
        streams[index].get_as<uint32_t>() = uint32_t(100 + index);
    ShaderResult result;
    TaskCountdown finished;
    shader(streams.data(), streams.size(), result, &TaskCountdown::arrive, &finished);
    backing.free();
    finished.wait();
    result.rethrow();
    for (const Slice& stream : streams) check_identity(stream);
    Slice incompatible(64, false, BuffetDescriptors::get(AlignedHeapBuffer::type_idx()));
    bool rejected = false;
    try { ShaderResult invalid; shader(incompatible, invalid); }
    catch (const GPUException&) { rejected = true; }
    require(rejected, "The selected GPU accepted an incompatible CPU placement");
}
/** --------------------------------------------------------------------------------------------------------- Embedded Dependencies
 * @brief Retains embedded identities and their backing after caller-owned handles and Shader disappear.
 */
void embedded_dependencies(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking embedded dependencies and accepted-work lifetime";
    constexpr std::string_view body = R"shader(
void alligator_main(Slice invocation) {
    if (gl_LocalInvocationIndex != 0u) return;
    Slice source = gpu_slice(slice_load_u32(invocation, 0u));
    Slice destination = gpu_slice(slice_load_u32(invocation, 1u));
    slice_store_u32(destination, 0u, slice_load_u32(source, 0u) + 1u);
}
)shader";
    std::array<Slice, 2> dependencies{Slice(64, true, placement), Slice(64, true, placement)};
    dependencies[0].get_as<uint32_t>() = 53;
    Slice observed = dependencies[1].slice();
    Slice invocation(64, placement);
    invocation.data<uint32_t>()[0] = dependencies[0].id();
    invocation.data<uint32_t>()[1] = dependencies[1].id();
    TaskCountdown finished;
    ShaderResult result;
    {
        Shader shader(source(body, native), "backend_embedded");
        shader(invocation, result, &TaskCountdown::arrive, &finished, 1, dependencies);
    }
    invocation.free();
    for (Slice& dependency : dependencies) dependency.free();
    finished.wait();
    result.rethrow();
    require(observed.get_as<uint32_t>() == 54, "An embedded dependency was reclaimed before execution");
}
/** --------------------------------------------------------------------------------------------------------- Dispatch Rounds
 * @brief Uses the actual prepared capacity to exercise full and partial rounds in one logical dispatch.
 */
void dispatch_rounds(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking full and partial backend dispatch rounds";
    ShaderState prepared(source(identity_body, native), "backend_rounds");
    const size_t capacity = prepared.capacity();
    require(capacity > 0, "The prepared backend has no stream capacity");
    const size_t count = capacity + 3;
    Slice backing(count * 64, placement);
    std::vector<Slice> streams;
    streams.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        streams.push_back(backing.slice(index * 64, 64));
        streams.back().get_as<uint32_t>() = uint32_t(index);
    }
    prepared.dispatch(streams.data(), streams.size(), 1);
    for (size_t index = 0; index < streams.size(); ++index) {
        check_identity(streams[index]);
        require(streams[index].data<uint32_t>()[3] == (index < capacity ? capacity : 3),
            "A partial round published stale dispatch dimensions");
    }
    prepared.dispatch(streams.data(), 1, 1);
    require(streams[0].data<uint32_t>()[3] == 1, "A shorter dispatch retained an earlier round count");
}
/** --------------------------------------------------------------------------------------------------------- Coroutine Task
 * @brief Keeps an external cancellation owner until its suspended frame is safe to destroy.
 */
struct CoroutineTask {
    struct promise_type {
        CoroutineTask get_return_object() {
            return CoroutineTask(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> frame;
    ShaderResult owner;
    explicit CoroutineTask(std::coroutine_handle<promise_type> handle) : frame(handle) {}
    CoroutineTask(const CoroutineTask&) = delete;
    ~CoroutineTask() { owner.cancel(); frame.destroy(); }
};
/** --------------------------------------------------------------------------------------------------------- Coroutine Context
 * @brief Publishes resumed state and terminal exceptions outside the coroutine frame.
 */
struct CoroutineContext {
    TaskCountdown finished;
    bool resumed = false;
    std::exception_ptr error;
};
/** --------------------------------------------------------------------------------------------------------- Await Completion
 * @brief Records completion after production await_resume propagates any terminal device error.
 */
CoroutineTask await_completion(ShaderAwaiter awaiter, CoroutineContext* context) {
    try { co_await std::move(awaiter); context->resumed = true; }
    catch (...) { context->error = std::current_exception(); }
    TaskCountdown::arrive(&context->finished);
}
/** --------------------------------------------------------------------------------------------------------- Coroutine Ownership
 * @brief Verifies real resumption, zero-count completion, and managed frame cancellation.
 */
void coroutine_ownership(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking backend coroutine resumption and cancellation";
    Shader shader(source(identity_body, native), "backend_coroutine");
    Slice payload(64, placement);
    payload.get_as<uint32_t>() = 87;
    CoroutineContext context;
    auto awaiter = shader.dispatch(&payload, 1);
    ShaderResult owner = awaiter.result();
    CoroutineTask task = await_completion(std::move(awaiter), &context);
    task.owner = owner;
    task.frame.resume();
    context.finished.wait();
    owner.cancel();
    if (context.error) std::rethrow_exception(context.error);
    require(context.resumed, "The backend did not resume its accepted coroutine");
    check_identity(payload);
    for (size_t index = 0; index < 16; ++index) {
        CoroutineContext pending_context;
        auto pending = shader.dispatch(nullptr, 0);
        ShaderResult cancellation = pending.result();
        CoroutineTask pending_task = await_completion(std::move(pending), &pending_context);
        pending_task.owner = cancellation;
        pending_task.frame.resume();
        cancellation.cancel();
    }
    TaskCountdown zero_finished;
    ShaderResult zero;
    shader(nullptr, 0, zero, &TaskCountdown::arrive, &zero_finished);
    zero_finished.wait();
    zero.rethrow();
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Context
 * @brief Holds independent results while submitting threads share the same prepared backend program.
 */
struct ConcurrentContext {
    Shader* shader;
    Slice stream;
    ShaderResult result;
    TaskCountdown finished;
    static void submit(ConcurrentContext* context) {
        (*context->shader)(context->stream, context->result,
            &TaskCountdown::arrive, &context->finished);
        context->finished.wait();
        context->result.rethrow();
    }
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Dispatch
 * @brief Requires shared prepared dispatch to preserve each caller's independent payload and identity.
 */
void concurrent_dispatch(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking shared backend Shader submission";
    Shader shader(source(identity_body, native), "backend_concurrent");
    const size_t count = std::max(size_t(2), size_t(std::thread::hardware_concurrency())) + 2;
    std::vector<std::unique_ptr<ConcurrentContext>> contexts;
    std::vector<std::thread> threads;
    contexts.reserve(count);
    threads.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        auto context = std::make_unique<ConcurrentContext>();
        context->shader = &shader;
        context->stream = Slice(64, placement);
        context->stream.get_as<uint32_t>() = uint32_t(index + 900);
        contexts.push_back(std::move(context));
    }
    for (auto& context : contexts) threads.emplace_back(&ConcurrentContext::submit, context.get());
    for (std::thread& thread : threads) thread.join();
    for (const auto& context : contexts) check_identity(context->stream);
}
/** --------------------------------------------------------------------------------------------------------- Program Roundtrip
 * @brief Requires the serialized source member to survive decode and warm program-cache reuse.
 */
void program_roundtrip(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking backend source encoding, decoding, and cache reuse";
    Shader original(source(identity_body, native), "backend_roundtrip");
    Slice encoded = GPU::encode(original);
    Shader first = GPU::decode(encoded);
    Shader second = GPU::decode(encoded);
    Slice payload(64, placement);
    payload.get_as<uint32_t>() = 1234;
    TaskCountdown finished;
    ShaderResult result;
    first(payload, result, &TaskCountdown::arrive, &finished);
    finished.wait();
    result.rethrow();
    check_identity(payload);
    payload.data<uint32_t>()[1] = 0;
    finished.rearm(1);
    second(payload, result, &TaskCountdown::arrive, &finished);
    encoded.free();
    finished.wait();
    result.rethrow();
    check_identity(payload);
}
/** --------------------------------------------------------------------------------------------------------- Translated Jobs
 * @brief Executes the existing GLSL reduction through encoded job-table compilation and dispatch.
 */
void translated_jobs(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking translated encoded job-table dispatch";
    std::array<Slice, 2> streams{Slice(64, placement), Slice(64, placement)};
    streams[0].get_as<std::array<float, 16>>() = {0, 0, 0, 0, 1, 2, 3, 4, 2, 4, 6, 8};
    streams[1].get_as<std::array<float, 16>>() = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
    for (Slice& stream : streams) stream.data<uint32_t>()[1] = 4;
    Slice encoded = GPU::compile_glsl(VULKAN_GLSL_L2_EXAMPLE);
    ShaderResult result;
    TaskCountdown finished;
    GPU::run(encoded, streams.data(), streams.size(), result, &TaskCountdown::arrive, &finished);
    encoded.free();
    finished.wait();
    result.rethrow();
    require(streams[0].get_as<float>() == 30 && streams[1].get_as<float>() == 4,
        "Translated job-table reduction returned incorrect independent results");
}
/** --------------------------------------------------------------------------------------------------------- Translated Reference Stages
 * @brief Runs four ordered reference stages with distinct dimensions and a shared resource identity.
 */
void translated_reference_stages(const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking translated four-stage reference dispatch and ring wraparound";
    constexpr std::string_view copy = R"glsl(
void alligator_main(Slice source, Slice destination) {
    if (gl_LocalInvocationIndex != 0u || gl_WorkGroupID.x != 0u || gl_WorkGroupID.y != 0u) return;
    slice_store_u32(destination, 0u, slice_load_u32(source, 0u) + 1u);
    slice_store_u32(destination, 1u, gl_NumWorkGroups.x);
}
)glsl";
    constexpr std::string_view multiply = R"glsl(
void alligator_main(Slice source, Slice destination) {
    if (gl_LocalInvocationIndex != 0u || gl_WorkGroupID.x != 0u || gl_WorkGroupID.y != 0u) return;
    slice_store_u32(destination, 0u, slice_load_u32(destination, 0u) * 2u);
    slice_store_u32(destination, 2u, gl_NumWorkGroups.x);
}
)glsl";
    constexpr std::string_view add_resource = R"glsl(
void alligator_main(Slice source, Slice destination) {
    if (gl_LocalInvocationIndex != 0u || gl_WorkGroupID.x != 0u || gl_WorkGroupID.y != 0u) return;
    slice_store_u32(destination, 0u, slice_load_u32(destination, 0u) + slice_load_u32(vulkan_resources(), 0u));
    slice_store_u32(destination, 3u, gl_NumWorkGroups.y);
}
)glsl";
    constexpr std::string_view finish = R"glsl(
void alligator_main(Slice source, Slice destination) {
    if (gl_LocalInvocationIndex != 0u || gl_WorkGroupID.x != 0u || gl_WorkGroupID.y != 0u) return;
    slice_store_u32(destination, 0u, slice_load_u32(destination, 0u) ^ 16u);
    slice_store_u32(destination, 4u, gl_NumWorkGroups.x);
    slice_store_u32(destination, 5u, gl_NumWorkGroups.y);
}
)glsl";
    const std::array<KernelGpuStage, 4> stages{{{copy, 1, 1, true}, {multiply, 2, 1, true},
        {add_resource, 1, 2, true}, {finish, 2, 2, true}}};
    std::array<Slice, 2> inputs{Slice(64, placement), Slice(64, placement)};
    std::array<Slice, 2> outputs{Slice(64, placement), Slice(64, placement)};
    std::array<Slice, 2> invocations{Slice(64, placement), Slice(64, placement)};
    inputs[0].get_as<uint32_t>() = 5;
    inputs[1].get_as<uint32_t>() = 11;
    for (size_t index = 0; index < invocations.size(); ++index) {
        invocations[index].data<uint32_t>()[0] = inputs[index].id();
        invocations[index].data<uint32_t>()[1] = outputs[index].id();
    }
    Slice resources(64, placement);
    resources.get_as<uint32_t>() = 7;
    Slice references(64, placement);
    references.data<uint32_t>()[14] = invocations[0].id();
    references.data<uint32_t>()[15] = invocations[1].id();
    references.data<uint32_t>()[0] = invocations[0].id();
    ShaderState shader(stages, "backend_reference_stages", references, resources.id());
    for (uint32_t first : {14u, 15u}) {
        shader.dispatch_references(first, 2);
        require(outputs[0].get_as<uint32_t>() == 3 && outputs[1].get_as<uint32_t>() == 15,
            "Translated reference stages lost ordering, resource lookup, or ring wraparound");
        for (const Slice& destination : outputs) {
            const uint32_t* words = destination.data<uint32_t>();
            require(words[1] == 1 && words[2] == 2 && words[3] == 2 && words[4] == 2 && words[5] == 2,
                "A translated reference stage used incorrect indirect dimensions");
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Region Directory
 * @brief Resolves a retained cross-region copy after its original id and other owners have retired.
 */
void region_directory(bool native, const BuffetDescriptor* placement) {
    LOG_INFO_STREAM << "Checking backend region directory lookup and retained copy ownership";
    Slice original(64, true, placement);
    original.get_as<uint32_t>() = 707;
    const uint32_t first_region = (original.id() >> 3) & 63;
    std::vector<Slice> copies;
    copies.reserve(REGION_SIZE);
    for (size_t index = 0; index < REGION_SIZE; ++index) {
        copies.emplace_back(original);
        if ((index & 65535) == 65535) LOG_INFO_STREAM << "Retained region copies: " << index + 1;
    }
    Slice retained(std::move(copies.back()));
    require(((retained.id() >> 3) & 63) != first_region, "The region test did not cross a region boundary");
    copies.clear();
    original.free();
    Shader shader(source(identity_body, native), "backend_region");
    ShaderResult result;
    TaskCountdown finished;
    shader(retained, result, &TaskCountdown::arrive, &finished);
    finished.wait();
    result.rethrow();
    check_identity(retained);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs native or translated contracts in a fresh explicitly selected GPU process.
 */
int main(int count, char** arguments) {
    try {
        require(count == 2, "Expected source path: native or translated");
        const std::string_view path(arguments[1]);
        require(path == "native" || path == "translated", "Unknown source path");
        const bool native = path == "native";
        const char* selected = std::getenv("ALLIGATOR_GPU_BACKEND");
        require(selected != nullptr, "Select ALLIGATOR_GPU_BACKEND before process startup");
        const std::string_view requested(selected);
        if (requested == "cpu") {
            require(!GPU::exists(), "CPU selection unexpectedly initialized a compute backend");
            LOG_INFO_STREAM << "Skipping backend shader contracts: CPU-only validation requested";
            return 77;
        } else if (requested == "vulkan") {
            require(!native, "Native MSL requires the Metal backend");
            if (!VulkanContext::device_present()) {
                LOG_INFO_STREAM << "Skipping backend shader contracts: no Vulkan device";
                return 77;
            }
        } else if (requested == "metal") {
#if defined(BUFFETALLIGATOR_HAS_METAL)
            if (!metal_available()) {
                LOG_INFO_STREAM << "Skipping backend shader contracts: no Metal device";
                return 77;
            }
#else
            LOG_ERROR_STREAM << "Metal backend contracts require a Metal-enabled build";
            return 1;
#endif
        } else {
            require(false, "Backend shader contracts require an explicit Vulkan or Metal selection");
        }
        Slice cpu_first(64, false, BuffetDescriptors::get(AlignedHeapBuffer::type_idx()));
        cpu_first.get_as<uint32_t>() = 41;
        const GPUDevice& device = gpu_device();
        require(device.kind == (requested == "metal" ? GPUBackend::Metal : GPUBackend::Vulkan),
            "The first CPU allocation changed the explicitly selected GPU backend");
        require(GPU::exists() && GPU::device_name() == device.name,
            "The public GPU identity disagrees with the frozen device");
        LOG_INFO_STREAM << "Backend shader contracts: " << requested << ", source=" << path
            << ", device=" << device.name;
        optimized_noop(native, device.placement);
        typed_io(native, device.placement);
        packed_io(native, device.placement);
        identity_and_placement(native, device.placement);
        embedded_dependencies(native, device.placement);
        dispatch_rounds(native, device.placement);
        coroutine_ownership(native, device.placement);
        concurrent_dispatch(native, device.placement);
        program_roundtrip(native, device.placement);
        if (!native) {
            translated_jobs(device.placement);
            translated_reference_stages(device.placement);
        }
        region_directory(native, device.placement);
        require(cpu_first.get_as<uint32_t>() == 41, "GPU execution corrupted an independent CPU allocation");
        Kitchen::inst().drain();
        LOG_INFO_STREAM << "Backend shader contracts passed";
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Backend shader contracts failed: " << error.what();
        return 1;
    }
}
