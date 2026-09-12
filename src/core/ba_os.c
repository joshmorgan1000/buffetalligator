/** --------------------------------------------------------------------------------------------------------- OS Layer
 * @file ba_os.c
 * @brief Implements operating-system memory and synchronization services.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "ba_os.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <memoryapi.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#endif
#endif

/** --------------------------------------------------------------------------------------------------------- Event
 * @brief Stores the platform's wake predicate and condition variable.
 */
struct ba_event {
#if defined(_WIN32)
    SRWLOCK mutex; ///< Predicate lock.
    CONDITION_VARIABLE condition; ///< Wake condition.
#else
    pthread_mutex_t mutex; ///< Predicate lock.
    pthread_cond_t condition; ///< Wake condition.
#endif
    int signaled; ///< Pending signal predicate.
};
/** --------------------------------------------------------------------------------------------------------- Mutex
 * @brief Stores the native exclusive lock for cold-path mutations.
 */
struct ba_mutex {
#if defined(_WIN32)
    SRWLOCK native; ///< Windows blocking lock.
#else
    pthread_mutex_t native; ///< POSIX blocking lock.
#endif
};
/** --------------------------------------------------------------------------------------------------------- Thread Start
 * @brief Carries a detached thread's entry point and argument.
 */
typedef struct ba_thread_start {
    ba_thread_fn function; ///< Thread entry point.
    void* argument; ///< Entry argument.
} ba_thread_start_t;
#if defined(_WIN32)
static DWORD g_tls_key;
static INIT_ONCE g_pressure_once = INIT_ONCE_STATIC_INIT;
static HANDLE g_pressure_notification;
/** --------------------------------------------------------------------------------------------------------- Pressure Init
 * @brief Creates the process memory pressure notification.
 */
static BOOL CALLBACK ba_pressure_init(PINIT_ONCE once, PVOID argument, PVOID* context) {
    (void)once; (void)argument; (void)context;
    g_pressure_notification = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    return g_pressure_notification != NULL;
}
#else
static pthread_key_t g_tls_key;
#if defined(__APPLE__)
static ba_tls_dtor g_tls_destructor;
static _Thread_local int g_tls_registered;
static _Thread_local void* g_tls_value;
/** --------------------------------------------------------------------------------------------------------- Native TLS Exit
 * @brief Runs arena cleanup before Darwin releases its compiler TLS storage.
 */
static void ba_native_tls_exit(void* ignored) {
    (void)ignored;
    if (g_tls_value) g_tls_destructor(g_tls_value);
}
static pthread_once_t g_pressure_once = PTHREAD_ONCE_INIT;
static dispatch_source_t g_pressure_source;
static _Atomic int g_pressure_level;
/** --------------------------------------------------------------------------------------------------------- Pressure Update
 * @brief Publishes the latest dispatch memory pressure level.
 */
static void ba_pressure_update(void* context) {
    (void)context;
    const unsigned long flags = dispatch_source_get_data(g_pressure_source);
    const int level = (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) ? BA_PRESSURE_CRITICAL :
        (flags & DISPATCH_MEMORYPRESSURE_WARN) ? BA_PRESSURE_WARN : BA_PRESSURE_NONE;
    atomic_store_explicit(&g_pressure_level, level, memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Pressure Init
 * @brief Starts the private dispatch memory pressure source.
 */
static void ba_pressure_init(void) {
    dispatch_queue_t queue = dispatch_queue_create("buffetalligator.pressure", DISPATCH_QUEUE_SERIAL);
    g_pressure_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, queue);
    if (!g_pressure_source) abort();
    dispatch_source_set_event_handler_f(g_pressure_source, ba_pressure_update);
    dispatch_resume(g_pressure_source);
    dispatch_release(queue);
}
#endif
/** --------------------------------------------------------------------------------------------------------- Rlimit
 * @brief Includes a present address-space limit in the effective ceiling.
 */
static void ba_limit_resource(uint64_t* limit, int resource) {
    struct rlimit value;
    if (getrlimit(resource, &value) == 0 && value.rlim_cur != RLIM_INFINITY && value.rlim_cur < *limit) {
        *limit = value.rlim_cur;
    }
}
#endif
#if defined(__linux__)
/** --------------------------------------------------------------------------------------------------------- Read Limit
 * @brief Includes a numeric cgroup limit when its controller file exists.
 */
static void ba_limit_file(uint64_t* limit, const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) return;
    unsigned long long value;
    if (fscanf(file, "%llu", &value) == 1 && value < *limit) *limit = value;
    fclose(file);
}
/** --------------------------------------------------------------------------------------------------------- Cgroup Limits
 * @brief Includes limits on the process cgroup and every ancestor controller.
 */
