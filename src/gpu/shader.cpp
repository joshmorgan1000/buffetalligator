/** --------------------------------------------------------------------------------------------------------- Shader Completion
 * @file shader.cpp
 * @brief Shares admission, resource retention, completion, and portable program caching across backends.
 */
#include <alligator/easygpu.hpp>
#include <alligator/kitchen.hpp>
#include <vulkan/shader_state.hpp>
#include <gpu/runtime.hpp>
#include <memory/lifetime.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <openssl/evp.h>

namespace buffetalligator {
ShaderProgram::~ShaderProgram() { if (ops) ops->destroy(*this); }
bool GPU::exists() { return gpu_device().kind != GPUBackend::CPU; }
bool GPU::unified_memory() { return gpu_device().unified; }
std::string GPU::device_name() { return gpu_device().name; }
DeviceMemoryUsage GPU::memory_usage() { return gpu_memory_usage(); }
uint32_t GPU::thread_limit_x() { return gpu_thread_limit_x(); }
uint32_t GPU::thread_limit_y() { return gpu_thread_limit_y(); }
uint32_t GPU::thread_limit_z() { return gpu_thread_limit_z(); }
uint32_t GPU::workgroup_limit_x() { return gpu_workgroup_limit_x(); }
uint32_t GPU::workgroup_limit_y() { return gpu_workgroup_limit_y(); }
uint32_t GPU::workgroup_limit_z() { return gpu_workgroup_limit_z(); }
/** --------------------------------------------------------------------------------------------------------- Shader Runtime
 * @brief Tracks accepted operations and immutable cached programs through arena teardown.
 */
struct ShaderRuntime {
    struct CacheEntry {
        std::vector<uint8_t> bytes;
        std::shared_ptr<ShaderState> state;
    };
    std::mutex mutex;
    std::condition_variable idle;
    size_t active = 0;
    bool stopping = false;
    std::mutex cache_mutex;
    std::vector<CacheEntry> cache;
};
ShaderRuntime& shader_runtime() {
    static RuntimeFinalizer lifetime(new ShaderRuntime, &RuntimeFinalizer::delete_owner<ShaderRuntime>);
    return *static_cast<ShaderRuntime*>(lifetime.object);
}
/** --------------------------------------------------------------------------------------------------------- Shader Preparation
 * @brief Counts in-progress preparation before final shutdown closes admission.
 */
struct ShaderPreparation {
    ShaderPreparation() {
        std::lock_guard lock(shader_runtime().mutex);
        if (shader_runtime().stopping) ALLIGATOR_GPU_THROW("GPU shutdown has closed preparation");
        ++shader_runtime().active;
    }
    ~ShaderPreparation() {
        std::lock_guard lock(shader_runtime().mutex);
        --shader_runtime().active;
        shader_runtime().idle.notify_all();
    }
};
void shader_runtime_initialize() { (void)shader_runtime(); }
#ifdef BUFFETALLIGATOR_SHADER_TESTING
static std::atomic<bool> shader_test_fail_admission{false};
static std::atomic<bool> shader_test_hold_admission{false};
static std::atomic<bool> shader_test_admission_entered{false};
#endif
/** --------------------------------------------------------------------------------------------------------- Shader State Resources
 * @brief Shares a backend program while admitting independent stable submission slots.
 */
struct ShaderState::Impl {
    using Format = ShaderFormat;
    Format format;
    std::string name;
    std::array<std::string, 3> sources;
    std::unique_ptr<ShaderProgram> program;
    std::vector<KernelGpuStage> stages;
    uint32_t reference_workgroups_x = 1;
    std::mutex admission;
    std::condition_variable availability;
    explicit Impl(const ShaderPrepareInfo& info)
        : format(info.format), name(info.name),
          sources{std::string(info.source.glsl), std::string(info.source.metal), std::string(info.source.cuda)},
          stages(info.stages.begin(), info.stages.end()) {
        const auto prepare = gpu_device().prepare;
        if (!prepare) ALLIGATOR_GPU_THROW("Shader preparation requires a compatible compute device");
        program = prepare(info);
    }
    ShaderSlot* acquire() {
        std::unique_lock lock(admission);
        for (;;) {
            for (const auto& slot : program->slots) {
                if (!slot->busy) { slot->busy = true; return slot.get(); }
            }
            if (availability.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout)
                LOG_INFO_STREAM << "Waiting for a prepared GPU submission slot";
        }
    }
    void retire(ShaderSlot* slot) {
        std::lock_guard lock(admission);
        slot->busy = false;
        availability.notify_one();
    }
    void submit(ShaderSlot& slot, uint32_t width, uint32_t height, uint32_t depth) {
        program->ops->submit(*program, slot, width, height, depth);
        program->ops->wait(*program, slot);
    }
};
/** --------------------------------------------------------------------------------------------------------- Source Preparation
 * @brief Owns source bytes before preparing the selected backend's resident parameter layout.
 */
ShaderState::ShaderState(const ShaderSource& source, std::string_view name, bool jobs) {
    ShaderPreparation preparation;
    ShaderPrepareInfo info{source, name, jobs ? ShaderFormat::Jobs : ShaderFormat::Slices};
    impl_ = std::make_shared<Impl>(info);
}
/** --------------------------------------------------------------------------------------------------------- Private Source Preparation
 * @brief Prepares private reference-ring or direct-list execution on the selected device.
 */
ShaderState::ShaderState(std::string_view source, std::string_view name,
    const Slice* references, uint32_t workgroups_x) {
    ShaderPreparation preparation;
    ShaderPrepareInfo info{ShaderSource{source, {}, {}}, name,
        references ? ShaderFormat::References : ShaderFormat::Slices};
    info.references = references;
    if (references) info.capacity = references->size<uint32_t>();
    impl_ = std::make_shared<Impl>(info);
    if (!workgroups_x || workgroups_x > impl_->program->max_workgroups_x)
        ALLIGATOR_GPU_THROW("Kernel workgroups exceed the device limit");
    impl_->reference_workgroups_x = workgroups_x;
}
/** --------------------------------------------------------------------------------------------------------- Stage Preparation
 * @brief Prepares a nonempty reference-ring pipeline sequence.
 */
ShaderState::ShaderState(std::span<const KernelGpuStage> stages, std::string_view name,
    const Slice& references, uint32_t resources) {
    ShaderPreparation preparation;
    if (stages.empty()) ALLIGATOR_GPU_THROW("Kernel preparation requires at least one stage");
    ShaderPrepareInfo info{{}, name, ShaderFormat::References};
    info.references = &references;
    info.capacity = references.size<uint32_t>();
    info.stages = stages;
    info.resources = resources;
    impl_ = std::make_shared<Impl>(info);
}
/** --------------------------------------------------------------------------------------------------------- SPIR-V Preparation
 * @brief Prepares private precompiled job code in the active backend.
 */
ShaderState::ShaderState(const uint32_t* words, size_t count, std::string_view name, size_t max_jobs) {
    ShaderPreparation preparation;
    ShaderPrepareInfo info{{}, name, ShaderFormat::Jobs};
    info.capacity = max_jobs;
    info.words = std::span<const uint32_t>(words, count);
    impl_ = std::make_shared<Impl>(info);
}
ShaderState::~ShaderState() = default;
const std::vector<uint32_t>& ShaderState::spirv() const { return impl_->program->spirv; }
const std::string& ShaderState::name() const { return impl_->name; }
size_t ShaderState::capacity() const { return impl_->program->capacity; }
/** --------------------------------------------------------------------------------------------------------- ShaderState::dispatch
 * @brief Dispatches the given number of workgroups for the shader.
 * @param streams The array of input slices.
 * @param count The number of input slices.
 * @param workgroups The number of workgroups to dispatch.
 */
void ShaderState::dispatch(const Slice* streams, size_t count, uint32_t workgroups) const {
    if (!workgroups || workgroups > impl_->program->max_workgroups_x)
        ALLIGATOR_GPU_THROW("Shader workgroups exceed the device limit");
    ShaderSlot* slot = impl_->acquire();
    try {
        for (size_t first = 0; first < count; first += impl_->program->capacity) {
            const size_t round = std::min(impl_->program->capacity, count - first);
            uint32_t* entries = slot->parameters.data<uint32_t>();
            for (size_t index = 0; index < round; ++index) entries[index] = streams[first + index].id();
            impl_->submit(*slot, workgroups, uint32_t(round), 1);
        }
    } catch (...) { impl_->retire(slot); throw; }
    impl_->retire(slot);
}
/** --------------------------------------------------------------------------------------------------------- Dispatch References
 * @brief Runs one reference-ring range using an exclusively admitted prepared slot.
 */
void ShaderState::dispatch_references(uint32_t first, uint32_t count) {
    ShaderSlot* slot = impl_->acquire();
    try {
        reinterpret_cast<uint32_t*>(slot->mapped)[3] = first;
        for (size_t index = 0; index < impl_->stages.size(); ++index) {
            const auto& stage = impl_->stages[index];
            uint32_t* dimensions = reinterpret_cast<uint32_t*>(slot->mapped + 32 + index * 16);
            dimensions[0] = stage.workgroups_x;
            dimensions[1] = stage.workgroups_y;
            dimensions[2] = stage.per_request ? count : 1;
        }
        impl_->submit(*slot, impl_->reference_workgroups_x, 1, count);
    } catch (...) { impl_->retire(slot); throw; }
    impl_->retire(slot);
}
/** --------------------------------------------------------------------------------------------------------- Shader Operation
 * @brief Retains a prepared slot, exact Slice identities, and synchronized completion ownership.
 */
struct ShaderOperation {
    std::shared_ptr<ShaderState::Impl> prepared;
    ShaderSlot* slot = nullptr;
    std::vector<Slice> streams;
    std::vector<Slice> dependencies;
    uint32_t workgroups = 1;
    void (*done)(void*) = nullptr;
    void* context = nullptr;
    std::recursive_mutex gate;
    std::atomic<bool> retired{false};
    bool cancelled = false;
    std::exception_ptr error;
    std::coroutine_handle<> continuation{};
    std::shared_ptr<ShaderOperation> self;
    /** ------------------------------------------------------------------------------------------- Retain
     * @brief Pins an existing table identity without copying its payload or allocating another id.
     */
    static Slice retain(const Slice& slice) {
        if (slice.is_null() || (slice.id() & 7) != gpu_device().placement->type_idx
            || Alligator::gpubuf_for(slice)->address == 0)
            ALLIGATOR_GPU_THROW("Shader admission requires a device-visible Slice");
        SliceEntry::from_slice(slice)->owners.fetch_add(1, std::memory_order_relaxed);
        return Slice(Slice::AdoptId{}, slice.id());
    }
    /** ------------------------------------------------------------------------------------------- Execute
     * @brief Retires all rounds on a Kitchen worker and publishes the terminal result.
     */
    static void execute(void* context) noexcept {
        auto& operation = *static_cast<ShaderOperation*>(context);
        try {
            const size_t capacity = operation.prepared->program->capacity;
            const bool jobs = operation.prepared->format == ShaderState::Impl::Format::Jobs;
            for (size_t first = 0; first < operation.streams.size(); first += capacity) {
                const size_t count = std::min(capacity, operation.streams.size() - first);
                uint32_t* records = operation.slot->parameters.data<uint32_t>();
                for (size_t index = 0; index < count; ++index) {
                    records[index * (jobs ? 8 : 1)] = operation.streams[first + index].id();
                    if (jobs) {
                        uint64_t* handle = reinterpret_cast<uint64_t*>(records + index * 8 + 4);
                        handle[0] = 0;
                        handle[1] = 0;
                    }
                }
                operation.prepared->submit(*operation.slot, operation.workgroups, jobs ? 1 : uint32_t(count),
                    jobs ? uint32_t(count) : 1);
            }
        } catch (...) {
            std::lock_guard lock(operation.gate);
            operation.error = std::current_exception();
        }
        operation.prepared->retire(operation.slot);
        operation.slot = nullptr;
        operation.retired.store(true, std::memory_order_release);
        operation.retired.notify_all();
    }
    /** ------------------------------------------------------------------------------------------- Complete
     * @brief Runs callbacks and continuations after the Order retires device work.
     */
    static void complete(void* context) noexcept {
        auto& operation = *static_cast<ShaderOperation*>(context);
        std::shared_ptr<ShaderOperation> retained = std::move(operation.self);
        {
            std::lock_guard lock(operation.gate);
            if (operation.done) {
                try { operation.done(operation.context); }
                catch (...) { operation.error = std::current_exception(); }
            }
            const std::coroutine_handle<> continuation = operation.continuation;
            operation.continuation = {};
            if (continuation && !operation.cancelled) {
                try { continuation.resume(); }
                catch (...) { operation.error = std::current_exception(); }
            }
            operation.streams.clear();
            operation.dependencies.clear();
            operation.prepared.reset();
        }
        std::lock_guard runtime_lock(shader_runtime().mutex);
        --shader_runtime().active;
        shader_runtime().idle.notify_all();
    }
    /** ------------------------------------------------------------------------------------------- Accept
     * @brief Queues device work and completion together in one borrowed Order.
     */
    static void accept(const std::shared_ptr<ShaderOperation>& operation) {
        {
            std::lock_guard lock(shader_runtime().mutex);
            if (shader_runtime().stopping) {
                operation->prepared->retire(operation->slot);
                operation->slot = nullptr;
                ALLIGATOR_GPU_THROW("GPU shutdown has closed admission");
            }
            operation->self = operation;
            ++shader_runtime().active;
        }
        try {
#ifdef BUFFETALLIGATOR_SHADER_TESTING
            if (shader_test_fail_admission.exchange(false, std::memory_order_acq_rel))
                throw std::bad_alloc();
#endif
            Kitchen::submit(Order(
                &ShaderOperation::execute,
                operation.get(),
                &ShaderOperation::complete,
                operation.get()
            ));
        } catch (...) {
            operation->self.reset();
            operation->prepared->retire(operation->slot);
            operation->slot = nullptr;
            std::lock_guard lock(shader_runtime().mutex);
            --shader_runtime().active;
            shader_runtime().idle.notify_all();
            throw;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Start
 * @brief Captures resources and acquires a prepared slot before any asynchronous ownership is accepted.
 */
std::shared_ptr<ShaderOperation> ShaderState::start(const Slice* streams, size_t count,
    void (*done)(void*), void* context, uint32_t workgroups,
    std::span<const Slice> dependencies) const {
    if (!workgroups || workgroups > impl_->program->max_workgroups_x)
        ALLIGATOR_GPU_THROW("Shader workgroups exceed the device limit");
    auto operation = std::make_shared<ShaderOperation>();
    operation->prepared = impl_;
    operation->done = done;
    operation->context = context;
    operation->workgroups = workgroups;
    operation->streams.reserve(count);
    operation->dependencies.reserve(dependencies.size());
    for (size_t index = 0; index < count; ++index)
        operation->streams.push_back(ShaderOperation::retain(streams[index]));
    for (const Slice& dependency : dependencies)
        operation->dependencies.push_back(ShaderOperation::retain(dependency));
    operation->slot = impl_->acquire();
    return operation;
}
/** --------------------------------------------------------------------------------------------------------- Drain
 * @brief Retires accepted work and cached programs before arena or device teardown.
 */
void ShaderState::drain() {
    {
        std::unique_lock lock(shader_runtime().mutex);
        shader_runtime().stopping = true;
        while (shader_runtime().active != 0) {
            if (shader_runtime().idle.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout)
                LOG_INFO_STREAM << "Draining accepted GPU operations";
        }
    }
    std::lock_guard cache_lock(shader_runtime().cache_mutex);
    shader_runtime().cache.clear();
}
/** --------------------------------------------------------------------------------------------------------- Shader Result Special Members
 * @brief Keeps opaque operation ownership outside public headers.
 */
ShaderResult::ShaderResult() = default;
ShaderResult::ShaderResult(const ShaderResult&) = default;
ShaderResult& ShaderResult::operator=(const ShaderResult&) = default;
ShaderResult::ShaderResult(ShaderResult&&) noexcept = default;
ShaderResult& ShaderResult::operator=(ShaderResult&&) noexcept = default;
ShaderResult::~ShaderResult() = default;
bool ShaderResult::ready() const { return operation_ && operation_->retired.load(std::memory_order_acquire); }
void ShaderResult::rethrow() const {
    if (!ready()) ALLIGATOR_GPU_THROW("Shader result has not retired");
    std::lock_guard lock(operation_->gate);
    if (operation_->error) std::rethrow_exception(operation_->error);
    if (operation_->cancelled) ALLIGATOR_GPU_THROW("Shader continuation was cancelled");
}
void ShaderResult::cancel() const {
    if (!operation_) return;
    std::lock_guard lock(operation_->gate);
    operation_->continuation = {};
    if (!operation_->retired.load(std::memory_order_acquire)) operation_->cancelled = true;
}
/** --------------------------------------------------------------------------------------------------------- Shader Awaiter
 * @brief Serializes continuation publication, cancellation, and worker resumption.
 */
ShaderAwaiter::ShaderAwaiter(ShaderResult result) : result_(std::move(result)) {}
ShaderAwaiter::ShaderAwaiter(ShaderAwaiter&&) noexcept = default;
ShaderAwaiter& ShaderAwaiter::operator=(ShaderAwaiter&& other) noexcept {
    if (this != &other) { result_.cancel(); result_ = std::move(other.result_); }
    return *this;
}
ShaderAwaiter::~ShaderAwaiter() { result_.cancel(); }
bool ShaderAwaiter::await_ready() const { return result_.ready(); }
bool ShaderAwaiter::await_suspend(std::coroutine_handle<> continuation) {
    std::shared_ptr<ShaderOperation> operation = result_.operation_;
    std::lock_guard lock(operation->gate);
    if (operation->retired.load(std::memory_order_acquire)) return false;
    operation->continuation = continuation;
    return true;
}
void ShaderAwaiter::await_resume() { result_.rethrow(); }
ShaderResult ShaderAwaiter::result() const { return result_; }
/** --------------------------------------------------------------------------------------------------------- Shader Construction
 * @brief Prepares owned native source alternatives behind the one-pointer public handle.
 */
Shader::Shader(const ShaderSource& source, std::string_view name)
    : state_(std::make_unique<ShaderState>(source, name)) {}
Shader::Shader(std::string_view source, std::string_view name) : Shader(ShaderSource{source, {}, {}}, name) {}
Shader::Shader(std::unique_ptr<ShaderState> state) : state_(std::move(state)) {}
Shader::Shader(Shader&&) noexcept = default;
Shader& Shader::operator=(Shader&&) noexcept = default;
Shader::~Shader() = default;
void Shader::operator()(const Slice* slices, size_t count, ShaderResult& result,
    void (*done)(void*), void* context, uint32_t workgroups,
    std::span<const Slice> dependencies) const {
    ShaderPreparation admission;
    if (!state_) ALLIGATOR_GPU_THROW("A moved-from Shader cannot dispatch");
    auto operation = state_->start(slices, count, done, context, workgroups, dependencies);
#ifdef BUFFETALLIGATOR_SHADER_TESTING
    if (shader_test_hold_admission.load(std::memory_order_acquire)) {
        shader_test_admission_entered.store(true, std::memory_order_release);
        shader_test_admission_entered.notify_all();
        shader_test_hold_admission.wait(true, std::memory_order_acquire);
    }
#endif
    result.operation_ = operation;
    try { ShaderOperation::accept(operation); }
    catch (...) { result.operation_.reset(); throw; }
}
void Shader::operator()(const Slice& slice, ShaderResult& result, void (*done)(void*),
    void* context, uint32_t workgroups, std::span<const Slice> dependencies) const {
    (*this)(&slice, 1, result, done, context, workgroups, dependencies);
}
ShaderAwaiter Shader::dispatch(const Slice* slices, size_t count, uint32_t workgroups,
    std::span<const Slice> dependencies) const {
    ShaderResult result;
    (*this)(slices, count, result, nullptr, nullptr, workgroups, dependencies);
    return ShaderAwaiter(std::move(result));
}
void GPU::run(const Shader& program, const Slice* streams, size_t count, ShaderResult& result,
    void (*done)(void*), void* context, std::span<const Slice> dependencies) {
    program(streams, count, result, done, context, 1, dependencies);
}
ShaderAwaiter GPU::dispatch(const Shader& program, const Slice* streams, size_t count,
    std::span<const Slice> dependencies) {
    return program.dispatch(streams, count, 1, dependencies);
}
namespace {
/** --------------------------------------------------------------------------------------------------------- Encoded Program
 * @brief Defines a 64-byte little-endian BAGP header followed by eight-byte-aligned source fields.
 */
struct EncodedProgram {
    static constexpr uint64_t signature = 0x0000000150474142ull;
    static constexpr size_t header_bytes = 64;
    bool jobs = false;
    ShaderSource source;
    std::string_view name;
    /** ------------------------------------------------------------------------------------------- Read
     * @brief Reads an unaligned little-endian integer from the portable header.
     */
    static uint64_t read(const uint8_t* bytes) {
        return uint64_t(bytes[0]) | uint64_t(bytes[1]) << 8 | uint64_t(bytes[2]) << 16
            | uint64_t(bytes[3]) << 24 | uint64_t(bytes[4]) << 32 | uint64_t(bytes[5]) << 40
            | uint64_t(bytes[6]) << 48 | uint64_t(bytes[7]) << 56;
    }
    /** ------------------------------------------------------------------------------------------- Write
     * @brief Writes one fixed-width little-endian header field.
     */
    static void write(uint8_t* bytes, uint64_t value) {
        bytes[0] = uint8_t(value); bytes[1] = uint8_t(value >> 8);
        bytes[2] = uint8_t(value >> 16); bytes[3] = uint8_t(value >> 24);
        bytes[4] = uint8_t(value >> 32); bytes[5] = uint8_t(value >> 40);
        bytes[6] = uint8_t(value >> 48); bytes[7] = uint8_t(value >> 56);
    }
    /** ------------------------------------------------------------------------------------------- Identity
     * @brief Hashes canonical serialized bytes with SHA-256 while excluding the identity field.
     */
    static uint64_t identity(const uint8_t* bytes, size_t count) {
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (!context) ALLIGATOR_GPU_THROW("Could not allocate program identity state");
        std::array<uint8_t, 32> digest{};
        unsigned int length = 0;
        const bool valid = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1
            && EVP_DigestUpdate(context, bytes, 16) == 1
            && EVP_DigestUpdate(context, bytes + 24, count - 24) == 1
            && EVP_DigestFinal_ex(context, digest.data(), &length) == 1;
        EVP_MD_CTX_free(context);
        if (!valid || length != digest.size()) ALLIGATOR_GPU_THROW("Could not hash the encoded program");
        return read(digest.data());
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks canonical lengths, flags, padding, and source identity before compilation.
     */
    explicit EncodedProgram(const Slice& program) {
        if (program.size_bytes() < header_bytes) ALLIGATOR_GPU_THROW("Encoded program header is truncated");
        const uint8_t* bytes = program.data<uint8_t>();
        if (read(bytes) != signature) ALLIGATOR_GPU_THROW("Unsupported BAGP magic or version");
        const uint64_t flags = read(bytes + 8);
        if ((flags >> 32) != header_bytes || uint32_t(flags) > 1)
            ALLIGATOR_GPU_THROW("Unsupported BAGP header or entry flags");
        jobs = uint32_t(flags) == 1;
        const uint64_t total = read(bytes + 56);
        if (total != program.size_bytes() || (total & 63) != 0)
            ALLIGATOR_GPU_THROW("Encoded program length does not match its Slice");
        if (read(bytes + 16) != identity(bytes, size_t(total)))
            ALLIGATOR_GPU_THROW("Encoded program content identity does not match");
        std::array<std::string_view, 4> fields;
        size_t position = header_bytes;
        for (size_t index = 0; index < fields.size(); ++index) {
            const uint64_t length = read(bytes + 24 + index * 8);
            if (length > total - position || length > SIZE_MAX - 7)
                ALLIGATOR_GPU_THROW("Encoded program source length exceeds its payload");
            fields[index] = std::string_view(reinterpret_cast<const char*>(bytes + position), size_t(length));
            if (fields[index].find('\0') != std::string_view::npos)
                ALLIGATOR_GPU_THROW("Encoded program fields cannot contain null bytes");
            const size_t padded = (size_t(length) + 7) & ~size_t{7};
            if (padded > total - position) ALLIGATOR_GPU_THROW("Encoded program padding exceeds its payload");
            position += padded;
        }
        if (((position + 63) & ~size_t{63}) != total)
            ALLIGATOR_GPU_THROW("Encoded program has trailing payload records");
        source = ShaderSource{fields[0], fields[1], fields[2]};
        name = fields[3];
        if (source.glsl.empty() && source.metal.empty() && source.cuda.empty())
            ALLIGATOR_GPU_THROW("Encoded program has no source");
    }
    /** ------------------------------------------------------------------------------------------- Cached
     * @brief Reuses a content-equal prepared program while compilation failures remain retryable.
     */
    static std::shared_ptr<ShaderState> cached(const Slice& program) {
        ShaderPreparation preparation;
        const EncodedProgram decoded(program);
        std::lock_guard lock(shader_runtime().cache_mutex);
        for (const auto& entry : shader_runtime().cache) {
            if (entry.bytes.size() == program.size_bytes()
                && std::memcmp(entry.bytes.data(), program.raw(), entry.bytes.size()) == 0)
                return entry.state;
        }
        auto prepared = std::make_shared<ShaderState>(decoded.source, decoded.name, decoded.jobs);
        const uint8_t* bytes = program.data<uint8_t>();
        ShaderRuntime::CacheEntry entry{std::vector<uint8_t>(bytes, bytes + program.size_bytes()), prepared};
        constexpr size_t cache_capacity = 64;
        if (shader_runtime().cache.size() == cache_capacity) shader_runtime().cache.erase(shader_runtime().cache.begin());
        shader_runtime().cache.push_back(std::move(entry));
        return prepared;
    }
};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Encode
 * @brief Serializes source fields with explicit widths and a content identity independent of device code.
 */
Slice GPU::encode(const Shader& program) {
    if (!program.state_) ALLIGATOR_GPU_THROW("Cannot encode a moved-from Shader");
    const auto& state = *program.state_->impl_;
    const std::array<std::string_view, 4> fields{
        state.sources[0], state.sources[1], state.sources[2], state.name};
    size_t total = EncodedProgram::header_bytes;
    for (const std::string_view field : fields) {
        if (field.size() > SIZE_MAX - total - 63) ALLIGATOR_GPU_THROW("Encoded program exceeds addressable memory");
        total += (field.size() + 7) & ~size_t{7};
    }
    total = (total + 63) & ~size_t{63};
    Slice encoded(total, BuffetDescriptors::get(0));
    uint8_t* bytes = encoded.data<uint8_t>();
    std::memset(bytes, 0, total);
    EncodedProgram::write(bytes, EncodedProgram::signature);
    EncodedProgram::write(bytes + 8, uint64_t{EncodedProgram::header_bytes} << 32
        | uint64_t(state.format == ShaderState::Impl::Format::Jobs));
    EncodedProgram::write(bytes + 56, total);
    size_t position = EncodedProgram::header_bytes;
    for (size_t index = 0; index < fields.size(); ++index) {
        EncodedProgram::write(bytes + 24 + index * 8, fields[index].size());
        if (!fields[index].empty()) std::memcpy(bytes + position, fields[index].data(), fields[index].size());
        position += (fields[index].size() + 7) & ~size_t{7};
    }
    EncodedProgram::write(bytes + 16, EncodedProgram::identity(bytes, total));
    return encoded;
}
/** --------------------------------------------------------------------------------------------------------- Decode
 * @brief Prepares or reuses the complete source identity for the active backend and compiler configuration.
 */
Shader GPU::decode(const Slice& program) {
    return Shader(std::make_unique<ShaderState>(*EncodedProgram::cached(program)));
}
/** --------------------------------------------------------------------------------------------------------- Compile GLSL
 * @brief Validates the job-table body at preparation and serializes its portable source.
 */
Slice GPU::compile_glsl(std::string_view body) {
    Shader program(std::make_unique<ShaderState>(ShaderSource{body, {}, {}}, "alligator_jobs", true));
    return encode(program);
}
/** --------------------------------------------------------------------------------------------------------- Run Encoded
 * @brief Dispatches a cached source program with the same owned completion contract as Shader.
 */
void GPU::run(const Slice& program, const Slice* streams, size_t count, ShaderResult& result,
    void (*done)(void*), void* context, std::span<const Slice> dependencies) {
    Shader prepared = decode(program);
    prepared(streams, count, result, done, context, 1, dependencies);
}
/** --------------------------------------------------------------------------------------------------------- Dispatch Encoded
 * @brief Returns an owned awaiter independent of the encoded program's lifetime.
 */
ShaderAwaiter GPU::dispatch(const Slice& program, const Slice* streams, size_t count,
    std::span<const Slice> dependencies) {
    Shader prepared = decode(program);
    return prepared.dispatch(streams, count, 1, dependencies);
}
} // namespace buffetalligator
