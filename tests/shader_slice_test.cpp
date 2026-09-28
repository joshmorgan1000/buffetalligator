/** --------------------------------------------------------------------------------------------------------- Shader Slice Test
 * @file shader_slice_test.cpp
 * @brief Exercises four-byte shader Slice identities and live GPU buffer table resolution.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <string_view>
#include <vector>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Slice Identity Shader
 * @brief Reads compact Slice members and reports their shared buffer records through two streams.
 */
static constexpr std::string_view SLICE_IDENTITY_SHADER = R"glsl(
struct SlicePair {
    uint marker;
    Slice slices[2];
    uint trailer;
};
layout(buffer_reference, std430, buffer_reference_align = 4) buffer SlicePairRef {
    SlicePair pair;
};
void vulkan_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    SlicePair pair = SlicePairRef(slice_address(stream)).pair;
    slice_store_u32(stream, 4u, pair.marker);
    slice_store_u32(stream, 5u, pair.slices[0].id);
    slice_store_u32(stream, 6u, pair.slices[1].id);
    slice_store_u32(stream, 7u, pair.trailer);
    slice_store_u64(stream, 4u, slice_address(pair.slices[0]));
    slice_store_u64(stream, 5u, gpu_slice_address(pair.slices[1].id));
    slice_store_u32(stream, 12u, slice_size(pair.slices[0]));
    slice_store_u32(stream, 13u, gpu_slice_size(pair.slices[1].id));
    slice_store_u32(stream, 14u, slice_read(slice_address(stream) + 4ul).id);
    slice_store_u32(stream, 15u, slice_read(slice_address(stream) + 8ul).id);
    slice_store_u32(stream, 16u, stream.id);
    slice_store_u32(stream, 17u, slice_size(stream));
    slice_store_u32(stream, 18u, slice_is_null(gpu_slice(0xFFFFFFFFu)) ? 1u : 0u);
    slice_store_u32(stream, 19u, slice_is_null(pair.slices[0]) ? 1u : 0u);
    slice_store_u32(stream, 20u, vulkan_count());
    slice_store_u32(stream, 21u, vulkan_index());
    slice_store_u32(stream, 22u, slice_load_u32(pair.slices[0], 0u));
    slice_store_u32(stream, 23u, slice_load_u32(gpu_slice(pair.slices[1].id), 0u));
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Seed Stream
 * @brief Writes the four-byte Slice identities between adjacent marker words.
 */
static void seed_stream(Slice& stream, const std::array<Slice, 2>& payloads) {
    uint32_t* words = stream.data<uint32_t>();
    words[0] = 0x13579BDFu;
    words[1] = payloads[0].id();
    words[2] = payloads[1].id();
    words[3] = 0x2468ACE0u;
}
/** --------------------------------------------------------------------------------------------------------- Verify Stream
 * @brief Checks shader member layout, lookup results, and dispatch identity.
 */
static void verify_stream(
    const Slice& stream,
    const std::array<Slice, 2>& payloads,
    uint32_t stream_index,
    uint64_t first_address,
    uint64_t second_address,
    uint32_t first_size,
    uint32_t first_value
) {
    const uint32_t* words = stream.data<uint32_t>();
    const uint64_t* addresses = stream.data<uint64_t>();
    require(words[4] == 0x13579BDFu && words[7] == 0x2468ACE0u,
        "GLSL Slice members changed the surrounding struct layout");
    require(words[5] == payloads[0].id() && words[6] == payloads[1].id(),
        "GLSL Slice arrays do not use the host's four-byte identity stride");
    require(addresses[4] == first_address && addresses[5] == second_address,
        "GLSL Slice address did not resolve the shared buffer base plus offset");
    require(words[12] == first_size && words[13] == 48,
        "GLSL Slice size did not resolve the shared buffer record");
    require(words[14] == payloads[0].id() && words[15] == payloads[1].id(),
        "slice_read did not read four-byte identities at four-byte alignment");
    require(words[16] == stream.id() && words[17] == stream.size_bytes(),
        "direct dispatch did not preserve the stream's Slice identity");
    require(words[18] == 1 && words[19] == 0,
        "GLSL Slice nullness did not use the null identity sentinel");
    require(words[20] == 2 && words[21] == stream_index,
        "direct dispatch did not bind two separate stream columns");
    require(words[22] == first_value && words[23] == 0x55667788u,
        "GLSL Slice loads did not use the resolved view addresses");
}
/** --------------------------------------------------------------------------------------------------------- Direct Dispatch
 * @brief Checks compact Slice layout and observes host record edits through a prepared shader.
 */