static void ba_cgroup_limits(uint64_t* limit) {
    FILE* file = fopen("/proc/self/cgroup", "r");
    if (!file) return;
    char line[8192];
    while (fgets(line, sizeof(line), file)) {
        char* first = strchr(line, ':');
        char* second = first ? strchr(first + 1, ':') : NULL;
        if (!second) continue;
        *second = '\0';
        const int unified = first[1] == '\0';
        if (!unified && !strstr(first + 1, "memory")) continue;
        char* relative = second + 1;
        relative[strcspn(relative, "\n")] = '\0';
        const char* root = unified ? "/sys/fs/cgroup" : "/sys/fs/cgroup/memory";
        const char* leaf = unified ? "memory.max" : "memory.limit_in_bytes";
        char path[16384];
        for (;;) {
            snprintf(path, sizeof(path), "%s%s/%s", root, relative, leaf);
            ba_limit_file(limit, path);
            char* slash = strrchr(relative, '/');
            if (!slash) break;
            *slash = '\0';
        }
    }
    fclose(file);
}
#endif
/** --------------------------------------------------------------------------------------------------------- Probe
 * @brief Reads fresh system capacity and process constraints.
 */
void ba_os_probe(ba_sysinfo_t* out) {
    *out = (ba_sysinfo_t){0};
#if defined(_WIN32)
    SYSTEM_INFO system;
    MEMORYSTATUSEX memory = { .dwLength = sizeof(memory) };
    PROCESS_MEMORY_COUNTERS process = { .cb = sizeof(process) };
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job;
    GetSystemInfo(&system);
    if (!GlobalMemoryStatusEx(&memory) || !GetProcessMemoryInfo(GetCurrentProcess(), &process, sizeof(process))) abort();
    out->page = system.dwPageSize;
    DWORD topology_bytes = 0;
    GetLogicalProcessorInformation(NULL, &topology_bytes);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* topology = malloc(topology_bytes);
    if (!topology || !GetLogicalProcessorInformation(topology, &topology_bytes)) abort();
    for (DWORD index = 0; index < topology_bytes / sizeof(*topology); ++index) {
        if (topology[index].Relationship == RelationCache && topology[index].Cache.Level == 1 &&
            topology[index].Cache.LineSize > out->cache_line) out->cache_line = topology[index].Cache.LineSize;
    }
    free(topology);
    out->hw_threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    out->physical = memory.ullTotalPhys;
    out->available = memory.ullAvailPhys;
    out->rss = process.WorkingSetSize;
    out->limit = out->physical;
    if (QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &job, sizeof(job), NULL) &&
        (job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) &&
        job.ProcessMemoryLimit < out->limit) out->limit = job.ProcessMemoryLimit;
    const SIZE_T large = GetLargePageMinimum();
    if (large) {
        void* probe = VirtualAlloc(NULL, large, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (probe) { out->large_page = large; VirtualFree(probe, 0, MEM_RELEASE); }
    }
#else
    out->page = (uint64_t)sysconf(_SC_PAGESIZE);
    out->hw_threads = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__APPLE__)
    uint64_t cache_line;
    size_t length = sizeof(cache_line);
    if (sysctlbyname("hw.cachelinesize", &cache_line, &length, NULL, 0) != 0) abort();
    out->cache_line = (uint32_t)cache_line;
    length = sizeof(out->physical);
    if (sysctlbyname("hw.memsize", &out->physical, &length, NULL, 0) != 0) abort();
    vm_statistics64_data_t memory;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const mach_port_t host = mach_host_self();
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&memory, &count) != KERN_SUCCESS) abort();
    mach_port_deallocate(mach_task_self(), host);
    out->available = ((uint64_t)memory.free_count + memory.inactive_count + memory.speculative_count) * out->page;
    task_vm_info_data_t process;
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&process, &count) != KERN_SUCCESS) abort();
    out->rss = process.phys_footprint;
