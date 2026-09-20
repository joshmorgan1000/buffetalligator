/** --------------------------------------------------------------------------------------------------------- Logging Tests
 * @file test_logging.cpp
 * @brief Tests logging overloads, macros, formatting, and serialized concurrent output.
 */
#include <logging.hpp>
#include <loggingutils.hpp>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}
struct Formattable {
    std::string value;
    std::string toString() const { return value; }
};
class ScopedLogOutput {
private:
    std::ostringstream output_;
    std::ostream* original_stdout_;
    std::ostream* original_additional_;
    bool original_json_;
    bool original_colored_;
    threadsafe_logger::Logger::LogLevel original_level_;
    std::string original_component_;
    std::string original_instance_name_;
public:
    ScopedLogOutput() :
        original_stdout_(threadsafe_logger::stdout_stream().exchange(&output_)),
        original_additional_(threadsafe_logger::additional_stream().exchange(nullptr)),
        original_json_(threadsafe_logger::log_format_json().exchange(false)),
        original_level_(threadsafe_logger::Logger::level()),
        original_component_(threadsafe_logger::component()),
        original_instance_name_(threadsafe_logger::instance_name()) {
        threadsafe_logger::logging::GlobalLoggingContext::instance();
        original_colored_ =
            threadsafe_logger::logging::GlobalLoggingContext::colored_output().exchange(false);
        threadsafe_logger::Logger::level() = threadsafe_logger::Logger::LogLevel::TRACE;
        threadsafe_logger::component().clear();
        threadsafe_logger::instance_name().clear();
    }
    ~ScopedLogOutput() {
        threadsafe_logger::stdout_stream().store(original_stdout_);
        threadsafe_logger::additional_stream().store(original_additional_);
        threadsafe_logger::log_format_json().store(original_json_);
        threadsafe_logger::logging::GlobalLoggingContext::colored_output().store(original_colored_);
        threadsafe_logger::Logger::level() = original_level_;
        threadsafe_logger::component() = original_component_;
        threadsafe_logger::instance_name() = original_instance_name_;
    }
    std::string str() const { return output_.str(); }
};
void test_text_logging_and_macros() {
    ScopedLogOutput capture;
    LOG_TRACE("trace");
    LOG_DEBUG("debug ", 7);
    LOG_INFO(std::string("info"));
    LOG_WARN(Formattable{"converted"});
    LOG_ERROR("error");
    LOG_TRACE_STREAM << "stream-trace";
    LOG_DEBUG_STREAM << "stream-debug";
    LOG_INFO_STREAM << "stream-info";
    LOG_WARN_STREAM << "stream-warn";
    LOG_ERROR_STREAM << "stream-error " << 9;
    LOG_RAW("raw");
    LOG_RAW_STREAM << "stream-raw";
    LOG_STREAM << "default-level";
    const std::string output = capture.str();
    expect((LOGGING_LOG_LEVEL <= 0) == (output.find(" [TRACE] trace\n") != std::string::npos),
        "LOG_TRACE respects the compile-time level");
    expect((LOGGING_LOG_LEVEL <= 1) == (output.find(" [DEBUG] debug 7\n") != std::string::npos),
        "LOG_DEBUG concatenates mixed arguments at its compile-time level");
    expect((LOGGING_LOG_LEVEL <= 2) == (output.find(" [INFO]  info\n") != std::string::npos),
        "LOG_INFO logs std::string values at its compile-time level");
    expect((LOGGING_LOG_LEVEL <= 3) == (output.find(" [WARN]  converted\n") != std::string::npos),
        "LOG_WARN uses toString values at its compile-time level");
    expect((LOGGING_LOG_LEVEL <= 4) == (output.find(" [ERROR] error\n") != std::string::npos),
        "LOG_ERROR respects the compile-time level");
    expect((LOGGING_LOG_LEVEL <= 0) == (output.find(" [TRACE] stream-trace\n") != std::string::npos),
        "LOG_TRACE_STREAM respects the compile-time level");
    expect((LOGGING_LOG_LEVEL <= 1) == (output.find(" [DEBUG] stream-debug\n") != std::string::npos),
        "LOG_DEBUG_STREAM respects the compile-time level");
    expect((LOGGING_LOG_LEVEL <= 2) == (output.find(" [INFO]  stream-info\n") != std::string::npos),
        "LOG_INFO_STREAM respects the compile-time level");
    expect((LOGGING_LOG_LEVEL <= 3) == (output.find(" [WARN]  stream-warn\n") != std::string::npos),
        "LOG_WARN_STREAM respects the compile-time level");
    expect((LOGGING_LOG_LEVEL <= 4) == (output.find(" [ERROR] stream-error 9\n") != std::string::npos),
        "LOG_ERROR_STREAM respects the compile-time level");
    expect(output.find("raw\n") != std::string::npos, "LOG_RAW omits the timestamp and level");
    expect(output.find("\nstream-raw\n") != std::string::npos, "LOG_RAW_STREAM omits the timestamp and level");
    expect(output.find(" [TRACE] default-level\n") != std::string::npos, "LOG_STREAM uses the current level");
}
void test_json_logging() {
    ScopedLogOutput capture;
    threadsafe_logger::component() = "parser\"core";
    threadsafe_logger::instance_name() = "worker\\one";
    threadsafe_logger::log_format_json().store(true);
    threadsafe_logger::Logger::log_message(
        threadsafe_logger::Logger::LogLevel::INFO, "line\n\tquoted\"");
    const std::string output = capture.str();
    expect(output.find("\"schema_version\":1") != std::string::npos, "JSON includes the schema version");
    expect(output.find("\"level\":\"INFO\"") != std::string::npos, "JSON includes the log level");
    expect(output.find("\"component\":\"parser\\\"core\"") != std::string::npos, "JSON escapes the component");
    expect(output.find("\"instance\":\"worker\\\\one\"") != std::string::npos, "JSON escapes the instance");
    expect(output.find("\"message\":\"line\\n\\tquoted\\\"\"") != std::string::npos, "JSON escapes the message");
}
void test_concurrent_output() {
    ScopedLogOutput capture;
    const int thread_count = 4;
    const int messages_per_thread = 50;
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([thread_index, messages_per_thread] {
            for (int message_index = 0; message_index < messages_per_thread; ++message_index) {
                LOG_RAW("worker=" + std::to_string(thread_index) + ",message=" + std::to_string(message_index));
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    std::istringstream lines(capture.str());
    std::set<std::string> messages;
    std::string line;
    while (std::getline(lines, line)) messages.insert(line);
    bool complete = static_cast<int>(messages.size()) == thread_count * messages_per_thread;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        for (int message_index = 0; message_index < messages_per_thread; ++message_index) {
            const std::string expected = "worker=" + std::to_string(thread_index) +
                ",message=" + std::to_string(message_index);
            complete = complete && messages.count(expected) == 1;
        }
    }
    expect(complete, "concurrent logging emits every message without interleaving");
}
} // namespace
int main() {
    test_text_logging_and_macros();
    test_json_logging();
    test_concurrent_output();
    return failures == 0 ? 0 : 1;
}
