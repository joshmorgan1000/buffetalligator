/** --------------------------------------------------------------------------------------------------------- Internal logging implementation
 * @file logging.cpp
 * @brief Internal implementation of logging utilities and global logging context.
 */
#include <cstdint>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <iomanip>
#include <cmath>
#include <functional>
#include <chrono>
#include <vector>
#include <algorithm>
#include <random>
#include <cstdlib>
#include <deque>
#include <queue>
#include <unordered_map>
#include <logging.hpp>
#if defined(_WIN32) || defined(_WIN64)
    #define LOGGER_PLATFORM_WINDOWS 1
    #define LOGGER_PLATFORM_POSIX 0
    #include <io.h>
    #include <windows.h>
    #define LOGGER_ISATTY(fd) _isatty(fd)
    #define LOGGER_FILENO(f) _fileno(f)
#else
    #define LOGGER_PLATFORM_WINDOWS 0
    #define LOGGER_PLATFORM_POSIX 1
    #include <sys/ioctl.h>
    #include <unistd.h>
    #include <termios.h>
    #ifdef B0
    #undef B0
    #endif
    #define LOGGER_ISATTY(fd) isatty(fd)
    #define LOGGER_FILENO(f) fileno(f)
#endif
#if defined(__APPLE__) || defined(__linux__)
    #include <execinfo.h>  // For backtrace support
#endif
#include <ctime>
#include <cstdlib>
#include <csignal>
#include <unordered_set>
#include <loggingutils.hpp>

namespace threadsafe_logger {
using namespace logging;
class InternalLogger;
Logger& Logger::instance() {
    static Logger logger_instance;
    return logger_instance;
}
class Logger::Impl {
private:
    static Logger& instance() {
        static Logger logger_instance;
        return logger_instance;
    }
    static LogLevel& level() {
        static LogLevel current_level = LogLevel::INFO;
        return current_level;
    }
    static uint64_t getHighPrecisionTimestamp();
    static uint64_t getMicrosecondTimestamp();
    static void RedrawAllProgressBars();
    static std::atomic<uint32_t>& progress_creation_counter();
    static std::function<void(float)> log_progress(const std::string& header);
    static void install_uncaught_logger();
    static void UpdateProgressBar(const ProgressBar& bar);
    friend class Logger;
    friend class ::threadsafe_logger::InternalLogger;
    friend class ModelPicker;
};
uint64_t Logger::get_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
uint64_t Logger::Impl::getHighPrecisionTimestamp() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(); }
uint64_t Logger::Impl::getMicrosecondTimestamp() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void Logger::Impl::UpdateProgressBar(const ProgressBar& bar) {
    auto& bars = GlobalLoggingContext::instance().progress_bars();
    int pos_from_bottom = 0;
    for (const auto& [id, other] : bars) {
        if (other.creation_order < bar.creation_order) {
            pos_from_bottom++;
        }
    }
    int lines_up = pos_from_bottom + 1;
    std::string content = RenderProgressBarLine(bar);
    std::ostringstream oss;
    if (lines_up > 0) {
        oss << "\033[" << lines_up << "A";
    }
    oss << "\033[2K\r" << content;
    if (lines_up > 0) {
        oss << "\033[" << lines_up << "B";
    }
    oss << "\r";
    GlobalLoggingContext::stdout_lock();
    std::cout << oss.str();
    std::cout.flush();
    GlobalLoggingContext::stdout_unlock();
}
void Logger::Impl::RedrawAllProgressBars() {
    std::unique_lock lock(GlobalLoggingContext::instance().progress_mutex());
    std::vector<ProgressBar*> sorted;
    for (auto& [id, bar] : GlobalLoggingContext::instance().progress_bars()) {
        sorted.push_back(&bar);
    }
    std::sort(sorted.begin(), sorted.end(), [](const ProgressBar* a, const ProgressBar* b) {
        return a->creation_order > b->creation_order;
    });
    for (auto* bar : sorted) {
        UpdateProgressBar(*bar);
    }
}
std::atomic<uint32_t>& Logger::Impl::progress_creation_counter() {
    static std::atomic<uint32_t> counter{0};
    return counter;
}
std::function<void(float)> Logger::Impl::log_progress(const std::string& header) {
    std::string random_string = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string random_id;
    for (int i = 0; i < 32; i++) {
        random_id += random_string[rand() % random_string.length()];
    }
    uint64_t order = progress_creation_counter().fetch_add(1, std::memory_order_relaxed);
    {
        int term_width = get_terminal_width();
        int bar_width = term_width - static_cast<int>(header.length()) - 55;
        if (bar_width < 10) {
            bar_width = 10;
        }
        std::string timestamp = GlobalLoggingContext::get_timestamp();
        std::unique_lock lock(GlobalLoggingContext::instance().progress_mutex());
        ProgressBar bar = {
            random_id,
            header,
            timestamp,
            0.0f,
            bar_width,
            0,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()),
            order
        };
        GlobalLoggingContext::instance().progress_bars()[random_id] = bar;
        GlobalLoggingContext::stdout_lock();
        std::cout << "\n";
        std::cout.flush();
        GlobalLoggingContext::stdout_unlock();
    }
    RedrawAllProgressBars();
    return [random_id](float progress) {
        std::unique_lock lock(GlobalLoggingContext::instance().progress_mutex());
        auto it = GlobalLoggingContext::instance().progress_bars().find(random_id);
        if (it == GlobalLoggingContext::instance().progress_bars().end()) {
            return;
        }
        it->second.progress = progress;
        if (progress >= 1.0f) {
            int pos_from_bottom = 0;
            auto& bars = GlobalLoggingContext::instance().progress_bars();
            for (const auto& [id, other] : bars) {
                if (other.creation_order < it->second.creation_order) {
                    pos_from_bottom++;
                }
            }
            int lines_up = pos_from_bottom + 1;
            std::string completed = RenderProgressBarLine(it->second);
            bars.erase(it);
            int remaining = static_cast<int>(bars.size());
            lock.unlock();
            GlobalLoggingContext::stdout_lock();
            std::ostringstream oss;
            if (lines_up > 0) {
                oss << "\033[" << lines_up << "A";
            }
            oss << "\033[2K" << completed << "\n";
            oss << "\033[M";
            if (lines_up - 1 > 0) {
                oss << "\033[" << (lines_up - 1) << "B";
            }
            oss << "\r";
            std::cout << oss.str();
            std::cout.flush();
            GlobalLoggingContext::stdout_unlock();
            if (remaining > 0) {
                RedrawAllProgressBars();
            }
        }
        else {
            UpdateProgressBar(it->second);
        }
    };
}
/** --------------------------------------------------------------------------------------------------------- log_message Method
 * @brief Logs a message with the specified log level. This method handles formatting, coloring, and
 * routing of the log message to the appropriate output streams (stdout, stderr, additional log stream).
 * It also supports JSON formatting for structured logging if enabled.
 * @param level The log level of the message (e.g., INFO, WARN, ERROR)
 * @param message The message to log
 */