#elif defined(__linux__)
    out->cache_line = (uint32_t)sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    cpu_set_t affinity;
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) abort();
    out->hw_threads = (uint32_t)CPU_COUNT(&affinity);
    out->physical = (uint64_t)sysconf(_SC_PHYS_PAGES) * out->page;
    FILE* file = fopen("/proc/meminfo", "r");
    if (!file) abort();
    char line[1024];
    unsigned long long value;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) { out->available = value * 1024; break; }
    }
    fclose(file);
    file = fopen("/proc/self/statm", "r");
    unsigned long long virtual_pages, resident_pages;
    if (!file || fscanf(file, "%llu %llu", &virtual_pages, &resident_pages) != 2) abort();
    fclose(file);
    out->rss = resident_pages * out->page;
    file = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    int enabled = 0;
    if (file) {
        if (fgets(line, sizeof(line), file)) enabled = strstr(line, "[always]") || strstr(line, "[madvise]");
        fclose(file);
    }
    if (enabled) {
        file = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
        if (file) { if (fscanf(file, "%llu", &value) == 1) out->large_page = value; fclose(file); }
    }
#endif
    out->limit = out->physical;
    ba_limit_resource(&out->limit, RLIMIT_AS);
    ba_limit_resource(&out->limit, RLIMIT_DATA);
#if defined(__linux__)
    ba_cgroup_limits(&out->limit);
#endif
#endif
    out->granule = out->large_page ? out->large_page : out->page;
}
/** --------------------------------------------------------------------------------------------------------- Map
 * @brief Allocates aligned anonymous pages without touching their contents.
 */
