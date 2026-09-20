#ifndef LOGGING_LOGGER_LOADED
#define LOGGING_LOGGER_LOADED
#pragma once
/** --------------------------------------------------------------------------------------------------------- Logging
 * @file logging.hpp
 * @brief Header for logging utilities and global logging context.
 */
#ifndef LOGGING_LOG_LEVEL
#define LOGGING_LOG_LEVEL 0
#endif
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <sstream>
#include <atomic>
#include <ostream>
#include <type_traits>
#include <stdexcept>
#include <utility>

namespace threadsafe_logger {
inline static std::string json_escape(const std::string& input) {
    std::string output;
    for (char c : input) {
        switch (c) {
            case '\"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                    output += buffer;
                } else {
                    output += c;
                }
        }
    }
    return output;
}
class InternalLogger;
template <typename T, typename = void> struct is_streamable : std::false_type {};
template <typename T> struct is_streamable<T, std::void_t<
    decltype(std::declval<std::ostream&>()<<std::declval<const T&>())>> : std::true_type {};
template <typename T> inline constexpr bool is_streamable_v = is_streamable<T>::value;
template <typename T, typename = void> struct has_to_string : std::false_type {};
template <typename T> struct has_to_string<T, std::void_t<
    decltype(std::declval<T>().toString())>>: std::true_type {};
template <typename T> inline constexpr bool has_to_string_v = has_to_string<T>::value;
template <typename T> inline constexpr bool is_essentially_streamable_v =
    is_streamable_v<T> || has_to_string_v<T>;
template <typename T, typename... Args> inline constexpr bool is_variadic_log_input_v =
    is_essentially_streamable_v<T> && (sizeof...(Args) > 0 ||
    (!std::is_same_v<std::decay_t<T>, const char*> && !std::is_same_v<std::decay_t<T>, std::string>));