static void direct_dispatch() {
    Slice storage(512, false, Placemat::HOST_VISIBLE);
    Slice parent = storage.slice(128, 160);
    std::array<Slice, 2> streams{storage.slice(16, 96), parent.slice(32, 128)};
    Slice payload_storage(128, false, Placemat::HOST_VISIBLE);
    std::array<Slice, 2> payloads{
        payload_storage.slice(16, 32), payload_storage.slice(64, 48)};
    payload_storage.data<uint32_t>()[4] = 0x11223344u;
    payload_storage.data<uint32_t>()[5] = 0x99AABBCCu;
    payload_storage.data<uint32_t>()[16] = 0x55667788u;
    seed_stream(streams[0], payloads);
    seed_stream(streams[1], payloads);
    GPUBuf* record = Alligator::gpubuf_for(payloads[0]);
    const GPUBuf* root_record = Alligator::gpubuf_for(payload_storage);
    require(record == Alligator::gpu_table() + payloads[0].id(),
        "host Slice lookup did not return its shared GPU table record");
    require(record->address == root_record->address
        && record->offset == root_record->offset + 16,
        "GPU sub-slicing changed the buffer base instead of its offset");
    const uint64_t payload_address = VulkanKernel::device_address(payload_storage);
    ShaderState shader(SLICE_IDENTITY_SHADER, "shader_slice_identity_test");
    shader.dispatch(streams.data(), streams.size(), 1);
    verify_stream(streams[0], payloads, 0, payload_address + 16, payload_address + 64,
        32, 0x11223344u);
    verify_stream(streams[1], payloads, 1, payload_address + 16, payload_address + 64,
        32, 0x11223344u);
    const GPUBuf original_record = *record;
    record->offset += sizeof(uint32_t);
    record->size = 0;
    shader.dispatch(streams.data(), streams.size(), 1);
    *record = original_record;
    verify_stream(streams[0], payloads, 0, payload_address + 20, payload_address + 64,
        0, 0x99AABBCCu);
    verify_stream(streams[1], payloads, 1, payload_address + 20, payload_address + 64,
        0, 0x99AABBCCu);
}
/** --------------------------------------------------------------------------------------------------------- Reference Dispatch
 * @brief Resolves invocation and payload identities through the prepared reference ring.
 */
static void reference_dispatch() {
    Slice input_storage(96, false, Placemat::HOST_VISIBLE);
    Slice output_storage(96, false, Placemat::HOST_VISIBLE);
    Slice invocation_storage(32, false, Placemat::HOST_VISIBLE);
    std::array<Slice, 2> inputs{input_storage.slice(8, 16), input_storage.slice(48, 24)};
    std::array<Slice, 2> outputs{output_storage.slice(8, 24), output_storage.slice(48, 24)};
    std::array<Slice, 2> invocations{
        invocation_storage.slice(4, 8), invocation_storage.slice(20, 8)};
    Slice references(4 * sizeof(uint32_t), false, Placemat::HOST_VISIBLE);
    for (size_t index = 0; index < inputs.size(); ++index) {
        inputs[index].data<uint32_t>()[0] = 101 + uint32_t(index);
        invocations[index].data<uint32_t>()[0] = inputs[index].id();
        invocations[index].data<uint32_t>()[1] = outputs[index].id();
        references.data<uint32_t>()[index + 1] = invocations[index].id();
    }
    constexpr std::string_view source = R"glsl(
void vulkan_main(Slice source_slice, Slice destination_slice) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32(destination_slice, 0u, source_slice.id);
    slice_store_u32(destination_slice, 1u, slice_size(source_slice));
    slice_store_u64(destination_slice, 1u, slice_address(source_slice));
    slice_store_u32(destination_slice, 4u, slice_load_u32(source_slice, 0u));
    slice_store_u32(destination_slice, 5u, destination_slice.id);
}
)glsl";
    ShaderState shader(source, "shader_slice_reference_test", &references);
    shader.dispatch_references(1, 2);
    for (size_t index = 0; index < outputs.size(); ++index) {
        const uint32_t* result = outputs[index].data<uint32_t>();
        require(result[0] == inputs[index].id() && result[5] == outputs[index].id(),
            "reference dispatch did not preserve input and output Slice identities");
        require(result[1] == inputs[index].size_bytes(),
            "reference dispatch resolved the wrong input size");
        require(outputs[index].data<uint64_t>()[1] == VulkanKernel::device_address(inputs[index]),
            "reference dispatch resolved the wrong input address");
        require(result[4] == 101 + uint32_t(index),
            "reference dispatch did not follow the requested ring range");
    }
}
/** --------------------------------------------------------------------------------------------------------- Job Dispatch
 * @brief Runs the public L2 shader example over two views through the compiled job interface.
 */