void* ba_os_map(uint64_t bytes, uint64_t alignment, int want_large_pages) {
    if (!bytes || !alignment || (alignment & (alignment - 1)) || bytes > SIZE_MAX - alignment) return NULL;
#if defined(_WIN32)
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const DWORD flags = MEM_RESERVE | MEM_COMMIT | (want_large_pages ? MEM_LARGE_PAGES : 0);
    MEM_ADDRESS_REQUIREMENTS requirements = {0};
    requirements.Alignment = alignment > system.dwAllocationGranularity ? alignment : system.dwAllocationGranularity;
    MEM_EXTENDED_PARAMETER parameter = {0};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(NULL, NULL, (SIZE_T)bytes, flags, PAGE_READWRITE, &parameter, 1);
#else
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    if (alignment < page) alignment = page;
    if (bytes % page) return NULL;
    void* mapping = mmap(NULL, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return NULL;
    const uintptr_t aligned = ((uintptr_t)mapping + alignment - 1) & ~(uintptr_t)(alignment - 1);
    const size_t head = aligned - (uintptr_t)mapping;
    const size_t tail = alignment - head;
    if (head) munmap(mapping, head);
    if (tail) munmap((void*)(aligned + bytes), tail);
#if defined(__linux__)
    if (want_large_pages && madvise((void*)aligned, bytes, MADV_HUGEPAGE) != 0) {
        munmap((void*)aligned, bytes);
        return NULL;
    }
#else
    (void)want_large_pages;
#endif
    return (void*)aligned;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Reserve
 * @brief Reserves an inaccessible address range for later page commitment.
 */
void* ba_os_reserve(uint64_t bytes) {
    if (!bytes || bytes > SIZE_MAX) return NULL;
#if defined(_WIN32)
    return VirtualAlloc(NULL, (SIZE_T)bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* mapping = mmap(NULL, (size_t)bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mapping == MAP_FAILED ? NULL : mapping;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Commit
 * @brief Commits writable pages within an existing reservation.
 */
int ba_os_commit(void* base, uint64_t bytes) {
    if (!base || !bytes || bytes > SIZE_MAX) return -1;
#if defined(_WIN32)
    return VirtualAlloc(base, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) == base ? 0 : -1;
#else
    return mprotect(base, (size_t)bytes, PROT_READ | PROT_WRITE);
#endif
}
/** --------------------------------------------------------------------------------------------------------- Unmap
 * @brief Releases an owned page mapping.
 */
void ba_os_unmap(void* base, uint64_t bytes) {
#if defined(_WIN32)
    (void)bytes;
    if (!VirtualFree(base, 0, MEM_RELEASE)) abort();
#else
    if (munmap(base, bytes) != 0) abort();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Rezero
 * @brief Replaces an owned range with fresh anonymous pages.
 */
int ba_os_rezero(void* base, uint64_t bytes) {
    if (!bytes) return 0;
#if defined(_WIN32)
    if (!VirtualFree(base, (SIZE_T)bytes, MEM_DECOMMIT)) return -1;
    return VirtualAlloc(base, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) == base ? 0 : -1;
#else
    return mmap(base, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base ? 0 : -1;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Pressure
 * @brief Samples the native memory pressure source.
 */
ba_pressure_t ba_os_pressure(void) {
#if defined(__APPLE__)
    pthread_once(&g_pressure_once, ba_pressure_init);
    return (ba_pressure_t)atomic_load_explicit(&g_pressure_level, memory_order_relaxed);
#elif defined(__linux__)
    FILE* file = fopen("/proc/pressure/memory", "r");
    if (!file) return BA_PRESSURE_NONE;
    char line[1024];
    double some = 0, full = 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "some avg10=%lf", &some) == 1) continue;
        (void)sscanf(line, "full avg10=%lf", &full);
    }
    fclose(file);
    return full > 5 ? BA_PRESSURE_CRITICAL : some > 10 ? BA_PRESSURE_WARN : BA_PRESSURE_NONE;
#else
    BOOL low = FALSE;
    if (!InitOnceExecuteOnce(&g_pressure_once, ba_pressure_init, NULL, NULL) ||
        !QueryMemoryResourceNotification(g_pressure_notification, &low)) abort();
    return low ? BA_PRESSURE_WARN : BA_PRESSURE_NONE;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Time
 * @brief Reads the monotonic platform clock in nanoseconds.
 */
uint64_t ba_os_now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000000ull +
        (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000000ull / frequency.QuadPart;
#else
    struct timespec value;
#if defined(__APPLE__)
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
#else
    clock_gettime(CLOCK_MONOTONIC, &value);
#endif
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Thread Entry
 * @brief Invokes a detached entry point after releasing its launch record.
 */
#if defined(_WIN32)
static DWORD WINAPI ba_thread_entry(void* argument) {
#else
static void* ba_thread_entry(void* argument) {
#endif
    const ba_thread_start_t start = *(ba_thread_start_t*)argument;
    free(argument);
    start.function(start.argument);
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Start Thread
 * @brief Launches a detached native thread.
 */
int ba_os_thread_start(ba_thread_fn function, void* argument) {
    ba_thread_start_t* start = malloc(sizeof(*start));
    if (!start) return -1;
    *start = (ba_thread_start_t){function, argument};
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, ba_thread_entry, start, 0, NULL);
    if (!thread) { free(start); return -1; }
    CloseHandle(thread);
    return 0;
#else
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) { free(start); return -1; }
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int result = pthread_create(&thread, &attributes, ba_thread_entry, start);
    pthread_attr_destroy(&attributes);
    if (result) free(start);
    return result;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Create Mutex
 * @brief Allocates a native blocking mutex.
 */
ba_mutex_t* ba_mutex_create(void) {
    ba_mutex_t* mutex = malloc(sizeof(*mutex));
    if (!mutex) return NULL;
#if defined(_WIN32)
    InitializeSRWLock(&mutex->native);
#else
    if (pthread_mutex_init(&mutex->native, NULL) != 0) { free(mutex); return NULL; }
#endif
    return mutex;
}
/** --------------------------------------------------------------------------------------------------------- Destroy Mutex
 * @brief Releases an unused native mutex.
 */
void ba_mutex_destroy(ba_mutex_t* mutex) {
#if !defined(_WIN32)
    if (pthread_mutex_destroy(&mutex->native) != 0) abort();
#endif
    free(mutex);
}
/** --------------------------------------------------------------------------------------------------------- Lock Mutex
 * @brief Acquires the native mutex and parks while another owner holds it.
 */
void ba_mutex_lock(ba_mutex_t* mutex) {
#if defined(_WIN32)
    AcquireSRWLockExclusive(&mutex->native);
#else
    if (pthread_mutex_lock(&mutex->native) != 0) abort();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Unlock Mutex
 * @brief Releases the native mutex after publishing protected state.
 */
void ba_mutex_unlock(ba_mutex_t* mutex) {
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&mutex->native);
#else
    if (pthread_mutex_unlock(&mutex->native) != 0) abort();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Create Event
 * @brief Allocates opaque event storage.
 */
ba_event_t* ba_event_create(void) {
    ba_event_t* event = malloc(sizeof(*event));
    if (event) ba_event_init(event);
    return event;
}
/** --------------------------------------------------------------------------------------------------------- Initialize Event
 * @brief Initializes an auto-reset event predicate and synchronization primitives.
 */
void ba_event_init(ba_event_t* event) {
    event->signaled = 0;
#if defined(_WIN32)
    InitializeSRWLock(&event->mutex); InitializeConditionVariable(&event->condition);
#else
    if (pthread_mutex_init(&event->mutex, NULL) != 0 || pthread_cond_init(&event->condition, NULL) != 0) abort();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Destroy Event
 * @brief Releases event storage after all waiters have completed.
 */
void ba_event_destroy(ba_event_t* event) {
#if !defined(_WIN32)
    pthread_cond_destroy(&event->condition);
    pthread_mutex_destroy(&event->mutex);
#endif
    free(event);
}
/** --------------------------------------------------------------------------------------------------------- Wait Event
 * @brief Waits for a persistent signal with a bounded timeout.
 */
void ba_event_wait(ba_event_t* event, uint64_t timeout_ns) {
#if defined(_WIN32)
    const ULONGLONG deadline = GetTickCount64() + (timeout_ns + 999999) / 1000000;
    AcquireSRWLockExclusive(&event->mutex);
    while (!event->signaled) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD remaining = (DWORD)((deadline - now) < INFINITE ? (deadline - now) : INFINITE - 1);
        if (!SleepConditionVariableSRW(&event->condition, &event->mutex, remaining, 0) && GetLastError() != ERROR_TIMEOUT) abort();
    }
    event->signaled = 0;
    ReleaseSRWLockExclusive(&event->mutex);
#else
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    const uint64_t nanos = (uint64_t)deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t)(nanos / 1000000000ull);
    deadline.tv_nsec = (long)(nanos % 1000000000ull);
    pthread_mutex_lock(&event->mutex);
    while (!event->signaled) {
        const int result = pthread_cond_timedwait(&event->condition, &event->mutex, &deadline);
        if (result == ETIMEDOUT) break;
        if (result) abort();
    }
    event->signaled = 0;
    pthread_mutex_unlock(&event->mutex);
#endif
}
/** --------------------------------------------------------------------------------------------------------- Signal Event
 * @brief Publishes a wake signal while holding the predicate lock.
 */
void ba_event_signal(ba_event_t* event) {
#if defined(_WIN32)
    AcquireSRWLockExclusive(&event->mutex);
    event->signaled = 1;
    WakeConditionVariable(&event->condition);
    ReleaseSRWLockExclusive(&event->mutex);
#else
    pthread_mutex_lock(&event->mutex);
    event->signaled = 1;
    pthread_cond_signal(&event->condition);
    pthread_mutex_unlock(&event->mutex);
#endif
}
/** --------------------------------------------------------------------------------------------------------- TLS Key
 * @brief Allocates the native thread-exit key.
 */
int ba_os_tls_key(ba_tls_dtor destructor) {
#if defined(_WIN32)
    g_tls_key = FlsAlloc((PFLS_CALLBACK_FUNCTION)destructor);
    return g_tls_key == FLS_OUT_OF_INDEXES ? -1 : 0;
#else
#if defined(__APPLE__)
    g_tls_destructor = destructor;
#endif
    return pthread_key_create(&g_tls_key, destructor);
#endif
}
/** --------------------------------------------------------------------------------------------------------- TLS Value
 * @brief Attaches cleanup state to the calling thread.
 */
void ba_os_tls_set(void* value) {
#if defined(_WIN32)
    if (!FlsSetValue(g_tls_key, value)) abort();
#elif defined(__APPLE__)
    g_tls_value = value;
    if (!g_tls_registered) {
        g_tls_registered = 1;
        _tlv_atexit(ba_native_tls_exit, NULL);
    }
#else
    if (pthread_setspecific(g_tls_key, value) != 0) abort();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Yield
 * @brief Yields execution to another runnable thread.
 */
void ba_os_yield(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}