class Exception;
class Logger {
public:
    class Impl;
    enum class LogLevel : uint8_t {TRACE=0,DEBUG=1,INFO=2,WARN=3,ERROR=4,RAW=5};
    static void log_message(LogLevel level, const std::string& message);
    class StreamLogger {
    private:
        LogLevel level_;
        std::stringstream stream_;
    public:
        explicit StreamLogger(LogLevel level) : level_(level) {}
        template <typename T> StreamLogger &operator<<(T &&value) {
            stream_ << std::forward<T>(value); return *this; }
        ~StreamLogger() { Logger::log_message(level_, stream_.str()); }
        StreamLogger& operator=(const StreamLogger&) = delete;
        StreamLogger(const StreamLogger&) = delete;
        StreamLogger(StreamLogger&&) = default;
        StreamLogger& operator=(StreamLogger&&) = default;
    };
    class NullStream {
    public:
        template <typename T> NullStream &operator<<(T &&) { return *this; }
        NullStream& operator=(const NullStream&) = delete;
        NullStream(const NullStream&) = delete;
        NullStream(NullStream&&) = default;
        NullStream& operator=(NullStream&&) = default;
        NullStream() = default;
    };
    static void install_uncaught_logger();
    static StreamLogger stream_log_info() { return StreamLogger(LogLevel::INFO); }
    static StreamLogger stream_log_warn() { return StreamLogger(LogLevel::WARN); }
    static StreamLogger stream_log_error() { return StreamLogger(LogLevel::ERROR); }
    static StreamLogger stream_log_debug() { return StreamLogger(LogLevel::DEBUG); }
    static StreamLogger stream_log_trace() { return StreamLogger(LogLevel::TRACE); }
    static StreamLogger stream_log_default() { return StreamLogger(level()); }
    static StreamLogger stream_log_raw() { return StreamLogger(LogLevel::RAW); }
    static NullStream null_stream() { return NullStream(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;
    ~Logger() = default;
    static Logger& instance();
    static LogLevel& level();
    static std::string& instance_name();
    static std::string& component();
    static std::string& thread_context();
    static std::atomic<bool>& log_format_json();
    static std::atomic<std::ostream*>& stdout_stream();
    static std::atomic<std::ostream*>& additional_stream();
    static std::atomic<std::ostream*>& stderr_stream();
    static std::string get_iso8601_timestamp(uint64_t now = 0);
    static std::string format_duration(uint64_t duration_ms);
    static bool stdout_is_tty();
    /** ------------------------------------------------------------------------------------------- Progress
     * @brief Starts one terminal progress animation and returns its update callback.
     * @param header Label displayed beside the animated progress bar.
     * @return Callback accepting progress from zero through one.
     */
    static std::function<void(float)> progress(const std::string& header);
    template <typename T> Logger &operator<<(T &&value) {
        stream_ << std::forward<T>(value); return *this;
    }
    template <typename T>
    inline static void concat_multi_parameter_inputs(std::stringstream &currentstream, T first) {
        if constexpr (is_streamable_v<T>) currentstream << first;
        else if constexpr (has_to_string_v<T>) currentstream << first.toString();
    }
    template <typename T, typename... Args>
    inline static void concat_multi_parameter_inputs(std::stringstream &currentstream, T first, Args... args) {
        concat_multi_parameter_inputs(currentstream, first);
        concat_multi_parameter_inputs(currentstream, args...);
    }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_message(LogLevel level_param, T first, Args... args) {
        if (static_cast<size_t>(level()) > static_cast<size_t>(level_param)) return;
        std::stringstream msg_stream;
        concat_multi_parameter_inputs(msg_stream, first, args...);
        log_message(level_param, msg_stream.str());
    }
    static void log_info(const std::string& message) { Logger::log_message(LogLevel::INFO, message); }
    static void log_warn(const std::string& message) { Logger::log_message(LogLevel::WARN, message); }
    static void log_error(const std::string& message) { Logger::log_message(LogLevel::ERROR, message); }
    static void log_debug(const std::string& message) { Logger::log_message(LogLevel::DEBUG, message); }
    static void log_trace(const std::string& message) { Logger::log_message(LogLevel::TRACE, message); }
    static void log_raw(const std::string& message) { Logger::log_message(LogLevel::RAW, message); }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_info(T first, Args... args) { Logger::log_message(LogLevel::INFO, first, args...); }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_warn(T first, Args... args) { Logger::log_message(LogLevel::WARN, first, args...); }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_error(T first, Args... args) { Logger::log_message(LogLevel::ERROR, first, args...); }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_debug(T first, Args... args) { Logger::log_message(LogLevel::DEBUG, first, args...); }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_trace(T first, Args... args) { Logger::log_message(LogLevel::TRACE, first, args...); }
    template <typename T, typename... Args, typename = std::enable_if_t<is_variadic_log_input_v<T, Args...>>>
    static void log_raw(T first, Args... args) { Logger::log_message(LogLevel::RAW, first, args...); }
    static uint64_t get_now();
private:
    Impl *impl_;
    LogLevel level_;
    std::stringstream stream_;
    friend class InternalLogger;
    Logger();
};
inline static std::atomic<std::ostream*>& stdout_stream() { return Logger::stdout_stream(); }
inline static std::atomic<std::ostream*>& additional_stream() { return Logger::additional_stream(); }
inline static std::atomic<std::ostream*>& stderr_stream() { return Logger::stderr_stream(); }
inline static std::atomic<bool>& log_format_json() { return Logger::log_format_json(); }
inline static std::string& instance_name() { return Logger::instance_name(); }
inline static std::string& component() { return Logger::component(); }
inline static std::string& thread_context() { return Logger::thread_context(); }
inline static void log_message(Logger::LogLevel level, const std::string& message) { Logger::log_message(level, message); }
inline static std::string get_iso8601_timestamp(uint64_t now = 0) { return Logger::get_iso8601_timestamp(now); }
inline static constexpr Logger::LogLevel ERROR = Logger::LogLevel::ERROR;
inline static constexpr Logger::LogLevel WARN = Logger::LogLevel::WARN;
inline static constexpr Logger::LogLevel INFO = Logger::LogLevel::INFO;
inline static constexpr Logger::LogLevel DEBUG = Logger::LogLevel::DEBUG;
inline static constexpr Logger::LogLevel TRACE = Logger::LogLevel::TRACE;
inline static bool stdout_is_tty() { return Logger::stdout_is_tty(); }
template<typename T>
struct always_false : std::false_type {};
class NullStream {
public:
    template<typename T>
    constexpr NullStream& operator<<(const T&) noexcept { return *this; }
};
inline constexpr NullStream null_stream() noexcept { return NullStream{}; }
#if LOGGING_LOG_LEVEL <= 0
    #define LOG_TRACE(...) ::threadsafe_logger::Logger::log_trace(__VA_ARGS__)
#else
    #define LOG_TRACE(...) ((void)0)
#endif
#if LOGGING_LOG_LEVEL <= 1
    #define LOG_DEBUG(...) ::threadsafe_logger::Logger::log_debug(__VA_ARGS__)
#else
    #define LOG_DEBUG(...) ((void)0)
#endif
#if LOGGING_LOG_LEVEL <= 2
    #define LOG_INFO(...) ::threadsafe_logger::Logger::log_info(__VA_ARGS__)
#else
    #define LOG_INFO(...) ((void)0)
#endif
#if LOGGING_LOG_LEVEL <= 3
    #define LOG_WARN(...) ::threadsafe_logger::Logger::log_warn(__VA_ARGS__)
#else
    #define LOG_WARN(...) ((void)0)
    #define LOGWARN(...) ((void)0)
#endif
#if LOGGING_LOG_LEVEL <= 4
    #define LOG_ERROR(...) ::threadsafe_logger::Logger::log_error(__VA_ARGS__)
#else
    #define LOG_ERROR(...) ((void)0)
    #define LOGERROR(...) ((void)0)
#endif
#if LOGGING_LOG_LEVEL <= 5
    #define LOG_RAW(...) ::threadsafe_logger::Logger::log_raw(__VA_ARGS__)
#else
    #define LOG_RAW(...) ((void)0)
#endif
#if LOGGING_LOG_LEVEL <= 0
    #define LOG_TRACE_STREAM ::threadsafe_logger::Logger::stream_log_trace()
#else
    #define LOG_TRACE_STREAM ::threadsafe_logger::Logger::null_stream()
#endif
#if LOGGING_LOG_LEVEL <= 1
    #define LOG_DEBUG_STREAM ::threadsafe_logger::Logger::stream_log_debug()
#else
    #define LOG_DEBUG_STREAM ::threadsafe_logger::Logger::null_stream()
#endif
#if LOGGING_LOG_LEVEL <= 2
    #define LOG_INFO_STREAM ::threadsafe_logger::Logger::stream_log_info()
#else
    #define LOG_INFO_STREAM ::threadsafe_logger::Logger::null_stream()
#endif
#if LOGGING_LOG_LEVEL <= 3
    #define LOG_WARN_STREAM ::threadsafe_logger::Logger::stream_log_warn()
#else
    #define LOG_WARN_STREAM ::threadsafe_logger::Logger::null_stream()
#endif
#if LOGGING_LOG_LEVEL <= 4
    #define LOG_ERROR_STREAM ::threadsafe_logger::Logger::stream_log_error()
#else
    #define LOG_ERROR_STREAM ::threadsafe_logger::Logger::null_stream()
#endif
#define LOG_STREAM ::threadsafe_logger::Logger::stream_log_default()
#if LOGGING_LOG_LEVEL <= 5
    #define LOG_RAW_STREAM ::threadsafe_logger::Logger::stream_log_raw()
#else
    #define LOG_RAW_STREAM ::threadsafe_logger::Logger::null_stream()
#endif
struct ErrorSite { const char* file; const char* function; int line; };
class Exception : public std::exception {
protected:
    std::string msg_;
    ErrorSite site_;
#if defined(__APPLE__) || defined(__linux__)
    static constexpr int kMaxFrames = 64;
    int frame_count_{0};
    void* frames_[kMaxFrames]{};
#endif
public:
    explicit Exception(const std::string &message
#if LOGGING_ENABLE_STACKTRACE
        , const char* file = __builtin_FILE(),
        int line = __builtin_LINE(),
        const char* function = __builtin_FUNCTION()
#if defined(__APPLE__) || defined(__linux__)
        , int capture_frames = 0
        , void* const* captured_frames = nullptr
#endif
#endif
    );
    virtual ~Exception() noexcept {}
    virtual const char* what() const noexcept override {
        return msg_.c_str();
    }
#if LOGGING_ENABLE_STACKTRACE
    const ErrorSite& site() const noexcept {
        return site_;
    }
    void dump_stacktrace() const noexcept;
#if defined(__APPLE__) || defined(__linux__)
    int frame_count() const noexcept;
    void* const* frames() const noexcept;
#endif
#endif
};
using Exception = Exception;  ///< Back-compat name; new code should use Exception.
inline Exception::Exception(const std::string& message
#if LOGGING_ENABLE_STACKTRACE
    , const char* file, int line, const char* function
#if defined(__APPLE__) || defined(__linux__)
    , int capture_frames, void* const* captured_frames
#endif
#endif
    ) : msg_(message)
#if LOGGING_ENABLE_STACKTRACE
    , site_{file, function, line}
#endif
{
#if LOGGING_ENABLE_STACKTRACE && (defined(__APPLE__) || defined(__linux__))
    if (captured_frames && capture_frames > 0) {
        frame_count_ = std::min(capture_frames, kMaxFrames);
        std::copy_n(captured_frames, frame_count_, frames_);
    } else if (capture_frames > 0) frame_count_ = ::backtrace(frames_, std::min(capture_frames, kMaxFrames));
#endif
}
#if LOGGING_ENABLE_STACKTRACE
inline void Exception::dump_stacktrace() const noexcept {
#if defined(__APPLE__) || defined(__linux__)
    if (frame_count_ <= 0) return;
    char** symbols = ::backtrace_symbols(frames_, frame_count_);
    if (!symbols) return;
    std::ostringstream output;
    for (int index = 0; index < frame_count_; ++index) output << '\n' << "  [" << index << "] " << symbols[index];
    std::free(symbols);
    Logger::log_message(LogLevel::ERROR, output.str());
#endif
}
#if defined(__APPLE__) || defined(__linux__)
inline int Exception::frame_count() const noexcept { return frame_count_; }
inline void* const* Exception::frames() const noexcept { return frames_; }
#endif
#endif
#define THROW(msg) throw ::threadsafe_logger::Exception(msg)
#define EXCEPTION_CLASS(classname) class classname##Exception : public ::threadsafe_logger::Exception { \
public: \
    using Exception::Exception; \
    using Exception::what; \
}; \
inline static void classname##_throw(const std::string& msg) { throw classname##Exception(msg); }
} // namespace threadsafe_logger
#endif // LOGGING_LOGGER_LOADED