static void job_dispatch() {
    Slice storage(160, false, Placemat::HOST_VISIBLE);
    std::array<Slice, 2> streams{storage.slice(16, 48), storage.slice(96, 48)};
    uint32_t* first_header = streams[0].data<uint32_t>();
    uint32_t* second_header = streams[1].data<uint32_t>();
    first_header[1] = 4;
    second_header[1] = 4;
    float* first = streams[0].data<float>();
    float* second = streams[1].data<float>();
    first[4] = 1.0f;
    first[5] = 2.0f;
    first[6] = 3.0f;
    first[7] = 4.0f;
    first[8] = 2.0f;
    first[9] = 4.0f;
    first[10] = 6.0f;
    first[11] = 8.0f;
    second[4] = 2.0f;
    second[5] = 4.0f;
    second[6] = 6.0f;
    second[7] = 8.0f;
    second[8] = 1.0f;
    second[9] = 1.0f;
    second[10] = 1.0f;
    second[11] = 1.0f;
    Slice program = GPU::compile_glsl(VULKAN_GLSL_L2_EXAMPLE);
    GPU::run(program, streams.data(), streams.size());
    require(first[0] == 30.0f && second[0] == 84.0f,
        "compiled job dispatch did not resolve both Slice views");
    require(first_header[1] == 4 && second_header[1] == 4,
        "compiled job dispatch wrote outside its result fields");
}
/** --------------------------------------------------------------------------------------------------------- Embedded Slices
 * @struct EmbeddedSlices
 * @brief Owns the source and destination views consumed directly by a shader from mapped storage.
 */
struct EmbeddedSlices {
    uint32_t marker;
    Slice source;
    Slice destination;
    uint32_t trailer;
};
static_assert(sizeof(EmbeddedSlices) == 16 && offsetof(EmbeddedSlices, source) == 4
    && offsetof(EmbeddedSlices, destination) == 8 && offsetof(EmbeddedSlices, trailer) == 12);
/** --------------------------------------------------------------------------------------------------------- Embedded Handles
 * @brief Keeps dedicated buffers alive through mapped Slice objects after their parents are freed.
 */
static void embedded_handles() {
    Slice source_storage(128, true, Placemat::HOST_VISIBLE);
    Slice destination_storage(128, true, Placemat::HOST_VISIBLE);
    Slice source_parent = source_storage.slice(16, 80);
    Slice destination_parent = destination_storage.slice(16, 96);
    SliceT<uint32_t> source_view(source_parent.slice(16, 16));
    Slice destination_view = SliceId(destination_parent.id()).slice(32, 16);
    uint32_t* source_words = source_storage.data<uint32_t>();
    uint32_t* destination_words = destination_storage.data<uint32_t>();
    source_words[7] = 0xAABBCCDDu;
    source_words[8] = 3;
    source_words[9] = 5;
    source_words[10] = 7;
    source_words[11] = 11;
    source_words[12] = 0xDDCCBBAAu;
    destination_words[11] = 0x11223344u;
    destination_words[16] = 0x44332211u;
    Slice descriptors(sizeof(EmbeddedSlices), false, Placemat::HOST_VISIBLE);
    EmbeddedSlices* pair = std::construct_at(descriptors.data<EmbeddedSlices>(), EmbeddedSlices{
        0x13579BDFu, std::move(source_view.root_slice()), std::move(destination_view), 0x2468ACE0u});
    const uint32_t source_id = pair->source.id();
    const uint32_t destination_id = pair->destination.id();
    source_parent.free();
    destination_parent.free();
    source_storage.free();
    destination_storage.free();
    require(source_view.is_null() && destination_view.is_null(),
        "moving views into mapped Slice members did not transfer ownership");
    constexpr std::string_view body = R"glsl(
struct EmbeddedSlices {
    uint marker;
    Slice source_slice;
    Slice destination_slice;
    uint trailer;
};
layout(buffer_reference, std430, buffer_reference_align = 4) buffer EmbeddedSlicesRef {
    EmbeddedSlices pair;
};
void vulkan_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    EmbeddedSlices pair = EmbeddedSlicesRef(slice_address(stream)).pair;
    slice_store_u32x4(pair.destination_slice, 0u,
        slice_load_u32x4(pair.source_slice, 0u) + uvec4(10u, 20u, 30u, 40u));
}
)glsl";
    Shader shader(body, "vulkan_embedded_slices_test");
    auto completion = shader(descriptors);
    require(completion->tryWait(), "public Shader did not signal completion");
    require(destination_words[12] == 13 && destination_words[13] == 25
        && destination_words[14] == 37 && destination_words[15] == 51,
        "GPU did not resolve owning C++ Slice members after their parents were freed");
    require(destination_words[11] == 0x11223344u && destination_words[16] == 0x44332211u,
        "GPU wrote outside the surviving destination sub-slice");
    require(source_words[7] == 0xAABBCCDDu && source_words[12] == 0xDDCCBBAAu
        && source_words[8] == 3 && source_words[11] == 11,
        "GPU modified the surviving source sub-slice or its guards");
    require(pair->marker == 0x13579BDFu && pair->trailer == 0x2468ACE0u
        && pair->source.id() == source_id && pair->destination.id() == destination_id,
        "GPU modified the host's embedded Slice identities");
    Slice surviving_destination = pair->destination;
    std::destroy_at(pair);
    descriptors.free();
    require(surviving_destination.data<uint32_t>()[0] == 13
        && surviving_destination.data<uint32_t>()[3] == 51,
        "copying a mapped Slice member did not retain its GPU-written data");
}
/** --------------------------------------------------------------------------------------------------------- Dispatch Rounds
 * @brief Checks pool identities and live list lengths across full, partial, and repeated dispatch rounds.
 */