void GlobalLoggingContext::log_message(Logger::LogLevel level, const std::string& message) {
    const bool raw_output = level == Logger::LogLevel::RAW;
    if (!raw_output && GlobalLoggingContext::log_format_json().load(std::memory_order_acquire)) {
        std::string line;
        line.reserve(160 + message.size() + GlobalLoggingContext::component().size() +
            GlobalLoggingContext::instance_name().size());
        line += "{\"schema_version\":1,\"timestamp\":\"";
        line += GlobalLoggingContext::get_iso8601_timestamp();
        line += "\",\"level\":\"";
        line += LogLevelName(level);
        line += "\",\"component\":\"";
        GlobalLoggingContext::json_escape_into(GlobalLoggingContext::component(), line);
        line += "\",\"instance\":\"";
        GlobalLoggingContext::json_escape_into(GlobalLoggingContext::instance_name(), line);
        line += "\",\"message\":\"";
        GlobalLoggingContext::json_escape_into(message, line);
        line += "\"}\n";
        std::ostream* json_out = GlobalLoggingContext::stdout_stream().load(std::memory_order_acquire);
        std::ostream* json_additional_out = GlobalLoggingContext::addtl_log_stream().load(
            std::memory_order_acquire);
        if (json_out != nullptr) {
            GlobalLoggingContext::stdout_lock();
            (*json_out) << line;
            json_out->flush();
            GlobalLoggingContext::stdout_unlock();
        }
        if (json_additional_out != nullptr) {
            (*json_additional_out) << line;
            json_additional_out->flush();
        }
        return;
    }
    if (!raw_output && GlobalLoggingContext::is_claude()) {
        std::stringstream oss;
        oss << "threadsafe_logger:" << (level == Logger::LogLevel::ERROR ? "error" :
            (level == Logger::LogLevel::WARN ? "warn" : (level == Logger::LogLevel::INFO ? "info" :
            (level == Logger::LogLevel::DEBUG ? "debug" : "trace")))) << ": " << message << "\n";
        GlobalLoggingContext::stdout_lock();
        std::cout << oss.str();
        std::cout.flush();
        GlobalLoggingContext::stdout_unlock();
        return;
    }
    if (GlobalLoggingContext::global_log_level() > level) {
        return;
    }
    std::stringstream oss;
    std::stringstream oss_plain;
    const bool stdout_tty = stdout_is_tty();
    const bool colored_output = GlobalLoggingContext::colored_output().load(
        std::memory_order_acquire);
    if (stdout_tty && colored_output && !raw_output) {
        oss << TTYGREY << GlobalLoggingContext::get_timestamp();
        switch (level) {
            case Logger::LogLevel::INFO:    oss << INFO_LOG_STANZA; break;
            case Logger::LogLevel::WARN:    oss << WARN_LOG_STANZA; break;
            case Logger::LogLevel::ERROR:   oss << ERROR_LOG_STANZA; break;
            case Logger::LogLevel::DEBUG:   oss << DEBUG_LOG_STANZA; break;
            case Logger::LogLevel::TRACE:   oss << TRACE_LOG_STANZA; break;
            case Logger::LogLevel::RAW:     break;
            default:                oss << " [UNKWN] "; break;
        }
        oss << " " << message << TTYRESET << "\n";
    }
    if (raw_output) {
        oss_plain << message << "\n";
    } else {
        oss_plain << GlobalLoggingContext::get_timestamp();
        switch (level) {
            case Logger::LogLevel::INFO:    oss_plain << " [INFO] "; break;
            case Logger::LogLevel::WARN:    oss_plain << " [WARN] "; break;
            case Logger::LogLevel::ERROR:   oss_plain << " [ERROR]"; break;
            case Logger::LogLevel::DEBUG:   oss_plain << " [DEBUG]"; break;
            case Logger::LogLevel::TRACE:   oss_plain << " [TRACE]"; break;
            case Logger::LogLevel::RAW:     break;
            default:                oss_plain << " [UNKWN]"; break;
        }
        oss_plain << " " << message << "\n";
    }
    const std::string plain_line = oss_plain.str();
    const std::string console_line = stdout_tty && colored_output && !raw_output
        ? oss.str() : plain_line;
    int lines = 0;
    int char_count = 0;
    int term_width = get_terminal_width();
    for (char c : console_line) {
        char_count++;
        if (char_count % term_width == 0) {
            lines++;
        }
        if (c == '\n') {
            lines++;
            char_count = 0;
        }
    }
    std::vector<std::string> line_buffers;
    std::string line_buffer;
    line_buffer.resize(term_width, ' ');
    line_buffer[term_width - 1] = 'E';    line_buffer.push_back('\n');
    line_buffers.resize(lines + 1, line_buffer);
    std::ostream* out_ptr_ = GlobalLoggingContext::stdout_stream().load(std::memory_order_acquire);
    std::ostream* additional_out_ptr = GlobalLoggingContext::addtl_log_stream().load(
        std::memory_order_acquire);
    if (additional_out_ptr != nullptr) {
        (*additional_out_ptr) << plain_line;
        additional_out_ptr->flush();
    }
    if (GlobalLoggingContext::log_message_route_to_compositor(console_line)) {
        return;
    }
    std::stringstream linebumps;
    if (out_ptr_ != nullptr) {
        std::lock_guard<std::mutex> plock(GlobalLoggingContext::instance().progress_mutex());
        auto& bars = GlobalLoggingContext::instance().progress_bars();
        size_t n_bars = bars.size();
        if (n_bars > 0) {
            std::ostringstream out;
            if (n_bars > 0) {
                for (size_t i = 0; i < n_bars; ++i) {
                    linebumps << line_buffer;
                }
            }
            for (size_t i = 0; i < n_bars + static_cast<size_t>(lines) + 1; ++i) {
                out << "\033[1A"; }
            for (size_t i = 0; i < n_bars + static_cast<size_t>(lines); ++i) {
                out << line_buffers[i];
            }
            for (size_t i = 0; i < n_bars + static_cast<size_t>(lines); ++i) {
                out << "\033[1A";
            }
            out << "\033[2K" << console_line;
            std::vector<ProgressBar*> sorted;
            for (auto& [id, bar] : bars) {
                sorted.push_back(&bar);
            }
            std::sort(sorted.begin(), sorted.end(), [](const ProgressBar* a, const ProgressBar* b) {
                return a->creation_order > b->creation_order;
            });
            for (auto* bar : sorted) {
                out << "\033[2K" << RenderProgressBarLine(*bar) << "\n";
            }
            GlobalLoggingContext::stdout_lock();
            (*out_ptr_) << linebumps.str();
            (*out_ptr_) << out.str();
            out_ptr_->flush();
            GlobalLoggingContext::stdout_unlock();
            GlobalLoggingContext::stdout_current_line() += lines;
        }
        else {
            GlobalLoggingContext::stdout_lock();
            (*out_ptr_) << console_line;
            out_ptr_->flush();
            GlobalLoggingContext::stdout_unlock();
            GlobalLoggingContext::stdout_current_line() += lines;
        }
    }
}
void Logger::install_uncaught_logger() {
    Impl::install_uncaught_logger();
}
void Logger::Impl::install_uncaught_logger() {
    std::set_terminate([]() noexcept {
        try {
            if (const std::exception_ptr current = std::current_exception()) std::rethrow_exception(current);
            GlobalLoggingContext::log_message(Logger::LogLevel::ERROR, "terminate called without an active exception");
        } catch (const Exception& error) {
            GlobalLoggingContext::log_message(Logger::LogLevel::ERROR, std::string("uncaught exception: ") + error.what());
#if ENABLE_STACKTRACE
            error.dump_stacktrace();
#endif
        } catch (const std::exception& error) {
            GlobalLoggingContext::log_message(Logger::LogLevel::ERROR, std::string("uncaught standard exception: ") + error.what());
        } catch (...) {
            GlobalLoggingContext::log_message(Logger::LogLevel::ERROR, "uncaught non-standard exception");
        }
        std::abort();
    });
}
Logger::Logger() : impl_(nullptr), level_(LogLevel::INFO) {}
Logger::LogLevel& Logger::level() {
    return Logger::Impl::level();
}
void Logger::log_message(LogLevel level, const std::string& message) {
    GlobalLoggingContext::log_message(level, message);
}
std::string& Logger::thread_context() {
    return GlobalLoggingContext::thread_context();
}
std::string& Logger::component() {
    return GlobalLoggingContext::component();
}
std::string& Logger::instance_name() {
    return GlobalLoggingContext::instance_name();
}
std::atomic<std::ostream*>& Logger::stdout_stream() {
    return GlobalLoggingContext::stdout_stream();
}
std::atomic<std::ostream*>& Logger::stderr_stream() {
    return GlobalLoggingContext::stderr_stream();
}
std::atomic<std::ostream*>& Logger::additional_stream() {
    return GlobalLoggingContext::addtl_log_stream();
}
std::atomic<bool>& Logger::log_format_json() {
    return GlobalLoggingContext::log_format_json();
}
std::string Logger::get_iso8601_timestamp(uint64_t now_ms) {
    return GlobalLoggingContext::get_iso8601_timestamp(now_ms);
}
bool LogoAnimation::advance_frame(uint64_t epoch_ms) {
    if (logo_animation_time_ == 0) {
        logo_animation_time_ =
            static_cast<int64_t>(epoch_ms)
            + static_cast<int64_t>(INTRO_DURATION_MS);
    }
    const int sw = static_cast<int>(get_terminal_width());
    const int sh = INTRO_ROWS;
    const size_t cells = static_cast<size_t>(sw) * static_cast<size_t>(sh);
    if (chars == nullptr || last_sw_ != sw) {
        delete[] chars;
        delete[] colors;
        chars = new uint32_t[cells]();
        colors = new TerminalColors[cells];
        last_sw_ = sw;
    }
    for (size_t i = 0; i < cells; ++i) {
        chars[i] = 0x20u;
        colors[i] = TerminalColors::TRANSPARENT;
    }
    std::vector<BraillePoint::CombinedPoint> combined(cells);
    const int64_t anim_start_ms =
        logo_animation_time_ - static_cast<int64_t>(INTRO_DURATION_MS);
    const int64_t elapsed_ms =
        static_cast<int64_t>(epoch_ms) - anim_start_ms;
    const int64_t finale_start =
        finale_start_ms_.load(std::memory_order_acquire);
    const int64_t finale_elapsed_ms = (finale_start == 0)
        ? INT64_MIN / 2
        : static_cast<int64_t>(epoch_ms) - finale_start;
    {
        static thread_local std::mt19937 spawn_rng{std::random_device{}()};
        constexpr uint64_t P1_START_MS = 0;
        constexpr uint64_t P1_END_MS = 500;
        constexpr uint64_t P2_START_MS = 1100;   // 600ms pause after P1
        constexpr uint64_t P2_END_MS = 1900;
        constexpr uint64_t P3_START_MS = 2500;   // 600ms pause after P2
        constexpr uint64_t P3_END_MS = 3700;   // finishes just before reveal
        constexpr int P1_TARGET = 60;
        constexpr int P2_DELTA = 70;
        constexpr int P3_DELTA = 120;
        const uint64_t elapsed = static_cast<uint64_t>(
            std::max<int64_t>(0, elapsed_ms));
        auto ramp = [&](uint64_t start, uint64_t end, int delta) -> int {
            if (elapsed <= start) {
                return 0;
            }
            if (elapsed >= end) {
                return delta;
            }
            const double frac =
                static_cast<double>(elapsed - start)
                / static_cast<double>(end - start);
            return static_cast<int>(frac * static_cast<double>(delta));
        };
        const int p1_want = ramp(P1_START_MS, P1_END_MS, P1_TARGET);
        const int p2_want = ramp(P2_START_MS, P2_END_MS, P2_DELTA);
        const int p3_want = ramp(P3_START_MS, P3_END_MS, P3_DELTA);
        {
            std::unique_lock<std::shared_mutex> lock(braille_points_mutex_);
            while (static_cast<int>(braille_points_.size()) < p1_want) {
                double r =
                    std::uniform_real_distribution<>(3.0, 10.0)(spawn_rng);
                double tilt =
                    std::uniform_real_distribution<>(1.00, 1.60)(spawn_rng);
                double tilt_axis =
                    std::uniform_real_distribution<>(-0.45, 0.45)(spawn_rng);
                TerminalColors color = PALETTE_[
                    std::uniform_int_distribution<>(
                        0, PALETTE_.size() - 1)(spawn_rng)];
                spawn_particle(epoch_ms, r, tilt, tilt_axis, 0.6, color, spawn_rng);
            }
            while (static_cast<int>(braille_points_.size())
                   < p1_want + p2_want) {
                double r =
                    std::uniform_real_distribution<>(14.0, 25.0)(spawn_rng);
                double tilt =
                    std::uniform_real_distribution<>(1.40, 1.50)(spawn_rng);
                double tilt_axis =
                    std::uniform_real_distribution<>(-0.10, 0.10)(spawn_rng);
                TerminalColors color = PALETTE_[
                    std::uniform_int_distribution<>(
                        0, PALETTE_.size() - 1)(spawn_rng)];
                spawn_particle(epoch_ms, r, tilt, tilt_axis, 0.5, color, spawn_rng);
            }
            while (static_cast<int>(braille_points_.size())
                   < p1_want + p2_want + p3_want) {
                constexpr double OUTER_R_MIN = 22.0;
                const double outer_r_max =
                    static_cast<double>(sw) / 3.3;
                const double u =
                    std::uniform_real_distribution<>(0.0, 1.0)(spawn_rng);
                double r =
                    OUTER_R_MIN + (outer_r_max - OUTER_R_MIN) * (u * u);
                constexpr double OUTER_TILT = 1.53;
                constexpr double OUTER_TILT_AXIS = 0.0;
                double tilt = OUTER_TILT;
                double tilt_axis = OUTER_TILT_AXIS;
                TerminalColors color = PALETTE_[
                    std::uniform_int_distribution<>(
                        0, PALETTE_.size() - 1)(spawn_rng)];
                spawn_particle(epoch_ms, r, tilt, tilt_axis, 0.4,
                               color, spawn_rng);
            }
        }
    }
    /// Finale timeline (relative to finale trigger). Kept verbatim.
    constexpr int64_t IMPLODE_START_MS = 800;
    constexpr int64_t IMPLODE_END_MS = 4000;
    constexpr int64_t EXPLODE_END_MS = 4500;
    double finale_scale = 1.5;
    bool particles_hidden = false;
    if (finale_elapsed_ms >= IMPLODE_START_MS && finale_elapsed_ms < IMPLODE_END_MS) {
        const double p = static_cast<double>(finale_elapsed_ms - IMPLODE_START_MS)
            / static_cast<double>(IMPLODE_END_MS - IMPLODE_START_MS);
        finale_scale = 1.5 - 1.42 * p;
    } else if (finale_elapsed_ms >= IMPLODE_END_MS && finale_elapsed_ms < EXPLODE_END_MS) {
        const double p = static_cast<double>(finale_elapsed_ms - IMPLODE_END_MS)
            / static_cast<double>(EXPLODE_END_MS - IMPLODE_END_MS);
        const double inv = 1.0 - p;
        const double ease = 1.0 - inv * inv * inv;
        finale_scale = 0.08 + 15.0 * ease;
    } else if (finale_elapsed_ms >= EXPLODE_END_MS) {
        particles_hidden = true;
    }
    if (!particles_hidden && !braille_points_.empty()) {
        const double t_now_s = static_cast<double>(epoch_ms) / 1000.0;
        constexpr double ORBIT_COL_OFFSET = 4.0;
        constexpr double ORBIT_ROW_OFFSET = 2.0;
        const double logo_col = logo_.position.column + ORBIT_COL_OFFSET;
        const double logo_row = logo_.position.row + ORBIT_ROW_OFFSET;
        constexpr double EMERGE_DURATION_S = 3.5;
        for (auto& bp : braille_points_) {
            const double age_s = t_now_s - bp.birth_time_sec;
            const double frac = std::min(1.0, std::max(0.0, age_s / EMERGE_DURATION_S));
            const double one_minus = 1.0 - frac;
            const double scale = 1.0 - one_minus * one_minus * one_minus;
            const double r_eff = bp.orbit_r * scale * finale_scale;
            const double angle = bp.orbit_phase + bp.orbit_speed * age_s;
            const double cx = r_eff * std::cos(angle);
            const double cy = r_eff * std::sin(angle);
            const double cosA = std::cos(bp.orbit_tilt_axis);
            const double sinA = std::sin(bp.orbit_tilt_axis);
            const double x1 =  cx * cosA + cy * sinA;
            const double y1 = -cx * sinA + cy * cosA;
            const double cosT = std::cos(bp.orbit_tilt);
            const double x2 = x1;
            const double y2 = y1 * cosT;
            const double wx = x2 * cosA - y2 * sinA;
            const double wy = x2 * sinA + y2 * cosA;
            bp.position.column = logo_col + wx * 1.8;
            bp.position.row = logo_row + wy * 2.0;
        }
        {
            std::shared_lock<std::shared_mutex> lock(braille_points_mutex_);
            static constexpr uint8_t dot_values[2][4] = {
                {64, 16, 4, 1},
                {128, 32, 8, 2}
            };
            for (const auto& point : braille_points_) {
                const int char_col = point.position.x_pos();
                const int char_row = point.position.y_pos();
                if (char_col < 0 || char_col >= sw
                    || char_row < 0 || char_row >= sh) {
                    continue;
                }
                const int raw_col = static_cast<int>(std::floor(point.position.column));
                const int raw_row = static_cast<int>(std::floor(point.position.row));
                const int sub_col = ((raw_col % 2) + 2) % 2;
                const int sub_row = ((raw_row % 4) + 4) % 4;
                BraillePoint::CombinedPoint combined_point;
                combined_point.combined_value = dot_values[sub_col][sub_row];
                combined_point.color = point.color;
                combined[static_cast<size_t>(char_row) * sw
                    + static_cast<size_t>(char_col)] += combined_point;
            }
        }
    }
    constexpr int64_t LOGO_REVEAL_START_MS = 3800;
    constexpr int64_t LOGO_REVEAL_END_MS = 12400;
    constexpr int64_t TAGLINE_SWEEP_START_MS = 4500;
    constexpr int64_t TAGLINE_SWEEP_END_MS = 9500;
    constexpr int64_t VERSION_SWEEP_START_MS = 7600;
    constexpr int64_t VERSION_SWEEP_END_MS = 11300;
    constexpr int WINDOW_WIDTH = 3;
    constexpr int TAGLINE_MAX_INDEX = 15;
    constexpr int VERSION_MAX_INDEX = 16;
    constexpr int LOGO_MAX_INDEX = 16;
    static thread_local std::mt19937 reveal_rng{std::random_device{}()};
    auto window_for_progress = [](double progress, int max_index)
            -> std::pair<int, int> {
        const int last = std::min<int>(
            max_index, static_cast<int>(PALETTE_.size()) - 1);
        const double clamped = std::min(1.0, std::max(0.0, progress));
        const int lo = static_cast<int>(clamped * static_cast<double>(last));
        const int hi = std::min(last, lo + WINDOW_WIDTH - 1);
        return {lo, hi};
    };
    const double colour_progress = static_cast<double>(
        elapsed_ms - LOGO_REVEAL_START_MS)
        / static_cast<double>(LOGO_REVEAL_END_MS - LOGO_REVEAL_START_MS);
    constexpr int64_t LOGO_CYAN_START_MS = 2900;
    constexpr int64_t LOGO_WHITE_BACK_MS = 4000;
    TerminalColors logo_override = TerminalColors::NONE_SPECIFIED;
    bool show_frame = false;
    if (finale_elapsed_ms >= LOGO_WHITE_BACK_MS) {
        logo_override = TerminalColors::WHITE;
        show_frame = true;
    } else if (finale_elapsed_ms >= LOGO_CYAN_START_MS) {
        logo_override = TerminalColors::BRIGHT_CYAN;
    } else if (colour_progress >= 1.0) {
        logo_.all_white.store(true, std::memory_order_release);
        logo_override = TerminalColors::WHITE;
    }
    const std::string tagline = "unequaled sentiment proximity detection";
    const std::string version = "v0.3.0";
    constexpr int TAGLINE_VERSION_GAP = 2;
    constexpr int TAGLINE_LEFT_OFFSET = -6;
    constexpr int FRAME_RULE_WIDTH = 55;
    const int logo_left_char = static_cast<int>(std::floor(
        (logo_.position.column + logo_.render_col_offset) / 2.0));
    const int tagline_start_col = logo_left_char + TAGLINE_LEFT_OFFSET;
    const int version_start_col =
        tagline_start_col + static_cast<int>(tagline.size())
        + TAGLINE_VERSION_GAP;
    if (elapsed_ms >= LOGO_REVEAL_START_MS) {
        const auto lh = window_for_progress(colour_progress, LOGO_MAX_INDEX);
        logo_.hitbox(
            combined.data(),
            PALETTE_,
            lh.first,
            lh.second,
            reveal_rng,
            logo_override
        );
    }
    if (show_frame) {
        constexpr int FRAME_EXTRA_LEFT = -8;
        logo_.draw_frame(
            combined.data(),
            TerminalColors::GREY,
            tagline_start_col + FRAME_EXTRA_LEFT,
            FRAME_RULE_WIDTH
        );
    }
    auto draw_sweep_at_col = [&](
        int char_row,
        int start_col,
        const std::string& text,
        double sweep_progress,
        int win_lo,
        int win_hi,
        TerminalColors settle
    ) {
        const int len = static_cast<int>(text.size());
        const double clamped = std::min(1.0, std::max(0.0, sweep_progress));
        const int visible = static_cast<int>(clamped * len);
        std::uniform_int_distribution<int> pick(win_lo, win_hi);
        for (int i = 0; i < visible; ++i) {
            const int col = start_col + i;
            if (col < 0 || col >= sw) {
                continue;
            }
            const size_t idx = static_cast<size_t>(char_row)
                * static_cast<size_t>(sw) + static_cast<size_t>(col);
            combined[idx].char_instead = text[i];
            if (settle != TerminalColors::NONE_SPECIFIED) {
                combined[idx].color = settle;
            } else {
                combined[idx].color =
                    PALETTE_[static_cast<size_t>(pick(reveal_rng))];
            }
        }
    };
    const TerminalColors version_settle = (colour_progress >= 1.0)
        ? TerminalColors::WHITE
        : TerminalColors::NONE_SPECIFIED;
    if (elapsed_ms >= TAGLINE_SWEEP_START_MS) {
        const double sweep = static_cast<double>(
            elapsed_ms - TAGLINE_SWEEP_START_MS)
            / static_cast<double>(TAGLINE_SWEEP_END_MS - TAGLINE_SWEEP_START_MS);
        const auto lh = window_for_progress(colour_progress, TAGLINE_MAX_INDEX);
        draw_sweep_at_col(5, tagline_start_col, tagline, sweep,
                          lh.first, lh.second,
                          TerminalColors::NONE_SPECIFIED);
    }
    if (elapsed_ms >= VERSION_SWEEP_START_MS) {
        const double sweep = static_cast<double>(
            elapsed_ms - VERSION_SWEEP_START_MS)
            / static_cast<double>(VERSION_SWEEP_END_MS - VERSION_SWEEP_START_MS);
        const auto lh = window_for_progress(colour_progress, VERSION_MAX_INDEX);
        draw_sweep_at_col(3, version_start_col, version, sweep,
                          lh.first, lh.second, version_settle);
    }
    for (int r = 0; r < sh; ++r) {
        for (int c = 0; c < sw; ++c) {
            const size_t idx = static_cast<size_t>(r) * sw + c;
            const auto& cp = combined[idx];
            if (cp.char_instead != ' '
                && cp.color != TerminalColors::NONE_SPECIFIED) {
                chars[idx] = static_cast<uint32_t>(
                    static_cast<unsigned char>(cp.char_instead));
                colors[idx] = cp.color;
            } else if (cp.combined_value != 0
                       && cp.color != TerminalColors::NONE_SPECIFIED) {
                const uint8_t uni_bits =
                    BraillePoint::custom_to_unicode(cp.combined_value);
                chars[idx] = 0x2800u + static_cast<uint32_t>(uni_bits);
                colors[idx] = cp.color;
            }
            /// else leave as transparent (initial fill).
        }
    }
    return !(particles_hidden && finale_elapsed_ms >= EXPLODE_END_MS);
}
void LogoAnimation::spawn_particle(
    uint64_t epoch_ms,
    double r,
    double tilt,
    double tilt_axis,
    double base_speed,
    TerminalColors color,
    std::mt19937& gen
) {
    double speed_jitter = std::uniform_real_distribution<>(0.85, 1.15)(gen);
    double speed = base_speed * speed_jitter / std::max(1.0, r * 0.05);
    double phase = std::uniform_real_distribution<>(0, 2.0 * M_PI)(gen);
    BraillePoint bp;
    bp.position = logo_.position;       // gets overwritten on first frame
    bp.color = color;
    bp.orbit_r = r;
    bp.orbit_tilt = tilt;
    bp.orbit_tilt_axis = tilt_axis;
    bp.orbit_speed = speed;
    bp.orbit_phase = phase;
    bp.birth_time_sec = static_cast<double>(epoch_ms) / 1000.0;
    braille_points_.emplace_back(std::move(bp));
}
std::string Logger::format_duration(uint64_t duration_ms) {
    return GlobalLoggingContext::format_duration(duration_ms);
}
bool Logger::stdout_is_tty() {
    return logging::stdout_is_tty();
}
std::function<void(float)> Logger::progress(const std::string& header) {
    if (!stdout_is_tty()) return [](float) {};
    return Impl::log_progress(header);
}
/** --------------------------------------------------------------------------------- Add Logo Window
 * @brief Stand up a window that hosts a `LogoAnimation` at the given
 * top-left position. Width fills the terminal; height matches
 * `LogoAnimation::INTRO_ROWS` plus an optional bottom-padding row.
 * @param top_row    Compositor row (0-indexed) for the window's top.
 * @param pad_rows   Extra blank rows below the logo (default 1).
 * @return Pointer to the LogoAnimation owned by the new window.
 */