static void dispatch_rounds() {
    constexpr std::string_view body = R"glsl(
void vulkan_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32x4(stream, 0u, uvec4(stream.id, vulkan_count(), vulkan_index(),
        slice_load_u32(stream, 3u) + 1u));
}
)glsl";
    ShaderState shader(body, "vulkan_dispatch_rounds_test");
    const size_t capacity = shader.capacity();
    const size_t count = capacity + 3;
    Slice storage(count * 16 + 32, false, Placemat::HOST_VISIBLE);
    storage.data<uint32_t>()[0] = 0x13579BDFu;
    storage.data<uint32_t>()[storage.size<uint32_t>() - 1] = 0x2468ACE0u;
    std::vector<Slice> streams;
    streams.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        streams.push_back(storage.slice(16 + index * 16, 16));
        streams.back().data<uint32_t>()[3] = 7;
    }
    shader.dispatch(streams.data(), streams.size(), 1);
    for (size_t index = 0; index < count; ++index) {
        const uint32_t* result = streams[index].data<uint32_t>();
        require(result[0] == streams[index].id() && result[1] == (index < capacity ? capacity : 3)
            && result[2] == index % capacity && result[3] == 8,
            "a full or partial dispatch round used the wrong Slice identity or list size");
    }
    shader.dispatch(streams.data() + capacity, 1, 1);
    const uint32_t* single = streams[capacity].data<uint32_t>();
    require(single[0] == streams[capacity].id() && single[1] == 1
        && single[2] == 0 && single[3] == 9,
        "a shorter dispatch retained the previous list length or Slice identity");
    require(streams[capacity - 1].data<uint32_t>()[3] == 8
        && streams[capacity + 1].data<uint32_t>()[3] == 8,
        "a shorter dispatch touched neighboring slices");
    shader.dispatch(streams.data(), streams.size(), 1);
    for (size_t index = 0; index < count; ++index) {
        const uint32_t* result = streams[index].data<uint32_t>();
        require(result[0] == streams[index].id() && result[1] == (index < capacity ? capacity : 3)
            && result[2] == index % capacity && result[3] == (index == capacity ? 10 : 9),
            "reusing a prepared dispatch did not restore its full parameter capacity");
    }
    require(storage.data<uint32_t>()[0] == 0x13579BDFu
        && storage.data<uint32_t>()[storage.size<uint32_t>() - 1] == 0x2468ACE0u,
        "dispatch rounds wrote outside their backing storage");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs shader identity regression cases when a Vulkan compute device is available.
 */
int main(int count, char** arguments) {
    if (!VulkanKernel::available()) {
        LOG_INFO_STREAM << "Skipping shader Slice tests: no Vulkan compute device";
        return 77;
    }
    LOG_INFO_STREAM << "Running shader Slice tests on " << VulkanKernel::device_name();
    return functional::run(count, arguments, {
        {"vulkan_shader_identity", &direct_dispatch},
        {"vulkan_reference_dispatch", &reference_dispatch},
        {"vulkan_job_dispatch", &job_dispatch},
        {"vulkan_embedded_slices", &embedded_handles},
        {"vulkan_dispatch_rounds", &dispatch_rounds}
    });
}