LogoAnimation* Terminal::add_logo_window(int top_row, int pad_rows) {
    const int w = std::max(1, get_terminal_width());
    const int h = LogoAnimation::INTRO_ROWS + std::max(0, pad_rows);
    auto screen = std::make_unique<TerminalScreen>(w, h);
    LogoAnimation* logo = screen->add_sprite<LogoAnimation>();
    add_window(TerminalPosition(0.0,
        static_cast<double>(top_row)), std::move(screen),
        [top_row, h]() -> TerminalSize {
            return TerminalSize(
                std::max(1, get_terminal_width()), h);
        });
    return logo;
}
/** --------------------------------------------------------------------------------------------------------- Terminal Screen Draw
 * @brief Compose every sprite owned by this screen into its `rows`/`colors`
 * backing grid. Called by the Terminal singleton before flipping the
 * rendered screen to the front buffer.
 */
bool TerminalScreen::draw() {
    std::vector<SpriteBase*> sprites;
    {
        std::shared_lock<std::shared_mutex> lock(sprites_mutex_);
        std::shared_lock<std::shared_mutex> log_lock(log_messages_mutex_);
        std::shared_lock<std::shared_mutex> bar_lock(progress_bars_mutex_);
        sprites.reserve(owned_sprites_.size() + log_messages_.size() + progress_bars_.size());
        for (const auto& log_msg : log_messages_) {
            sprites.push_back(static_cast<SpriteBase*>(log_msg.get()));
        }
        for (const auto& sprite : owned_sprites_) {
            sprites.push_back(sprite.get());
        }
        for (const auto& bar : progress_bars_) {
            sprites.push_back(static_cast<SpriteBase*>(bar.get()));
        }
    }
    for (int r = 0; r < size.height; ++r) {
        for (int c = 0; c < size.width; ++c) {
            rows[r][c] = 0x20u;
            colors[r][c] = TerminalColors::TRANSPARENT;
        }
    }
    bool still_animating = false;
    const int inset = has_border_ ? 1 : 0;
    for (SpriteBase* sprite : sprites) {
        const bool active = sprite->advance_frame(epoch_ms);
        still_animating = still_animating || active;
        Terminal::draw_sprite(sprite, this, inset, inset);
    }
    if (has_border_) {
        paint_border();
    }
    return still_animating;
}
bool Terminal::LogMessageQueueSprite::advance_frame(uint64_t epoch_ms) {
    bool resized = false;
    if (parent_ != nullptr) {
        const TerminalSize interior = parent_->interior_size();
        const TerminalSize want(
            std::max(0, static_cast<int>(interior.width)
                - parent_margin_right_),
            std::max(0, static_cast<int>(interior.height)
                - parent_margin_bottom_));
        if (want.width != size_.width || want.height != size_.height) {
            size_ = want;
            delete[] chars_;
            delete[] colors_;
            const size_t cells = cell_count();
            chars_ = new uint32_t[cells]();
            colors_ = new TerminalColors[cells];
            for (size_t i = 0; i < cells; ++i) {
                colors_[i] = TerminalColors::TRANSPARENT;
            }
            resized = true;
        }
    }
    bool flip_active_now = false;
    bool flip_horizontal_now = false;
    bool flip_expired = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (flip_end_ms_ != 0) {
            if (epoch_ms >= flip_end_ms_) {
                flip_end_ms_ = 0;
                flip_expired = true;
            } else {
                flip_active_now = true;
                flip_horizontal_now = flip_horizontal_;
            }
        }
    }
    if (!dirty_.exchange(false, std::memory_order_acq_rel)
        && !resized
        && !flip_expired) {
        return false;
    }
    const int w = static_cast<int>(size_.width);
    const int h = static_cast<int>(size_.height);
    if (w <= 0 || h <= 0) {
        return flip_active_now;
    }
    const size_t width_sz = static_cast<size_t>(w);
    const size_t cells = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint32_t> stage_chars(cells, 0x20u);
    std::vector<TerminalColors> stage_colors(
        cells, TerminalColors::TRANSPARENT);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!messages_.empty()) {
            /// First pass - newest → oldest, accumulate row counts.
            const size_t row_cap = static_cast<size_t>(h);
            size_t total_rows = 0;
            size_t oldest_visible_skip = 0;
            auto oldest_it = messages_.rend();
            for (auto it = messages_.rbegin();
                    it != messages_.rend(); ++it) {
                const size_t msg_rows = std::max<size_t>(
                    1, (it->count + width_sz - 1) / width_sz);
                if (total_rows + msg_rows >= row_cap) {
                    oldest_visible_skip =
                        (total_rows + msg_rows) - row_cap;
                    total_rows = row_cap;
                    oldest_it = it;
                    ++oldest_it;
                    break;
                }
                total_rows += msg_rows;
                oldest_it = it;
                ++oldest_it;
            }
            /// Second pass - oldest → newest, top-down.
            int render_row = 0;
            auto fwd_end = messages_.end();
            auto fwd_it = oldest_it.base();
            bool first = true;
            for (; fwd_it != fwd_end && render_row < h; ++fwd_it) {
                const Message& m = *fwd_it;
                const size_t msg_rows = std::max<size_t>(
                    1, (m.count + width_sz - 1) / width_sz);
                const size_t skip_rows = first ? oldest_visible_skip : 0;
                first = false;
                size_t cp_idx = skip_rows * width_sz;
                for (size_t r_in_msg = skip_rows;
                        r_in_msg < msg_rows && render_row < h;
                        ++r_in_msg) {
                    for (int c = 0; c < w && cp_idx < m.count; ++c) {
                        const size_t dst = static_cast<size_t>(render_row)
                            * width_sz + static_cast<size_t>(c);
                        stage_chars[dst] = m.cps[cp_idx];
                        stage_colors[dst] = m.per_cell_colors
                            ? m.per_cell_colors[cp_idx]
                            : m.color;
                        ++cp_idx;
                    }
                    ++render_row;
                }
            }
        }
    }
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            const size_t src = static_cast<size_t>(r) * width_sz
                + static_cast<size_t>(c);
            size_t dst;
            if (!flip_active_now) {
                dst = src;
            } else if (flip_horizontal_now) {
                dst = static_cast<size_t>(r) * width_sz
                    + static_cast<size_t>(w - 1 - c);
            } else {
                dst = static_cast<size_t>(h - 1 - r) * width_sz
                    + static_cast<size_t>(c);
            }
            chars_[dst] = stage_chars[src];
            colors_[dst] = stage_colors[src];
        }
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        bands_.erase(
            std::remove_if(bands_.begin(), bands_.end(),
                [&](const ColorBand& b) {
                    return b.end_ms <= epoch_ms;
                }),
            bands_.end());
        for (const auto& b : bands_) {
            if (b.horizontal) {
                if (b.index < 0 || b.index >= h) {
                    continue;
                }
                for (int c = 0; c < w; ++c) {
                    const size_t idx = static_cast<size_t>(b.index)
                        * width_sz + static_cast<size_t>(c);
                    if (colors_[idx] != TerminalColors::TRANSPARENT) {
                        colors_[idx] = b.color;
                    }
                }
            } else {
                if (b.index < 0 || b.index >= w) {
                    continue;
                }
                for (int r = 0; r < h; ++r) {
                    const size_t idx = static_cast<size_t>(r)
                        * width_sz + static_cast<size_t>(b.index);
                    if (colors_[idx] != TerminalColors::TRANSPARENT) {
                        colors_[idx] = b.color;
                    }
                }
            }
        }
    }
    return false;
}
} // namespace threadsafe_logger
