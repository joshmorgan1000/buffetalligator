#pragma once
/** --------------------------------------------------------------------------------------------------------- Logging Utilities
 * @file loggingutils.hpp
 * @brief Utility classes and constructs for the logging and TUI system.
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
#include <cstring>
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

namespace threadsafe_logger::logging {
inline int get_terminal_width() {
    #if LOGGING_PLATFORM_WINDOWS
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            return csbi.srWindow.Right - csbi.srWindow.Left + 1;
        }
        return 80;
    #else
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
            return w.ws_col;
        }
        return 80;
    #endif
}
inline int get_terminal_height() {
    #if LOGGING_PLATFORM_WINDOWS
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
        return 24;
    #else
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
            return w.ws_row;
        }
        return 24;
    #endif
}
inline int get_terminal_current_column() {
    #if LOGGING_PLATFORM_WINDOWS
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            return csbi.dwTerminalPosition.X;
        }
        return 0;
    #else
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
            return w.ws_col;
        }
        return 0;
    #endif
}
inline int get_terminal_current_row() {
    #if LOGGING_PLATFORM_WINDOWS
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            return csbi.dwTerminalPosition.Y;
        }
        return 0;
    #else
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
            return w.ws_row;
        }
        return 0;
    #endif
}
inline bool stdout_is_tty() {
    #if LOGGING_PLATFORM_WINDOWS
        return _isatty(_fileno(stdout)) != 0;
    #else
        return isatty(STDOUT_FILENO) != 0;
    #endif
}
inline bool stderr_is_tty() {
    #if LOGGING_PLATFORM_WINDOWS
        return _isatty(_fileno(stderr)) != 0;
    #else
        return isatty(STDERR_FILENO) != 0;
    #endif
}
inline bool enable_ansi_colors() {
#if LOGGING_PLATFORM_WINDOWS
    // Try to enable Virtual Terminal Processing on Windows 10+
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) {
        return false;
    }
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(hOut, dwMode)) {
        return false;
    }
    // Also enable for stderr
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        GetConsoleMode(hErr, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hErr, dwMode);
    }
    return true;
#else
    return true;  // POSIX terminals support ANSI natively
#endif
}
struct TerminalScreen;
enum class TerminalColors : char {
    NONE_SPECIFIED = '\0', DEFAULT = 'D', DARK_RED = 'r', RED = 'R', BRIGHT_RED = 'E', DARK_ORANGE = 'o',
    ORANGE = 'O', BRIGHT_ORANGE = 'A', DARK_YELLOW = 'y', YELLOW = 'Y', BRIGHT_YELLOW = 'W', DARK_GREEN = 'g',
    GREEN = 'G', BRIGHT_GREEN = 'N', DARK_CYAN = 'c', CYAN = 'C', BRIGHT_CYAN = 'I', DARK_BLUE = 'b',
    BLUE = 'B', BRIGHT_BLUE = 'U', DARK_MAGENTA = 'm', MAGENTA = 'M', BRIGHT_MAGENTA = 'T', DARK_PURPLE = 'p',
    PURPLE = 'P', BRIGHT_PURPLE = 'Q', BLACK = 'k', GREY = 'w', WHITE = 'K', DARK_GRAY = 'a', TRANSPARENT = ' '
};
inline const char* TTYRED = "\033[31m";
inline const char* TTYYELLOW = "\033[93m";
inline const char* TTYORANGE = "\033[33m";
inline const char* TTYBLUE = "\033[94m";
inline const char* TTYDARKBLUE = "\033[34m";
inline const char* TTYGREEN = "\033[32m";
inline const char* TTYBRGREEN = "\033[92m";
inline const char* TTYLIGHTGREY = "\033[37m";
inline const char* TTYGREY = "\033[90m";
inline const char* TTYDARKGREY = "\033[30m";
inline const char* TTYWHITE = "\033[97m";
inline const char* TTYCYAN = "\033[96m";
inline const char* TTYPURPLE = "\033[95m";
inline const char* TTYRESET = "\033[0m";
inline static const std::string INFO_LOG_STANZA = TTYBRGREEN + std::string(" [INFO] ") + TTYGREEN;
inline static const std::string WARN_LOG_STANZA = TTYYELLOW + std::string(" [WARN] ") + TTYORANGE;
inline static const std::string ERROR_LOG_STANZA = TTYRED + std::string(" [ERROR]") + TTYRED;
inline static const std::string DEBUG_LOG_STANZA = TTYCYAN + std::string(" [DEBUG]") + TTYBLUE;
inline static const std::string TRACE_LOG_STANZA = TTYPURPLE + std::string(" [TRACE]");
inline const char* LogLevelName(Logger::LogLevel level) {
    switch (level) {
        case Logger::LogLevel::INFO:  return "INFO";
        case Logger::LogLevel::WARN:  return "WARN";
        case Logger::LogLevel::ERROR: return "ERROR";
        case Logger::LogLevel::DEBUG: return "DEBUG";
        case Logger::LogLevel::TRACE: return "TRACE";
        case Logger::LogLevel::RAW:   return "";
    }
    return "UNKNOWN";
}
inline static const std::unordered_map<TerminalColors, std::string> terminal_color_to_ansi_code = {
    { TerminalColors::NONE_SPECIFIED, "" }, { TerminalColors::DEFAULT, "\033[0m" }, { TerminalColors::DARK_RED, "\033[31m" }, { TerminalColors::RED, "\033[91m" }, { TerminalColors::BRIGHT_RED, "\033[91m" }, { TerminalColors::DARK_ORANGE, "\033[38;5;208m" }, { TerminalColors::ORANGE, "\033[38;5;214m" }, { TerminalColors::BRIGHT_ORANGE, "\033[38;5;220m" }, { TerminalColors::DARK_YELLOW, "\033[33m" }, { TerminalColors::YELLOW, "\033[93m" }, { TerminalColors::BRIGHT_YELLOW, "\033[93m" }, { TerminalColors::DARK_GREEN, "\033[32m" }, { TerminalColors::GREEN, "\033[92m" }, { TerminalColors::BRIGHT_GREEN, "\033[92m" }, { TerminalColors::DARK_CYAN, "\033[36m" }, { TerminalColors::CYAN, "\033[96m" }, { TerminalColors::BRIGHT_CYAN, "\033[96m" }, { TerminalColors::DARK_BLUE, "\033[34m" }, { TerminalColors::BLUE, "\033[94m" }, { TerminalColors::BRIGHT_BLUE, "\033[94m" }, { TerminalColors::DARK_MAGENTA, "\033[35m" }, { TerminalColors::MAGENTA, "\033[95m" }, { TerminalColors::BRIGHT_MAGENTA, "\033[95m" }, { TerminalColors::DARK_PURPLE, "\033[38;5;129m" }, { TerminalColors::PURPLE, "\033[38;5;135m" }, { TerminalColors::BRIGHT_PURPLE, "\033[38;5;141m" }, { TerminalColors::BLACK, "\033[30m" }, { TerminalColors::GREY, "\033[90m" }, { TerminalColors::DARK_GRAY, "\033[37m" }, { TerminalColors::WHITE, "\033[97m" }, { TerminalColors::TRANSPARENT, "\033[0m" }
};
inline static const std::unordered_map<TerminalColors, std::unordered_map<TerminalColors, TerminalColors>> terminal_color_combinations = {
    { TerminalColors::NONE_SPECIFIED, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::NONE_SPECIFIED }, { TerminalColors::DEFAULT, TerminalColors::DEFAULT }, { TerminalColors::DARK_RED, TerminalColors::DARK_RED }, { TerminalColors::RED, TerminalColors::RED }, { TerminalColors::BRIGHT_RED, TerminalColors::BRIGHT_RED }, { TerminalColors::DARK_ORANGE, TerminalColors::DARK_ORANGE }, { TerminalColors::ORANGE, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DARK_YELLOW, TerminalColors::DARK_YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::DARK_GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::BRIGHT_GREEN }, { TerminalColors::DARK_CYAN, TerminalColors::DARK_CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::BRIGHT_CYAN }, { TerminalColors::DARK_BLUE, TerminalColors::DARK_BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::BRIGHT_BLUE }, { TerminalColors::DARK_MAGENTA, TerminalColors::DARK_MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::DARK_PURPLE, TerminalColors::DARK_PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BLACK, TerminalColors::BLACK }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::TRANSPARENT }
    }}, { TerminalColors::DEFAULT, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DEFAULT }, { TerminalColors::DEFAULT, TerminalColors::DEFAULT }, { TerminalColors::DARK_RED, TerminalColors::DARK_RED }, { TerminalColors::RED, TerminalColors::RED }, { TerminalColors::BRIGHT_RED, TerminalColors::BRIGHT_RED }, { TerminalColors::DARK_ORANGE, TerminalColors::DARK_ORANGE }, { TerminalColors::ORANGE, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DARK_YELLOW, TerminalColors::DARK_YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::DARK_GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::BRIGHT_GREEN }, { TerminalColors::DARK_CYAN, TerminalColors::DARK_CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::BRIGHT_CYAN }, { TerminalColors::DARK_BLUE, TerminalColors::DARK_BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::BRIGHT_BLUE }, { TerminalColors::DARK_MAGENTA, TerminalColors::DARK_MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::DARK_PURPLE, TerminalColors::DARK_PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BLACK, TerminalColors::BLACK }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DEFAULT }
    }}, { TerminalColors::DARK_RED, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_RED }, { TerminalColors::DEFAULT, TerminalColors::DARK_RED }, { TerminalColors::DARK_RED, TerminalColors::DARK_RED }, { TerminalColors::RED, TerminalColors::RED }, { TerminalColors::BRIGHT_RED, TerminalColors::BRIGHT_RED }, { TerminalColors::DARK_ORANGE, TerminalColors::DARK_ORANGE }, { TerminalColors::ORANGE, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DARK_YELLOW, TerminalColors::DARK_YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::DARK_GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::BRIGHT_GREEN }, { TerminalColors::DARK_CYAN, TerminalColors::DARK_CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::BRIGHT_CYAN }, { TerminalColors::DARK_BLUE, TerminalColors::DARK_BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::BRIGHT_BLUE }, { TerminalColors::DARK_MAGENTA, TerminalColors::DARK_MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::DARK_PURPLE, TerminalColors::DARK_PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BLACK, TerminalColors::BLACK }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_RED }
    } }, { TerminalColors::RED, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::RED }, { TerminalColors::DEFAULT, TerminalColors::RED }, { TerminalColors::DARK_RED, TerminalColors::RED }, { TerminalColors::RED, TerminalColors::BRIGHT_RED }, { TerminalColors::BRIGHT_RED, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_ORANGE, TerminalColors::ORANGE }, { TerminalColors::ORANGE, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::BRIGHT_CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::RED }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::RED }
    }}, { TerminalColors::BRIGHT_RED, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_RED }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_RED }, { TerminalColors::DARK_RED, TerminalColors::BRIGHT_RED }, { TerminalColors::RED, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::ORANGE }, { TerminalColors::ORANGE, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::RED }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_RED }
    } }, { TerminalColors::DARK_ORANGE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_ORANGE }, { TerminalColors::DEFAULT, TerminalColors::DARK_ORANGE }, { TerminalColors::DARK_RED, TerminalColors::DARK_ORANGE }, { TerminalColors::RED, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_RED, TerminalColors::ORANGE }, { TerminalColors::DARK_ORANGE, TerminalColors::ORANGE }, { TerminalColors::ORANGE, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_ORANGE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_ORANGE }
    } }, { TerminalColors::ORANGE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::ORANGE }, { TerminalColors::DEFAULT, TerminalColors::ORANGE }, { TerminalColors::DARK_RED, TerminalColors::ORANGE }, { TerminalColors::RED, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_RED, TerminalColors::ORANGE }, { TerminalColors::DARK_ORANGE, TerminalColors::ORANGE }, { TerminalColors::ORANGE, TerminalColors::ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::ORANGE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::ORANGE }
    } }, { TerminalColors::BRIGHT_ORANGE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::DARK_RED, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::ORANGE, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_ORANGE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_ORANGE }
    } }, { TerminalColors::DARK_YELLOW, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_YELLOW }, { TerminalColors::DEFAULT, TerminalColors::DARK_YELLOW }, { TerminalColors::DARK_RED, TerminalColors::DARK_YELLOW }, { TerminalColors::RED, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_RED, TerminalColors::YELLOW }, { TerminalColors::DARK_ORANGE, TerminalColors::YELLOW }, { TerminalColors::ORANGE, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_YELLOW }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_YELLOW }
    } }, { TerminalColors::YELLOW, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::YELLOW }, { TerminalColors::DEFAULT, TerminalColors::YELLOW }, { TerminalColors::DARK_RED, TerminalColors::YELLOW }, { TerminalColors::RED, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_RED, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_ORANGE, TerminalColors::YELLOW }, { TerminalColors::ORANGE, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::YELLOW }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::YELLOW }
    } }, { TerminalColors::BRIGHT_YELLOW, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_RED, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::ORANGE, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_YELLOW }
    } }, { TerminalColors::DARK_GREEN, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_GREEN }, { TerminalColors::DEFAULT, TerminalColors::DARK_GREEN }, { TerminalColors::DARK_RED, TerminalColors::DARK_ORANGE }, { TerminalColors::RED, TerminalColors::BLUE }, { TerminalColors::BRIGHT_RED, TerminalColors::CYAN }, { TerminalColors::DARK_ORANGE, TerminalColors::GREEN }, { TerminalColors::ORANGE, TerminalColors::GREEN }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::GREEN }, { TerminalColors::YELLOW, TerminalColors::GREEN }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::DARK_GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_GREEN }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_GREEN }
    } }, { TerminalColors::GREEN, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::GREEN }, { TerminalColors::DEFAULT, TerminalColors::GREEN }, { TerminalColors::DARK_RED, TerminalColors::GREEN }, { TerminalColors::RED, TerminalColors::CYAN }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::GREEN }, { TerminalColors::ORANGE, TerminalColors::GREEN }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::GREEN }, { TerminalColors::YELLOW, TerminalColors::GREEN }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::GREEN }, { TerminalColors::GREEN, TerminalColors::BRIGHT_GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::GREEN }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::GREEN }
    } }, { TerminalColors::BRIGHT_GREEN, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_GREEN }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_GREEN }, { TerminalColors::DARK_RED, TerminalColors::WHITE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::WHITE }, { TerminalColors::ORANGE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::WHITE }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_GREEN }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_GREEN }
    } }, { TerminalColors::DARK_CYAN, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_CYAN }, { TerminalColors::DEFAULT, TerminalColors::DARK_CYAN }, { TerminalColors::DARK_RED, TerminalColors::CYAN }, { TerminalColors::RED, TerminalColors::CYAN }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::CYAN }, { TerminalColors::ORANGE, TerminalColors::CYAN }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::CYAN }, { TerminalColors::YELLOW, TerminalColors::CYAN }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::CYAN }, { TerminalColors::GREEN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::DARK_CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_CYAN }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_CYAN }
    } }, { TerminalColors::CYAN, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::CYAN }, { TerminalColors::DEFAULT, TerminalColors::CYAN }, { TerminalColors::DARK_RED, TerminalColors::CYAN }, { TerminalColors::RED, TerminalColors::CYAN }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::CYAN }, { TerminalColors::ORANGE, TerminalColors::CYAN }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::CYAN }, { TerminalColors::YELLOW, TerminalColors::CYAN }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::CYAN }, { TerminalColors::GREEN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::CYAN }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::CYAN }
    } }, { TerminalColors::BRIGHT_CYAN, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_CYAN }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_CYAN }, { TerminalColors::DARK_RED, TerminalColors::WHITE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::WHITE }, { TerminalColors::ORANGE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::WHITE }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_CYAN }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_CYAN }
    } }, { TerminalColors::DARK_BLUE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_BLUE }, { TerminalColors::DEFAULT, TerminalColors::DARK_BLUE }, { TerminalColors::DARK_RED, TerminalColors::BLUE }, { TerminalColors::RED, TerminalColors::BLUE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::BLUE }, { TerminalColors::ORANGE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::BLUE }, { TerminalColors::YELLOW, TerminalColors::BLUE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::BLUE }, { TerminalColors::GREEN, TerminalColors::BLUE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::BLUE }, { TerminalColors::CYAN, TerminalColors::BLUE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::DARK_BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_BLUE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_BLUE }
    } }, { TerminalColors::BLUE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BLUE }, { TerminalColors::DEFAULT, TerminalColors::BLUE }, { TerminalColors::DARK_RED, TerminalColors::BLUE }, { TerminalColors::RED, TerminalColors::BLUE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::BLUE }, { TerminalColors::ORANGE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::BLUE }, { TerminalColors::YELLOW, TerminalColors::BLUE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::BLUE }, { TerminalColors::GREEN, TerminalColors::BLUE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::BLUE }, { TerminalColors::CYAN, TerminalColors::BLUE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BLUE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BLUE }
    } }, { TerminalColors::BRIGHT_BLUE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_BLUE }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_BLUE }, { TerminalColors::DARK_RED, TerminalColors::WHITE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::WHITE }, { TerminalColors::ORANGE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::WHITE }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_BLUE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_BLUE }
    } }, { TerminalColors::DARK_MAGENTA, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_MAGENTA }, { TerminalColors::DEFAULT, TerminalColors::DARK_MAGENTA }, { TerminalColors::DARK_RED, TerminalColors::MAGENTA }, { TerminalColors::RED, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::MAGENTA }, { TerminalColors::ORANGE, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::MAGENTA }, { TerminalColors::YELLOW, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::MAGENTA }, { TerminalColors::GREEN, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::MAGENTA }, { TerminalColors::CYAN, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::MAGENTA }, { TerminalColors::BLUE, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::DARK_MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_MAGENTA }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_MAGENTA }
    } }, { TerminalColors::MAGENTA, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::MAGENTA }, { TerminalColors::DEFAULT, TerminalColors::MAGENTA }, { TerminalColors::DARK_RED, TerminalColors::MAGENTA }, { TerminalColors::RED, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::MAGENTA }, { TerminalColors::ORANGE, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::MAGENTA }, { TerminalColors::YELLOW, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::MAGENTA }, { TerminalColors::GREEN, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::MAGENTA }, { TerminalColors::CYAN, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::MAGENTA }, { TerminalColors::BLUE, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::MAGENTA }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::MAGENTA }
    } }, { TerminalColors::BRIGHT_MAGENTA, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::DARK_RED, TerminalColors::WHITE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::WHITE }, { TerminalColors::ORANGE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::WHITE }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_MAGENTA }
    } }, { TerminalColors::DARK_PURPLE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_PURPLE }, { TerminalColors::DEFAULT, TerminalColors::DARK_PURPLE }, { TerminalColors::DARK_RED, TerminalColors::PURPLE }, { TerminalColors::RED, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::PURPLE }, { TerminalColors::ORANGE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::PURPLE }, { TerminalColors::YELLOW, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::PURPLE }, { TerminalColors::GREEN, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::PURPLE }, { TerminalColors::CYAN, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::PURPLE }, { TerminalColors::BLUE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::DARK_PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::DARK_PURPLE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::DARK_PURPLE }
    } }, { TerminalColors::PURPLE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::PURPLE }, { TerminalColors::DEFAULT, TerminalColors::PURPLE }, { TerminalColors::DARK_RED, TerminalColors::PURPLE }, { TerminalColors::RED, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::PURPLE }, { TerminalColors::ORANGE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::PURPLE }, { TerminalColors::YELLOW, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::PURPLE }, { TerminalColors::GREEN, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::PURPLE }, { TerminalColors::CYAN, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::PURPLE }, { TerminalColors::BLUE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::PURPLE }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::PURPLE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::PURPLE }
    } }, { TerminalColors::BRIGHT_PURPLE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::DEFAULT, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::DARK_RED, TerminalColors::WHITE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::WHITE }, { TerminalColors::ORANGE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::WHITE }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::BRIGHT_PURPLE }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::BRIGHT_PURPLE }
    } }, { TerminalColors::BLACK, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::BLACK }, { TerminalColors::DEFAULT, TerminalColors::BLACK }, { TerminalColors::DARK_RED, TerminalColors::GREY }, { TerminalColors::RED, TerminalColors::GREY }, { TerminalColors::BRIGHT_RED, TerminalColors::GREY }, { TerminalColors::DARK_ORANGE, TerminalColors::GREY }, { TerminalColors::ORANGE, TerminalColors::GREY }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::GREY }, { TerminalColors::DARK_YELLOW, TerminalColors::GREY }, { TerminalColors::YELLOW, TerminalColors::GREY }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::GREY }, { TerminalColors::DARK_GREEN, TerminalColors::GREY }, { TerminalColors::GREEN, TerminalColors::GREY }, { TerminalColors::BRIGHT_GREEN, TerminalColors::GREY }, { TerminalColors::DARK_CYAN, TerminalColors::GREY }, { TerminalColors::CYAN, TerminalColors::GREY }, { TerminalColors::BRIGHT_CYAN, TerminalColors::GREY }, { TerminalColors::DARK_BLUE, TerminalColors::GREY }, { TerminalColors::BLUE, TerminalColors::GREY }, { TerminalColors::BRIGHT_BLUE, TerminalColors::GREY }, { TerminalColors::DARK_MAGENTA, TerminalColors::GREY }, { TerminalColors::MAGENTA, TerminalColors::GREY }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::GREY }, { TerminalColors::DARK_PURPLE, TerminalColors::GREY }, { TerminalColors::PURPLE, TerminalColors::GREY }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::GREY }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::GREY }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::GREY }
    } }, { TerminalColors::GREY, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::GREY }, { TerminalColors::DEFAULT, TerminalColors::GREY }, { TerminalColors::DARK_RED, TerminalColors::RED }, { TerminalColors::RED, TerminalColors::BRIGHT_RED }, { TerminalColors::BRIGHT_RED, TerminalColors::BRIGHT_RED }, { TerminalColors::DARK_ORANGE, TerminalColors::ORANGE }, { TerminalColors::ORANGE, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::YELLOW }, { TerminalColors::DARK_YELLOW, TerminalColors::YELLOW }, { TerminalColors::YELLOW, TerminalColors::YELLOW }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::BRIGHT_YELLOW }, { TerminalColors::DARK_GREEN, TerminalColors::DARK_GREEN }, { TerminalColors::GREEN, TerminalColors::GREEN }, { TerminalColors::BRIGHT_GREEN, TerminalColors::BRIGHT_GREEN }, { TerminalColors::DARK_CYAN, TerminalColors::CYAN }, { TerminalColors::CYAN, TerminalColors::CYAN }, { TerminalColors::BRIGHT_CYAN, TerminalColors::BRIGHT_CYAN }, { TerminalColors::DARK_BLUE, TerminalColors::DARK_BLUE }, { TerminalColors::BLUE, TerminalColors::BLUE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::BRIGHT_BLUE }, { TerminalColors::DARK_MAGENTA, TerminalColors::DARK_MAGENTA }, { TerminalColors::MAGENTA, TerminalColors::MAGENTA }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::BRIGHT_MAGENTA }, { TerminalColors::PURPLE, TerminalColors::PURPLE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::DARK_GRAY }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::WHITE }, { TerminalColors::TRANSPARENT, TerminalColors::GREY }
    } }, { TerminalColors::WHITE, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::WHITE }, { TerminalColors::DEFAULT, TerminalColors::WHITE }, { TerminalColors::DARK_RED, TerminalColors::WHITE }, { TerminalColors::RED, TerminalColors::WHITE }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::WHITE }, { TerminalColors::ORANGE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::WHITE }, { TerminalColors::YELLOW, TerminalColors::WHITE }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::WHITE }, { TerminalColors::GREEN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::WHITE }, { TerminalColors::CYAN, TerminalColors::WHITE }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::WHITE }, { TerminalColors::BLUE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::WHITE }, { TerminalColors::MAGENTA, TerminalColors::WHITE }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::WHITE }, { TerminalColors::PURPLE, TerminalColors::WHITE }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::WHITE }
    } }, { TerminalColors::TRANSPARENT, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::TRANSPARENT }, { TerminalColors::DEFAULT, TerminalColors::TRANSPARENT }, { TerminalColors::DARK_RED, TerminalColors::TRANSPARENT }, { TerminalColors::RED, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_RED, TerminalColors::WHITE }, { TerminalColors::DARK_ORANGE, TerminalColors::TRANSPARENT }, { TerminalColors::ORANGE, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::WHITE }, { TerminalColors::DARK_YELLOW, TerminalColors::TRANSPARENT }, { TerminalColors::YELLOW, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::WHITE }, { TerminalColors::DARK_GREEN, TerminalColors::TRANSPARENT }, { TerminalColors::GREEN, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_GREEN, TerminalColors::WHITE }, { TerminalColors::DARK_CYAN, TerminalColors::TRANSPARENT }, { TerminalColors::CYAN, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_CYAN, TerminalColors::WHITE }, { TerminalColors::DARK_BLUE, TerminalColors::TRANSPARENT }, { TerminalColors::BLUE, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_BLUE, TerminalColors::WHITE }, { TerminalColors::DARK_MAGENTA, TerminalColors::TRANSPARENT }, { TerminalColors::MAGENTA, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::WHITE }, { TerminalColors::DARK_PURPLE, TerminalColors::TRANSPARENT }, { TerminalColors::PURPLE, TerminalColors::TRANSPARENT }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::WHITE }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::TRANSPARENT }
    } }, { TerminalColors::DARK_GRAY, {
        { TerminalColors::NONE_SPECIFIED, TerminalColors::DARK_GRAY }, { TerminalColors::DEFAULT, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_RED, TerminalColors::DARK_GRAY }, { TerminalColors::RED, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_RED, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_ORANGE, TerminalColors::DARK_GRAY }, { TerminalColors::ORANGE, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_ORANGE, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_YELLOW, TerminalColors::DARK_GRAY }, { TerminalColors::YELLOW, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_YELLOW, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_GREEN, TerminalColors::DARK_GRAY }, { TerminalColors::GREEN, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_GREEN, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_CYAN, TerminalColors::DARK_GRAY }, { TerminalColors::CYAN, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_CYAN, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_BLUE, TerminalColors::DARK_GRAY }, { TerminalColors::BLUE, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_BLUE, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_MAGENTA, TerminalColors::DARK_GRAY }, { TerminalColors::MAGENTA, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_MAGENTA, TerminalColors::DARK_GRAY }, { TerminalColors::DARK_PURPLE, TerminalColors::DARK_GRAY }, { TerminalColors::PURPLE, TerminalColors::DARK_GRAY }, { TerminalColors::BRIGHT_PURPLE, TerminalColors::DARK_GRAY }, { TerminalColors::BLACK, TerminalColors::GREY }, { TerminalColors::GREY, TerminalColors::GREY }, { TerminalColors::WHITE, TerminalColors::WHITE }, { TerminalColors::DARK_GRAY, TerminalColors::DARK_GRAY }, { TerminalColors::TRANSPARENT, TerminalColors::GREY }
    } }
};
inline void encode_utf8(uint32_t cp, std::ostream& out) {
    if (cp < 0x80u) {
        out.put(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.put(static_cast<char>(0xC0u | (cp >> 6)));
        out.put(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.put(static_cast<char>(0xE0u | (cp >> 12)));
        out.put(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.put(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.put(static_cast<char>(0xF0u | (cp >> 18)));
        out.put(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.put(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.put(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}
struct TerminalVelocity;
struct alignas(16) TerminalPosition {
    double column = 0;
    double row = 0;
    TerminalPosition(bool capture_now = true) {
        if (capture_now) {
            capture();
        } else {
            column = 0;
            row = 0;
        }
    }
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    TerminalPosition(T _column, T _row) :
        column(static_cast<double>(_column)), row(static_cast<double>(_row)) {}
    bool operator==(const TerminalPosition& other) const {
        return (other.column == column) && (other.row == row);
    }
    bool operator!=(const TerminalPosition& other) const {
        return !(*this == other);
    }
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    TerminalPosition& operator+=(T x) {
        column += x;
        return *this;
    }
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    TerminalPosition& operator-=(T x) {
        column -= x;
        return *this;
    }
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    TerminalPosition& operator*=(T y) {
        row += y;
        return *this;
    }
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    TerminalPosition& operator/=(T y) {
        row -= y;
        return *this;
    }
    void capture() {
        column = get_terminal_current_column();
        row = get_terminal_current_row();
    }
    void go_to() {
        move_to(*this);
    }
    TerminalPosition go_to_save_old() {
        return move_to_save_old(*this);
    }
    static void move_to(const TerminalPosition& pos) {
        std::cout << "\033[" << (pos.row + 1) << ";" << (pos.column + 1) << "H";
    }
    static TerminalPosition move_to_save_old(
        const TerminalPosition& pos
    ) {
        TerminalPosition original_pos(true);
        std::cout << "\033[" << (pos.row + 1) << ";" << (pos.column + 1) << "H";
        return original_pos;
    }
    int x_pos() const {
        return static_cast<int>(std::floor(column / 2.0));
    }
    int y_pos() const {
        return static_cast<int>(std::floor(row / 4.0));
    }
    bool on_screen(TerminalScreen* screen) const;
    void apply_velocity(const TerminalVelocity& velocity, double delta_time);
    friend TerminalPosition operator+(const TerminalPosition& pos, const double& x) {
        TerminalPosition result = pos;
        result += x;
        return result;
    }
};
struct alignas(16) TerminalVelocity {
    double x = 0;
    double y = 0;
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    TerminalVelocity(T _x = 0.0, T _y = 0.0) :
        x(static_cast<double>(_x)), y(static_cast<double>(_y)) {}
    TerminalVelocity& operator+=(const TerminalVelocity& other) {
        x += other.x;
        y += other.y;
        return *this;
    }
    TerminalVelocity& operator-=(const TerminalVelocity& other) {
        x -= other.x;
        y -= other.y;
        return *this;
    }
    TerminalVelocity& operator*=(const TerminalVelocity& other) {
        x *= other.x;
        y *= other.y;
        return *this;
    }
    void dampen(double factor) {
        x *= factor;
        y *= factor;
    }
    void gravitate_towards(
        const TerminalPosition& current_position,
        const TerminalPosition& target_position,
        double mass,
        double delta_time
    ) {
        double dx = target_position.column - current_position.column;
        double dy = target_position.row - current_position.row;
        double distance_squared = dx * dx + dy * dy;
        double distance = std::sqrt(distance_squared);
        double G = LOGGING_G();
        double accel = G * mass / distance_squared;
        if (accel > 1000) {
            if (distance < 0.1) {
                TerminalVelocity repulsion_force(-dx * 800, -dy * 800);
                *this += repulsion_force;
            } else if (distance > 10.0) {
                TerminalVelocity attraction_force(dx * 800, dy * 800);
                *this += attraction_force;
            }
            accel = 1000;
        }
        x += accel * dx * delta_time;
        y += accel * dy * delta_time;
    }
    static double& TTY_GRAVITY_CONSTANT_MULTIPLIER() {
        static double multiplier = 1.6;
        return multiplier;
    }
    static double LOGGING_G() {
        return 500.0 * TTY_GRAVITY_CONSTANT_MULTIPLIER();
    }
    template<typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0,
            typename U, std::enable_if_t<std::is_arithmetic_v<U>, int> = 0,
            typename V, std::enable_if_t<std::is_arithmetic_v<V>, int> = 0,
            typename W, std::enable_if_t<std::is_arithmetic_v<W>, int> = 0,
            typename X, std::enable_if_t<std::is_arithmetic_v<X>, int> = 0,
            typename Y, std::enable_if_t<std::is_arithmetic_v<Y>, int> = 0>
    static std::pair<TerminalVelocity, TerminalPosition> orbital_velocity_of(
        const TerminalPosition& stationary_object_position,
        T starting_distance,
        U starting_angle_radians,
        V elipse_ratio,
        W radius,
        X object_mass = 1.0,
        Y decay_factor = 1.0
    ) {
        const double G = LOGGING_G();
        double angle_radians = starting_angle_radians;
        double adjusted_radius = radius * elipse_ratio;
        double velocity_magnitude =
            std::sqrt(G * object_mass / adjusted_radius) * decay_factor;
        double vx = -velocity_magnitude * std::sin(angle_radians);
        double vy = velocity_magnitude * std::cos(angle_radians);
        TerminalVelocity velocity(vx, vy);
        TerminalPosition position(
            stationary_object_position.column + starting_distance *
                std::cos(angle_radians),
            stationary_object_position.row + starting_distance *
                std::sin(angle_radians)
        );
        return {velocity, position};
    }
    double slow_decay(
        TerminalPosition& current_position,
        double mass,
        TerminalPosition& target_position
    ) {
        double dx = target_position.column - current_position.column;
        double dy = target_position.row - current_position.row;
        double distance = std::sqrt(dx * dx + dy * dy);
        double current_decay = (distance > 0) ? (mass / (distance * distance)) : 0;
        if (current_decay > 1.0) {
            double decay_factor = 3.0 / current_decay;
            decay_factor = 1.0 - (1.0 - decay_factor) * 0.1;
            x *= decay_factor;
            y *= decay_factor;
        }
        return current_decay;
    }
    friend TerminalVelocity& operator*(const TerminalVelocity& lhs, const double& rhs) {
        TerminalVelocity* result = new TerminalVelocity(lhs);
        result->x *= rhs;
        result->y *= rhs;
        return *result;
    }
};
inline static uint32_t decode_utf8_one(const unsigned char*& p, const unsigned char* end) {
    if (p >= end) return 0;
    const unsigned char b0 = *p;
    if (b0 < 0x80u) {
        ++p;
        return static_cast<uint32_t>(b0);
    }
    uint32_t cp;
    size_t need;
    if ((b0 & 0xE0u) == 0xC0u) {
        cp = b0 & 0x1Fu;
        need = 1;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        cp = b0 & 0x0Fu;
        need = 2;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        cp = b0 & 0x07u;
        need = 3;
    } else {
        ++p;
        return 0xFFFDu;
    }
    if (p + need >= end) {
        ++p;
        return 0xFFFDu;
    }
    for (size_t off = 1; off <= need; ++off) {
        const unsigned char c = p[off];
        if ((c & 0xC0u) != 0x80u) {
            ++p;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    p += need + 1;
    return cp;
}
inline static std::unique_ptr<uint32_t[]> decode_utf8(const std::string& s, size_t& count_out) {
    auto out = std::make_unique<uint32_t[]>(s.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* const end = p + s.size();
    size_t n = 0;
    while (p < end) out[n++] = decode_utf8_one(p, end);
    count_out = n;
    return out;
}
/** --------------------------------------------------------------------------------------------------------- Braille Point Struct
 * @struct BraillePoint
 * @brief Represents a single point in a 2x4 grid that corresponds to a braille character.
 */
struct BraillePoint {
    TerminalPosition position = TerminalPosition(0.0, 0.0);
    TerminalVelocity velocity = TerminalVelocity(0.0, 0.0);
    uint8_t point_value = 0;
    TerminalColors color = TerminalColors::DEFAULT;
    double orbit_r = 0;             // radius of the 3D circular orbit (half-cells)
    double orbit_tilt = 0;          // tilt of orbit plane (radians). π/2 = edge-on
    double orbit_tilt_axis = 0;     // direction of tilt hinge in XY (radians)
    double orbit_speed = 0;         // angular velocity (radians/sec)
    double orbit_phase = 0;         // starting angle (radians)
    double birth_time_sec = 0;      // wall-clock seconds when spawned
    static inline const std::string BRAILLE_CHARS[256] = {
        " ", "⡀", "⢀", "⣀", "⠄", "⡄", "⢄", "⣄", "⠠", "⡠", "⢠", "⣠", "⠤", "⡤", "⢤", "⣤",
        "⠂", "⡂", "⢂", "⣂", "⠆", "⡆", "⢆", "⣆", "⠢", "⡢", "⢢", "⣢", "⠦", "⡦", "⢦", "⣦",
        "⠐", "⡐", "⢐", "⣐", "⠔", "⡔", "⢔", "⣔", "⠰", "⡰", "⢰", "⣰", "⠴", "⡴", "⢴", "⣴",
        "⠒", "⡒", "⢒", "⣒", "⠖", "⡖", "⢖", "⣖", "⠲", "⡲", "⢲", "⣲", "⠶", "⡶", "⢶", "⣶",
        "⠁", "⡁", "⢁", "⣁", "⠅", "⡅", "⢅", "⣅", "⠡", "⡡", "⢡", "⣡", "⠥", "⡥", "⢥", "⣥",
        "⠃", "⡃", "⢃", "⣃", "⠇", "⡇", "⢇", "⣇", "⠣", "⡣", "⢣", "⣣", "⠧", "⡧", "⢧", "⣧",
        "⠑", "⡑", "⢑", "⣑", "⠕", "⡕", "⢕", "⣕", "⠱", "⡱", "⢱", "⣱", "⠵", "⡵", "⢵", "⣵",
        "⠓", "⡓", "⢓", "⣓", "⠗", "⡗", "⢗", "⣗", "⠳", "⡳", "⢳", "⣳", "⠷", "⡷", "⢷", "⣷",
        "⠈", "⡈", "⢈", "⣈", "⠌", "⡌", "⢌", "⣌", "⠨", "⡨", "⢨", "⣨", "⠬", "⡬", "⢬", "⣬",
        "⠊", "⡊", "⢊", "⣊", "⠎", "⡎", "⢎", "⣎", "⠪", "⡪", "⢪", "⣪", "⠮", "⡮", "⢮", "⣮",
        "⠘", "⡘", "⢘", "⣘", "⠜", "⡜", "⢜", "⣜", "⠸", "⡸", "⢸", "⣸", "⠼", "⡼", "⢼", "⣼",
        "⠚", "⡚", "⢚", "⣚", "⠞", "⡞", "⢞", "⣞", "⠺", "⡺", "⢺", "⣺", "⠾", "⡾", "⢾", "⣾",
        "⠉", "⡉", "⢉", "⣉", "⠍", "⡍", "⢍", "⣍", "⠩", "⡩", "⢩", "⣩", "⠭", "⡭", "⢭", "⣭",
        "⠋", "⡋", "⢋", "⣋", "⠏", "⡏", "⢏", "⣏", "⠫", "⡫", "⢫", "⣫", "⠯", "⡯", "⢯", "⣯",
        "⠙", "⡙", "⢙", "⣙", "⠝", "⡝", "⢝", "⣝", "⠹", "⡹", "⢹", "⣹", "⠽", "⡽", "⢽", "⣽",
        "⠛", "⡛", "⢛", "⣛", "⠟", "⡟", "⢟", "⣟", "⠻", "⡻", "⢻", "⣻", "⠿", "⡿", "⢿", "⣿"
    };
    static constexpr uint8_t unicode_to_custom(uint8_t u) {
        uint8_t r = 0;
        if (u & 0x01) { r |= 0x40; }
        if (u & 0x02) { r |= 0x10; }
        if (u & 0x04) { r |= 0x04; }
        if (u & 0x08) { r |= 0x80; }
        if (u & 0x10) { r |= 0x20; }
        if (u & 0x20) { r |= 0x08; }
        if (u & 0x40) { r |= 0x01; }
        if (u & 0x80) { r |= 0x02; }
        return r;
    }
    static constexpr uint8_t custom_to_unicode(uint8_t c) {
        uint8_t r = 0;
        if (c & 0x40) { r |= 0x01; }
        if (c & 0x10) { r |= 0x02; }
        if (c & 0x04) { r |= 0x04; }
        if (c & 0x80) { r |= 0x08; }
        if (c & 0x20) { r |= 0x10; }
        if (c & 0x08) { r |= 0x20; }
        if (c & 0x01) { r |= 0x40; }
        if (c & 0x02) { r |= 0x80; }
        return r;
    }
    static TerminalColors combine_colors(TerminalColors c1, TerminalColors c2) {
        auto first_query = terminal_color_to_ansi_code.find(c1);
        if (first_query == terminal_color_to_ansi_code.end()) {
            return c2;
        }
        auto second_query = terminal_color_to_ansi_code.find(c2);
        if (second_query == terminal_color_to_ansi_code.end()) {
            return c1;
        }
        return terminal_color_combinations.at(c1).at(c2);
    }
    struct CombinedPoint {
        uint8_t combined_value;
        char char_instead = ' ';
        TerminalColors color;
        CombinedPoint& operator+=(const CombinedPoint& point) {
            combined_value |= point.combined_value;
            color = combine_colors(color, point.color);
            return *this;
        }
        CombinedPoint intersection(const CombinedPoint& other) const {
            CombinedPoint result;
            result.combined_value = combined_value & other.combined_value;
            if (result.combined_value != 0) {
                result.color = combine_colors(color, other.color);
            } else {
                result.color = TerminalColors::TRANSPARENT;
            }
            return result;
        }
    };
};
inline TerminalColors parse_sgr(const char* params, size_t plen, TerminalColors current);
/** --------------------------------------------------------------------------------------------------------- Braille Sprite Struct
 * @struct BrailleSprite
 * @brief Represents a sprite that can be rendered using braille characters in the terminal.
 */
struct alignas(64) BrailleSprite {
    double column = 0.0;
    double row = 0.0;
    std::string* chars = nullptr;
    std::string* mask = nullptr;
    TerminalColors* colors = nullptr;
    double mass = 1.0;
    TerminalVelocity velocity = TerminalVelocity(0.0, 0.0);
    static inline const std::string BRAILLE_ALPHABET[26] = {
        "⢀⣀\n⠣⠼", "⣇⡀\n⠧⠜", "⢀⣀\n⠣⠤", "⢀⣸\n⠣⠼", "⢀⡀\n⠣⠭", "⣰⡁\n⢸", "⢀⡀\n⣑⡺",
        "⣇⡀\n⠇⠸", "⠄\n⠇", "⠠\n⡸", "⡇⡠\n⠏⠢", "⡇\n⠣", "⣀⣀\n⠇⠇⠇", "⣀⡀\n⠇⠸", "⢀⡀\n⠣⠜",
        "⣀⡀\n⡧⠜", "⣀⡀\n⠣⢼", "⢀⣀\n⠏", "⡀⣀\n⠭⠕", "⣰⡀\n⠘⠤", "⡀⢀\n⠣⠼", "⡀⢀\n⠱⠃",
        "⡀ ⢀\n⠱⠱⠃", "⡀⢀\n⠜⠣", "⡀⢀\n⣑⡺", "⣀⣀\n⠴⠥"
    };
    static inline const std::string BRAILLE_ALPHABET_MASKS[26] = {
        "##\n##", "##\n##", "##\n##", "##\n##", "##\n##", "##\n#", "##\n##",
        "##\n##", "#\n#", "#\n#", "##\n##", "#\n#", "##\n###", "##\n##", "##\n##",
        "##\n##", "##\n##", "##\n#", "##\n##", "##\n##", "##\n##", "##\n##",
        "# #\n###", "##\n##", "##\n##", "##\n##"
    };
    static std::vector<std::vector<std::array<BraillePoint::CombinedPoint, 2>>> get_text(
        const std::string& text,
        TerminalColors default_color = TerminalColors::GREY
    ) {
        std::vector<std::vector<std::array<BraillePoint::CombinedPoint, 2>>> result;
        size_t letters = 0;
        for (char c : text) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                ++letters;
            }
        }
        result.resize(letters);
        size_t letter_index = 0;
        for (char c : text) {
            std::vector<std::array<BraillePoint::CombinedPoint, 2>> char_points;
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            } else if (c < 'a' || c > 'z') {
                continue;
            }
            if (c == 'i' || c == 'j' || c == 'l') {
                char_points.resize(1 + 3);
            } else if (c == 'w' || c == 'm') {
                char_points.resize(3 + 3);
            } else {
                char_points.resize(2 + 3);
            }
            const std::string& chars_row = BRAILLE_ALPHABET[c - 'a'];
            const size_t nl = chars_row.find("\n");
            const std::string top_row = chars_row.substr(0, nl);
            const std::string bottom_row = (nl == std::string::npos)
                ? std::string() : chars_row.substr(nl + 1);
            size_t cidx_top = 0, cidx_bot = 0;
            for (size_t column = 0; column < char_points.size(); ++column) {
                bool top_consumed = false;
                while (cidx_top < top_row.size() && !top_consumed) {
                    if (top_row[cidx_top] == ' ') {
                        ++cidx_top;
                        top_consumed = true;
                        break;
                    }
                    if (cidx_top + 2 < top_row.size()
                        && static_cast<uint8_t>(top_row[cidx_top]) == 0xE2) {
                        uint8_t b1 = static_cast<uint8_t>(top_row[cidx_top + 1]);
                        uint8_t b2 = static_cast<uint8_t>(top_row[cidx_top + 2]);
                        uint8_t unicode_low = static_cast<uint8_t>(
                            ((b1 & 0x3F) << 6) | (b2 & 0x3F));
                        char_points[column][0].combined_value =
                            BraillePoint::unicode_to_custom(unicode_low);
                        char_points[column][0].color = default_color;
                        cidx_top += 3;
                        top_consumed = true;
                        break;
                    }
                    ++cidx_top;
                }
                bool bot_consumed = false;
                while (cidx_bot < bottom_row.size() && !bot_consumed) {
                    if (bottom_row[cidx_bot] == ' ') {
                        ++cidx_bot;
                        bot_consumed = true;
                        break;
                    }
                    if (cidx_bot + 2 < bottom_row.size()
                        && static_cast<uint8_t>(bottom_row[cidx_bot]) == 0xE2) {
                        uint8_t b1 = static_cast<uint8_t>(bottom_row[cidx_bot + 1]);
                        uint8_t b2 = static_cast<uint8_t>(bottom_row[cidx_bot + 2]);
                        uint8_t unicode_low = static_cast<uint8_t>(
                            ((b1 & 0x3F) << 6) | (b2 & 0x3F));
                        char_points[column][1].combined_value =
                            BraillePoint::unicode_to_custom(unicode_low);
                        char_points[column][1].color = default_color;
                        cidx_bot += 3;
                        bot_consumed = true;
                        break;
                    }
                    ++cidx_bot;
                }
            }
            result[letter_index++] = std::move(char_points);
        }
        return result;
    }
    BrailleSprite() = default;
};
struct alignas(8) TerminalSize {
    int width;
    int height;
    TerminalSize() :
        width(get_terminal_width()),
        height(get_terminal_height()) {}
    TerminalSize(int width, int height) : width(width), height(height) {}
    bool operator==(const TerminalSize& other) const {
        return (other.width == width) && (other.height == height);
    }
    bool operator!=(const TerminalSize& other) const {
        return !(*this == other);
    }
    bool on_screen(const TerminalPosition& pos) const {
        return pos.x_pos() >= 0 && pos.x_pos() < static_cast<int>(width)
            && pos.y_pos() >= 0 && pos.y_pos() < static_cast<int>(height);
    }
};
class SpriteBase {
public:
    virtual ~SpriteBase() = default;
    virtual TerminalSize get_size() const = 0;
    virtual TerminalPosition& position() = 0;
    virtual bool advance_frame(uint64_t epoch_ms) = 0;
    virtual uint32_t* get_buffer() const = 0;
    virtual TerminalColors* get_colors() const = 0;
};
struct TerminalLogMessage : public SpriteBase {
    TerminalPosition position_;
    std::string* message;
    uint32_t* chars = nullptr;
    TerminalColors* colors = nullptr;
    size_t cell_count_ = 0;
    Logger::LogLevel level;
    uint64_t timestamp_ms;
    TerminalLogMessage(const std::string& msg = "", Logger::LogLevel lvl = Logger::LogLevel::INFO) :
        level(lvl), timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()) {
        message = new std::string(msg);
    }
    ~TerminalLogMessage() {
        if (message) {
            delete message;
            message = nullptr;
        }
        delete[] chars;
        delete[] colors;
    }
    //----------------------------------------------------- Non-copyable, but movable.
    TerminalLogMessage(const TerminalLogMessage&) = delete;
    TerminalLogMessage& operator=(const TerminalLogMessage&) = delete;
    TerminalLogMessage(TerminalLogMessage&& other) noexcept :
        message(other.message),
        chars(other.chars),
        colors(other.colors),
        cell_count_(other.cell_count_),
        level(other.level),
        timestamp_ms(other.timestamp_ms) {
        other.message = nullptr;
        other.chars = nullptr;
        other.colors = nullptr;
        other.cell_count_ = 0;
        other.level = Logger::LogLevel::INFO;
        other.timestamp_ms = 0;
    }
    TerminalLogMessage& operator=(TerminalLogMessage&& other) noexcept {
        if (this != &other) {
            delete message;
            delete[] chars;
            delete[] colors;
            message = other.message;
            chars = other.chars;
            colors = other.colors;
            cell_count_ = other.cell_count_;
            level = other.level;
            timestamp_ms = other.timestamp_ms;
            other.message = nullptr;
            other.chars = nullptr;
            other.colors = nullptr;
            other.cell_count_ = 0;
            other.level = Logger::LogLevel::INFO;
            other.timestamp_ms = 0;
        }
        return *this;
    }
    TerminalPosition& position() override {
        return position_;
    }
    TerminalSize get_size() const override {
        return TerminalSize(static_cast<int>(cell_count_), 1);
    }
    uint32_t* get_buffer() const override {
        return chars;
    }
    TerminalColors* get_colors() const override {
        return colors;
    }
    bool advance_frame(uint64_t /*epoch_ms*/) override {
        if (chars != nullptr || message == nullptr) {
            return false;
        }
        size_t n = 0;
        auto decoded = decode_utf8(*message, n);
        chars = decoded.release();
        colors = new TerminalColors[n];
        cell_count_ = n;
        TerminalColors level_color = TerminalColors::DEFAULT;
        switch (level) {
            case Logger::LogLevel::DEBUG: level_color = TerminalColors::CYAN; break;
            case Logger::LogLevel::INFO:  level_color = TerminalColors::GREEN; break;
            case Logger::LogLevel::WARN:  level_color = TerminalColors::YELLOW; break;
            case Logger::LogLevel::ERROR: level_color = TerminalColors::BRIGHT_RED; break;
            case Logger::LogLevel::TRACE: level_color = TerminalColors::BRIGHT_PURPLE; break;
            case Logger::LogLevel::RAW: level_color = TerminalColors::DEFAULT; break;
        }
        for (size_t i = 0; i < n; ++i) {
            colors[i] = level_color;
        }
        return true;
    }
};
class ProgressSprite;
using UpdateProgressCallbackFn = void(*)(ProgressSprite&,float);
inline void poke_compositor();
inline void schedule_wake(uint64_t epoch_ms);
class ProgressSprite : public SpriteBase {
public:
    std::string label;
    uint64_t timestamp_ms = 0;
    UpdateProgressCallbackFn on_progress_update;
    uint64_t last_update_ms = 0;
    uint64_t timeout = 30000;
    float progress_value = 0.0f;
    std::deque<std::pair<uint64_t, float>> rate_samples_;
    static constexpr uint64_t kRateWindowMs = 60'000;
    Logger::LogLevel log_level;
    char style[4] = {'[', '=', '-', ']'};
    std::unique_ptr<std::mutex> progress_mutex = std::make_unique<std::mutex>();
    ProgressSprite(
        const std::string& lbl = "",
        Logger::LogLevel level = Logger::LogLevel::INFO,
        UpdateProgressCallbackFn callback = nullptr,
        uint64_t timeout_ms = 30000
    ) : label(lbl), timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count()), on_progress_update(callback), timeout(timeout_ms), log_level(level) {
        last_update_ms = timestamp_ms;
    }
    //----------------------------------------------------- Non-copyable, but movable.
    ProgressSprite(const ProgressSprite&) = delete;
    ProgressSprite& operator=(const ProgressSprite&) = delete;
    ProgressSprite(ProgressSprite&& other) noexcept : label(std::move(other.label)),
        timestamp_ms(other.timestamp_ms), on_progress_update(other.on_progress_update),
        last_update_ms(other.last_update_ms), timeout(other.timeout),
        progress_value(other.progress_value), log_level(other.log_level) {}
    ProgressSprite& operator=(ProgressSprite&& other) noexcept {
        if (this != &other) {
            label = std::move(other.label);
            timestamp_ms = other.timestamp_ms;
            on_progress_update = other.on_progress_update;
            last_update_ms = other.last_update_ms;
            timeout = other.timeout;
            progress_value = other.progress_value;
            log_level = other.log_level;
        }
        return *this;
    }
    static void default_progress_update(ProgressSprite& bar, float new_progress) {
        bar.progress_value = new_progress;
        bar.last_update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        bar.rate_samples_.emplace_back(bar.last_update_ms, new_progress);
        while (!bar.rate_samples_.empty()
            && bar.last_update_ms - bar.rate_samples_.front().first > kRateWindowMs) {
            bar.rate_samples_.pop_front();
        }
        poke_compositor();
    }
    std::function<void(float)> get_progress_updater() {
        if (on_progress_update) {
            return [this](float new_progress) {
                std::lock_guard<std::mutex> lock(*progress_mutex);
                default_progress_update(*this, new_progress);
                on_progress_update(*this, new_progress);
            };
        } else {
            return [this](float new_progress) {
                std::lock_guard<std::mutex> lock(*progress_mutex);
                default_progress_update(*this, new_progress);
            };
        }
    }
    TerminalPosition position_;
    mutable TerminalSize size_;
    uint32_t* chars_ = nullptr;
    TerminalColors* colors_ = nullptr;
    int buffer_cells_ = 0;
    float last_rendered_value_ = -1.0f;
    std::string last_rendered_label_;
    mutable char eta_cache_[32] = {0};
    mutable int eta_cache_len_ = 0;
    mutable float eta_cache_progress_ = -1.0f;
    mutable uint64_t eta_cache_ms_ = 0;
    static constexpr uint64_t kEtaWarmupMs = 10'000;
    static constexpr uint64_t kEtaDebounceMs = 1000;
    ~ProgressSprite() override {
        if (chars_) {
            delete[] chars_;
            chars_ = nullptr;
        }
        if (colors_) {
            delete[] colors_;
            colors_ = nullptr;
        }
    }
    TerminalPosition& position() override {
        return position_;
    }
    TerminalSize get_size() const override {
        TerminalSize s;
        size_ = s;
        return size_;
    }
    uint32_t* get_buffer() const override {
        return chars_;
    }
    TerminalColors* get_colors() const override {
        return colors_;
    }
    bool advance_frame(uint64_t /*epoch_ms*/) override {
        TerminalSize term_size = get_size();
        const int total_cells = std::max(1, static_cast<int>(term_size.width));
        float progress = progress_value;
        if (progress < 0.0f) {
            progress = 0.0f;
        } else if (progress > 1.0f) {
            progress = 1.0f;
        }
        char ts_buf[32] = {0};
        {
            const time_t secs = static_cast<time_t>(timestamp_ms / 1000);
            char t[24];
            std::strftime(t, sizeof(t), "%Y-%m-%dT%H:%M:%S", std::gmtime(&secs));
            std::snprintf(ts_buf, sizeof(ts_buf), "%s.%03dZ",
                t, static_cast<int>(timestamp_ms % 1000));
        }
        const char* level_tag;
        TerminalColors level_color;
        switch (log_level) {
            case Logger::LogLevel::INFO:  level_tag = " [INFO] ";  level_color = TerminalColors::GREEN;       break;
            case Logger::LogLevel::WARN:  level_tag = " [WARN] ";  level_color = TerminalColors::YELLOW;      break;
            case Logger::LogLevel::ERROR: level_tag = " [ERROR]";  level_color = TerminalColors::BRIGHT_RED;  break;
            case Logger::LogLevel::DEBUG: level_tag = " [DEBUG]";  level_color = TerminalColors::BLUE;        break;
            case Logger::LogLevel::TRACE: level_tag = " [TRACE]";  level_color = TerminalColors::MAGENTA;     break;
            default:              level_tag = " [UNKWN]";  level_color = TerminalColors::GREY;        break;
        }
        const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool eta_first = eta_cache_progress_ < 0.0f;
        const bool eta_progress_moved = progress != eta_cache_progress_;
        const bool eta_debounced = (now - eta_cache_ms_) >= kEtaDebounceMs;
        if (eta_first || (eta_progress_moved && eta_debounced)) {
            const uint64_t elapsed_ms = (now > timestamp_ms) ? (now - timestamp_ms) : 0;
            int n = 0;
            if (elapsed_ms < kEtaWarmupMs || progress <= 0.001f) {
                n = std::snprintf(eta_cache_, sizeof(eta_cache_), " est:--");
            } else {
                const uint64_t remain_ms = get_estimated_time_left_ms();
                if (remain_ms == UINT64_MAX) {
                    n = std::snprintf(eta_cache_, sizeof(eta_cache_), " est:--");
                } else {
                    const uint64_t secs = remain_ms / 1000;
                    const uint64_t m = secs / 60;
                    const uint64_t s = secs % 60;
                    if (m > 0) {
                        n = std::snprintf(eta_cache_, sizeof(eta_cache_),
                            " est:%llum %llus",
                            static_cast<unsigned long long>(m),
                            static_cast<unsigned long long>(s));
                    } else {
                        n = std::snprintf(eta_cache_, sizeof(eta_cache_),
                            " est:%llus",
                            static_cast<unsigned long long>(s));
                    }
                }
            }
            eta_cache_len_ = (n > 0) ? n : 0;
            eta_cache_progress_ = progress;
            eta_cache_ms_ = now;
        }
        uint64_t est_str_chars = 16;
        char pct_buf[16] = {0};
        const int pct_n = std::snprintf(pct_buf, sizeof(pct_buf),
            " %.1f%%", static_cast<double>(progress * 100.0f));
        const int pct_chars = pct_n > 0 ? pct_n : 0;
        const int ts_len = 32;
        const int level_len = static_cast<int>(std::strlen(level_tag));
        const int header_len = static_cast<int>(label.size());
        const int bar_overhead = 1 /*space*/ + 1 /*'['*/ + 1         + pct_chars + static_cast<int>(est_str_chars);
        const int bar_width = std::max(
            4, total_cells - ts_len - level_len - 1             - header_len - bar_overhead);
        const int filled = std::min(bar_width,
            std::max(0, static_cast<int>(progress * bar_width)));
        if (total_cells != buffer_cells_) {
            delete[] chars_;
            delete[] colors_;
            chars_ = new uint32_t[total_cells];
            colors_ = new TerminalColors[total_cells];
            buffer_cells_ = total_cells;
        }
        for (int i = 0; i < total_cells; ++i) {
            chars_[i] = 0u;
            colors_[i] = TerminalColors::TRANSPARENT;
        }
        auto write_span = [&](int col, const char* text, int n,
                            TerminalColors color) -> int {
            for (int i = 0; i < n && col < total_cells; ++i, ++col) {
                chars_[col] = static_cast<uint32_t>(
                    static_cast<unsigned char>(text[i]));
                colors_[col] = color;
            }
            return col;
        };
        int col = 0;
        col = write_span(col, ts_buf, ts_len, TerminalColors::GREY);
        col = write_span(col, level_tag, level_len, level_color);
        col = write_span(col, " ", 1, TerminalColors::DEFAULT);
        col = write_span(col, label.c_str(), header_len,
            TerminalColors::GREEN);
        col = write_span(col, " [", 2, TerminalColors::DEFAULT);
        for (int i = 0; i < filled && col < total_cells; ++i, ++col) {
            chars_[col] = static_cast<uint32_t>('=');
            colors_[col] = level_color;
        }
        for (int i = filled; i < bar_width && col < total_cells; ++i, ++col) {
            chars_[col] = static_cast<uint32_t>('-');
            colors_[col] = TerminalColors::GREY;
        }
        col = write_span(col, "]", 1, TerminalColors::DEFAULT);
        col = write_span(col, pct_buf, pct_chars, TerminalColors::WHITE);
        col = write_span(col, eta_cache_, static_cast<int>(est_str_chars),
            TerminalColors::GREY);
        last_rendered_value_ = progress_value;
        last_rendered_label_ = label;
        return true;
    }
    uint64_t get_estimated_time_left_ms() const {
        if (progress_value < 0.001f) return UINT64_MAX;
        if (progress_value >= 0.999f) return 0;
        const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const uint64_t elapsed = (now > timestamp_ms) ? (now - timestamp_ms) : 0;
        if (elapsed < kEtaWarmupMs) {
            return UINT64_MAX;
        }
        if (rate_samples_.size() >= 2) {
            const auto& oldest = rate_samples_.front();
            const uint64_t span_ms = (now > oldest.first) ? (now - oldest.first) : 0;
            const float span_p = progress_value - oldest.second;
            if (span_ms >= 1000 && span_p > 1e-6f) {
                const float remaining_p = 1.0f - progress_value;
                const double rate_p_per_ms =
                    static_cast<double>(span_p) / static_cast<double>(span_ms);
                return static_cast<uint64_t>(
                    static_cast<double>(remaining_p) / rate_p_per_ms);
            }
        }
        const uint64_t total_est = static_cast<uint64_t>(
            static_cast<double>(elapsed) / progress_value);
        return (total_est > elapsed) ? (total_est - elapsed) : 0;
    }
};
inline static size_t decode_line_with_ansi(const std::string& line,
    TerminalColors initial_color,std::unique_ptr<uint32_t[]>& cps_out,
    std::unique_ptr<TerminalColors[]>& colors_out
) {
    cps_out = std::make_unique<uint32_t[]>(line.size() + 1);
    colors_out = std::make_unique<TerminalColors[]>(line.size() + 1);
    TerminalColors current = initial_color;
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(line.data());
    const unsigned char* const end = p + line.size();
    size_t out = 0;
    while (p < end) {
        if (*p == 0x1Bu) {
            /// CSI: ESC [ params final
            if (p + 1 < end && p[1] == '[') {
                p += 2;
                const unsigned char* params_start = p;
                while (p < end
                    && (*p == ';' || (*p >= '0' && *p <= '9'))) {
                    ++p;
                }
                if (p < end) {
                    const unsigned char final_byte = *p;
                    if (final_byte == 'm') {
                        current = parse_sgr(
                            reinterpret_cast<const char*>(params_start),
                            static_cast<size_t>(p - params_start),
                            current);
                    }
                    ++p;
                }
                continue;
            }
            /// OSC: ESC ] ... BEL or ST. Skip until terminator.
            if (p + 1 < end && p[1] == ']') {
                p += 2;
                while (p < end && *p != 0x07u) {
                    if (*p == 0x1Bu && p + 1 < end && p[1] == 0x5Cu) {
                        p += 2;
                        break;
                    }
                    ++p;
                }
                if (p < end && *p == 0x07u) ++p;
                continue;
            }
            ++p;
            continue;
        }
        const uint32_t cp = decode_utf8_one(p, end);
        cps_out[out] = cp;
        colors_out[out] = current;
        ++out;
    }
    return out;
}
struct TerminalScreen {
    TerminalSize size;
    uint64_t epoch_ms;
    std::vector<std::vector<uint32_t>> rows;
    std::vector<std::vector<TerminalColors>> colors;
    std::vector<std::unique_ptr<SpriteBase>> owned_sprites_;
    std::shared_mutex sprites_mutex_;
    std::vector<std::unique_ptr<TerminalLogMessage>> log_messages_;
    std::shared_mutex log_messages_mutex_;
    std::vector<std::unique_ptr<ProgressSprite>> progress_bars_;
    std::shared_mutex progress_bars_mutex_;
    bool has_border_ = false;
    TerminalColors border_color_ = TerminalColors::DEFAULT;
    TerminalScreen() {
        size = TerminalSize();
        rows.assign(size.height, std::vector<uint32_t>(size.width, 0x20u));
        colors.assign(size.height,
            std::vector<TerminalColors>(size.width, TerminalColors::DEFAULT));
        epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    TerminalScreen(int width, int height) {
        size = TerminalSize(width, height);
        rows.assign(size.height, std::vector<uint32_t>(size.width, 0x20u));
        colors.assign(size.height,
            std::vector<TerminalColors>(size.width, TerminalColors::DEFAULT));
        epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    //----------------------------------------------------- Non-copyable, but movable.
    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;
    TerminalScreen(TerminalScreen&& other) noexcept :
        size(other.size),
        epoch_ms(other.epoch_ms),
        rows(std::move(other.rows)),
        colors(std::move(other.colors)),
        owned_sprites_(std::move(other.owned_sprites_)),
        log_messages_(std::move(other.log_messages_)),
        progress_bars_(std::move(other.progress_bars_)) {
        other.epoch_ms = 0;
    }
    TerminalScreen& operator=(TerminalScreen&& other) noexcept {
        if (this != &other) {
            size = other.size;
            epoch_ms = other.epoch_ms;
            rows = std::move(other.rows);
            colors = std::move(other.colors);
            owned_sprites_ = std::move(other.owned_sprites_);
            log_messages_ = std::move(other.log_messages_);
            progress_bars_ = std::move(other.progress_bars_);
            other.epoch_ms = 0;
            other.size = TerminalSize();
        }
        return *this;
    }
    TerminalScreen clone() const {
        TerminalScreen copy;
        copy.size = size;
        copy.epoch_ms = epoch_ms;
        copy.rows = rows;
        copy.colors = colors;
        return copy;
    }
    bool operator==(const TerminalScreen& other) const {
        if (other.size.width != size.width || other.size.height != size.height) {
            return false;
        }
        for (int i = 0; i < size.height; ++i) {
            if (other.rows[i] != rows[i]) {
                return false;
            }
        }
        return true;
    }
    bool operator!=(const TerminalScreen& other) const {
        return !(*this == other);
    }
    ~TerminalScreen() {
        rows.clear();
    }
    void touch() {
        epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    void resize(int new_width, int new_height) {
        if (new_width == size.width && new_height == size.height) {
            return;
        }
        std::vector<std::vector<uint32_t>> new_rows(
            new_height, std::vector<uint32_t>(new_width, 0x20u));
        std::vector<std::vector<TerminalColors>> new_colors(
            new_height,
            std::vector<TerminalColors>(new_width, TerminalColors::DEFAULT));
        const int copy_h = std::min(static_cast<int>(size.height), new_height);
        const int copy_w = std::min(static_cast<int>(size.width), new_width);
        for (int r = 0; r < copy_h; ++r) {
            for (int c = 0; c < copy_w; ++c) {
                new_rows[r][c] = rows[r][c];
                new_colors[r][c] = colors[r][c];
            }
        }
        rows = std::move(new_rows);
        colors = std::move(new_colors);
        size = TerminalSize(new_width, new_height);
    }
    void enable_border(TerminalColors color = TerminalColors::DEFAULT) {
        has_border_ = color != TerminalColors::TRANSPARENT;
        border_color_ = color;
    }
    TerminalSize interior_size() const {
        if (!has_border_) {
            return size;
        }
        const int w = static_cast<int>(size.width);
        const int h = static_cast<int>(size.height);
        return TerminalSize(std::max(0, w - 2), std::max(0, h - 2));
    }
    template<typename T, typename... Args>
    T* add_sprite(Args&&... args) {
        auto sprite = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = sprite.get();
        {
            std::unique_lock<std::shared_mutex> lock(sprites_mutex_);
            owned_sprites_.push_back(std::move(sprite));
        }
        return raw;
    }
    ProgressSprite* add_progress_bar(const std::string& label = "") {
        auto bar = std::make_unique<ProgressSprite>(label);
        ProgressSprite* raw = bar.get();
        {
            std::unique_lock<std::shared_mutex> lock(progress_bars_mutex_);
            progress_bars_.push_back(std::move(bar));
        }
        return raw;
    }
    void paint_border() {
        const int w = static_cast<int>(size.width);
        const int h = static_cast<int>(size.height);
        if (w < 2 || h < 2) {
            return;
        }
        rows[0][0] = 0x250C; colors[0][0] = border_color_;
        rows[0][w - 1] = 0x2510; colors[0][w - 1] = border_color_;
        rows[h - 1][0] = 0x2514; colors[h - 1][0] = border_color_;
        rows[h - 1][w - 1] = 0x2518; colors[h - 1][w - 1] = border_color_;
        for (int c = 1; c < w - 1; ++c) {
            rows[0][c] = 0x2500; colors[0][c] = border_color_;
            rows[h - 1][c] = 0x2500; colors[h - 1][c] = border_color_;
        }
        for (int r = 1; r < h - 1; ++r) {
            rows[r][0] = 0x2502; colors[r][0] = border_color_;
            rows[r][w - 1] = 0x2502; colors[r][w - 1] = border_color_;
        }
    }
    bool draw();
};
class LogoAnimation;
class Terminal {
public:
    struct ScheduledTask {
        uint64_t epoch_ms;
        std::function<void()> fn;
    };
private:
    struct TaskComparator {
        bool operator()(const ScheduledTask& a, const ScheduledTask& b) const {
            return a.epoch_ms > b.epoch_ms;
        }
    };
    /// A composited window plus an optional size policy. When `sizer` is set
    /// the compositor re-evaluates it every frame and resizes the window to
    /// follow terminal resize events.
    struct WindowSlot {
        TerminalPosition pos;
        std::unique_ptr<TerminalScreen> screen;
        std::function<TerminalSize()> sizer;
    };
    std::vector<WindowSlot> windows_;
    std::mutex windows_mutex_;
    std::atomic<TerminalScreen*> current_screen_{nullptr};
    std::priority_queue<ScheduledTask,
                        std::vector<ScheduledTask>,
                        TaskComparator> tasks_;
    std::mutex tasks_mutex_;
    std::condition_variable tasks_cv_;
    std::atomic<bool> running_{true};
    std::thread worker_thread_;
    static constexpr uint64_t FRAME_INTERVAL_MS = 33;
    /// Idle wake-up cadence so terminal resizes are picked up even when no
    /// sprite is animating (pure polling - portable, no signals involved).
    static constexpr uint64_t RESIZE_POLL_MS = 250;
    uint64_t last_paint_ms_ = 0;
    TerminalSize terminal_size() {
        return TerminalSize(get_terminal_width(), get_terminal_height());
    }
    void paint_screen(TerminalScreen* screen) {
        const int th = static_cast<int>(screen->size.height);
        std::stringstream out;
        out << "\033[?25l\033[?7l";
        TerminalColors current_color = TerminalColors::NONE_SPECIFIED;
        for (int row = 0; row < th; ++row) {
            out << "\033[" << (row + 1) << ";1H";
            for (int col = 0; col < screen->size.width; ++col) {
                TerminalColors cell_color = screen->colors[row][col];
                uint32_t cell_char = screen->rows[row][col];
                if (cell_color == TerminalColors::TRANSPARENT
                    || cell_color == TerminalColors::NONE_SPECIFIED) {
                    cell_color = TerminalColors::DEFAULT;
                    cell_char = 0x20u;
                }
                if (cell_color != current_color) {
                    const auto it = terminal_color_to_ansi_code.find(cell_color);
                    if (it != terminal_color_to_ansi_code.end()) {
                        out << it->second;
                    }
                    current_color = cell_color;
                }
                encode_utf8(cell_char, out);
            }
            out << "\033[K";
        }
        out << "\033[0m\033[?7h";
        std::cout << out.str();
        std::cout.flush();
    }
    void place_window(
        const TerminalScreen* window,
        const TerminalPosition& pos,
        TerminalScreen* target_screen
    ) {
        const TerminalSize size = window->size;
        for (int row = 0; row < size.height; ++row) {
            const int target_row = pos.row + row;
            if (target_row < 0 || target_row >= target_screen->size.height) {
                continue;
            }
            for (int col = 0; col < size.width; ++col) {
                const int target_col = pos.column + col;
                if (target_col < 0 || target_col >= target_screen->size.width) {
                    continue;
                }
                if (window->colors[row][col] == TerminalColors::TRANSPARENT) {
                    continue;
                }
                target_screen->rows[target_row][target_col] =
                    window->rows[row][col];
                target_screen->colors[target_row][target_col] =
                    window->colors[row][col];
            }
        }
    }
    static uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    void screen_update_worker() {
        while (running_.load(std::memory_order_acquire)) {
            std::vector<ScheduledTask> due;
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                const uint64_t t = now_ms();
                while (!tasks_.empty() && tasks_.top().epoch_ms <= t) {
                    due.push_back(std::move(
                        const_cast<ScheduledTask&>(tasks_.top())));
                    tasks_.pop();
                }
            }
            for (auto& task : due) {
                if (task.fn) {
                    task.fn();
                }
            }
            const uint64_t epoch_ms = now_ms();
            bool any_active = false;
            if (epoch_ms >= last_paint_ms_ + FRAME_INTERVAL_MS
                || last_paint_ms_ == 0) {
                const TerminalSize term = terminal_size();
                auto* next_screen = new TerminalScreen(
                    static_cast<int>(term.width),
                    static_cast<int>(term.height));
                next_screen->epoch_ms = epoch_ms;
                {
                    std::lock_guard<std::mutex> lock(windows_mutex_);
                    for (auto& win_slot : windows_) {
                        if (win_slot.sizer) {
                            const TerminalSize want = win_slot.sizer();
                            if (want != win_slot.screen->size) {
                                win_slot.screen->resize(
                                    static_cast<int>(want.width),
                                    static_cast<int>(want.height));
                            }
                        }
                        auto& win = win_slot.screen;
                        auto& pos = win_slot.pos;
                        win->epoch_ms = epoch_ms;
                        const bool win_active = win->draw();
                        any_active = any_active || win_active;
                        place_window(win.get(), pos, next_screen);
                    }
                }
                TerminalScreen* old_screen = current_screen_.exchange(
                    next_screen, std::memory_order_acq_rel);
                delete old_screen;
                paint_screen(next_screen);
                last_paint_ms_ = epoch_ms;
            } else {
                std::lock_guard<std::mutex> lock(windows_mutex_);
                for (auto& win_slot : windows_) {
                    (void)win_slot.pos;
                    if (win_slot.screen->draw()) {
                        any_active = true;
                    }
                }
            }
            std::unique_lock<std::mutex> lock(tasks_mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            uint64_t next_wake_ms = now_ms() + RESIZE_POLL_MS;
            if (!tasks_.empty()) {
                next_wake_ms = std::min(
                    next_wake_ms, tasks_.top().epoch_ms);
            }
            if (any_active) {
                next_wake_ms = std::min(
                    next_wake_ms, last_paint_ms_ + FRAME_INTERVAL_MS);
            }
            const uint64_t t = now_ms();
            if (next_wake_ms > t) {
                tasks_cv_.wait_for(
                    lock,
                    std::chrono::milliseconds(next_wake_ms - t));
            }
        }
    }
    static void signal_restore_cursor(int sig) {
        if (Terminal* t = instance_if_alive()) {
            t->running_.store(false, std::memory_order_release);
        }
        const char seq[] = "\033[?25h\n";
        [[maybe_unused]] const ssize_t _ = ::write(STDOUT_FILENO, seq, sizeof(seq) - 1);
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }
    static Terminal*& instance_ptr() {
        static Terminal* p = nullptr;
        return p;
    }
    static void atexit_restore_cursor() {
        const char seq[] = "\033[?25h";
        [[maybe_unused]] const ssize_t _ = ::write(STDOUT_FILENO, seq, sizeof(seq) - 1);
    }
    Terminal() {
        instance_ptr() = this;
        std::signal(SIGINT, &Terminal::signal_restore_cursor);
        std::signal(SIGTERM, &Terminal::signal_restore_cursor);
#ifndef _WIN32
        std::signal(SIGHUP, &Terminal::signal_restore_cursor);
#endif
        std::atexit(&Terminal::atexit_restore_cursor);
        worker_thread_ = std::thread(&Terminal::screen_update_worker, this);
    }
public:
    static Terminal& instance() {
        static Terminal instance;
        return instance;
    }
    static Terminal* instance_if_alive() {
        return instance_ptr();
    }
    void shutdown() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        tasks_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        TerminalScreen* current_screen =
            current_screen_.load(std::memory_order_acquire);
        if (current_screen != nullptr) {
            delete current_screen;
            current_screen_.store(nullptr, std::memory_order_release);
        }
        std::cout << "\033[?25h" << std::flush;
        instance_ptr() = nullptr;
    }
    ~Terminal() {
        shutdown();
    }
    //-------------------------------------------------- Non-copyable / non-movable singleton.
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;
    void schedule(uint64_t epoch_ms, std::function<void()> fn = {}) {
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.push({epoch_ms, std::move(fn)});
        }
        tasks_cv_.notify_one();
    }
    void poke() {
        schedule(0);
    }
    TerminalScreen* add_window(
        TerminalPosition pos,
        std::unique_ptr<TerminalScreen>&& screen
    ) {
        TerminalScreen* raw = screen.get();
        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            windows_.push_back(WindowSlot{
                pos, std::move(screen), std::function<TerminalSize()>()});
        }
        poke();
        return raw;
    }
    /// Adds a window that tracks the terminal size through `sizer`; the
    /// compositor resizes the window whenever the sizer's answer changes.
    TerminalScreen* add_window(
        TerminalPosition pos,
        std::unique_ptr<TerminalScreen>&& screen,
        std::function<TerminalSize()> sizer
    ) {
        TerminalScreen* raw = screen.get();
        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            windows_.push_back(WindowSlot{pos, std::move(screen),
                std::move(sizer)});
        }
        poke();
        return raw;
    }
    ProgressSprite* add_progress_bar_window(
        int top_row,
        const std::string& label = ""
    ) {
        const int w = std::max(1, get_terminal_width());
        auto screen = std::make_unique<TerminalScreen>(w, 1);
        ProgressSprite* bar = screen->add_progress_bar(label);
        bar->position().column = 0.0;
        bar->position().row = 0.0;
        add_window(TerminalPosition(0.0,
            static_cast<double>(top_row)), std::move(screen),
            [top_row]() -> TerminalSize {
                return TerminalSize(
                    std::max(1, get_terminal_width()), 1);
            });
        return bar;
    }
    void remove_window(TerminalScreen* handle) {
        {
            std::lock_guard<std::mutex> lock(windows_mutex_);
            for (auto it = windows_.begin(); it != windows_.end(); ++it) {
                if (it->screen.get() == handle) {
                    windows_.erase(it);
                    break;
                }
            }
        }
        poke();
    }
    LogoAnimation* add_logo_window(int top_row, int pad_rows = 1);
    static void draw_sprite(
        SpriteBase* sprite,
        TerminalScreen* screen,
        int x_offset = 0,
        int y_offset = 0
    ) {
        const TerminalSize size = sprite->get_size();
        const TerminalPosition pos = sprite->position();
        const uint32_t* buffer = sprite->get_buffer();
        const TerminalColors* colors = sprite->get_colors();
        if (buffer == nullptr || colors == nullptr) {
            return;
        }
        const int screen_w = static_cast<int>(screen->size.width);
        const int screen_h = static_cast<int>(screen->size.height);
        const int sprite_w = static_cast<int>(size.width);
        const int sprite_h = static_cast<int>(size.height);
        const int x0 = static_cast<int>(pos.column) + x_offset;
        const int y0 = static_cast<int>(pos.row) + y_offset;
        size_t buffer_index = 0;
        for (int sy = 0; sy < sprite_h; ++sy) {
            const int dy = y0 + sy;
            for (int sx = 0; sx < sprite_w; ++sx, ++buffer_index) {
                if (colors[buffer_index] == TerminalColors::TRANSPARENT) {
                    continue;
                }
                const int dx = x0 + sx;
                if (dx < 0 || dx >= screen_w || dy < 0 || dy >= screen_h) {
                    continue;
                }
                screen->rows[dy][dx] = buffer[buffer_index];
                screen->colors[dy][dx] = colors[buffer_index];
            }
        }
    }
    class LogMessageQueueSprite : public SpriteBase {
    public:
        struct Message {
            uint64_t timestamp_ms;
            std::unique_ptr<uint32_t[]> cps;
            size_t count;
            TerminalColors color;
            std::unique_ptr<TerminalColors[]> per_cell_colors;
        };
    private:
        TerminalPosition position_;
        TerminalSize size_{0, 0};
        TerminalScreen* parent_ = nullptr;
        int parent_margin_right_ = 0;
        int parent_margin_bottom_ = 0;
        uint32_t* chars_ = nullptr;
        TerminalColors* colors_ = nullptr;
        std::deque<Message> messages_;
        size_t byte_budget_used_ = 0;
        size_t byte_budget_cap_ = 4u * 1024u * 1024u;
        std::atomic<bool> dirty_{true};
        std::mutex queue_mutex_;
        uint64_t append_counter_ = 0;
        uint64_t flip_end_ms_ = 0;
        bool flip_horizontal_ = false;
        bool flip_easter_egg_ = false;
        uint64_t flip_interval_appends_ = 500;
        uint64_t flip_duration_ms_ = 3000;
        std::mt19937 flip_rng_{std::random_device{}()};
        struct ColorBand {
            bool horizontal;
            int index;
            TerminalColors color;
            uint64_t end_ms;
        };
        std::vector<ColorBand> bands_;
        uint64_t band_chance_ = 0;
        uint64_t band_duration_ms_ = 2000;
    public:
        LogMessageQueueSprite(TerminalPosition pos, TerminalSize size)
            : position_(pos), size_(size) {
            const size_t cells = cell_count();
            chars_ = new uint32_t[cells]();
            colors_ = new TerminalColors[cells];
            for (size_t i = 0; i < cells; ++i) {
                colors_[i] = TerminalColors::TRANSPARENT;
            }
        }
        void bind_parent(
            TerminalScreen* parent,
            int margin_right = 0,
            int margin_bottom = 0
        ) {
            parent_ = parent;
            parent_margin_right_ = margin_right;
            parent_margin_bottom_ = margin_bottom;
            dirty_.store(true, std::memory_order_release);
        }
        void set_easter_egg(
            bool enabled,
            uint64_t interval_appends = 500,
            uint64_t duration_ms = 3000
        ) {
            flip_easter_egg_ = enabled;
            flip_interval_appends_ = std::max<uint64_t>(1, interval_appends);
            flip_duration_ms_ = duration_ms;
        }
        void set_color_band_egg(uint64_t chance_denom, uint64_t duration_ms = 2000) {
            band_chance_ = chance_denom;
            band_duration_ms_ = duration_ms;
        }
        ~LogMessageQueueSprite() override {
            delete[] chars_;
            delete[] colors_;
        }
        LogMessageQueueSprite(const LogMessageQueueSprite&) = delete;
        LogMessageQueueSprite& operator=(const LogMessageQueueSprite&) = delete;
        void set_byte_budget(size_t new_cap) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            byte_budget_cap_ = new_cap;
            evict_to_budget_locked();
            dirty_.store(true, std::memory_order_release);
        }
        void append(
            const std::string& line,
            TerminalColors color = TerminalColors::DEFAULT
        ) {
            std::unique_ptr<uint32_t[]> cps;
            std::unique_ptr<TerminalColors[]> per_cell_colors;
            size_t n = 0;
            const bool has_ansi =
                line.find('\033') != std::string::npos;
            if (has_ansi) {
                n = decode_line_with_ansi(line, color, cps, per_cell_colors);
            } else {
                cps = decode_utf8(line, n);
            }
            const uint64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
            Message m;
            m.timestamp_ms = now_ms;
            m.cps = std::move(cps);
            m.count = n;
            m.color = color;
            m.per_cell_colors = std::move(per_cell_colors);
            bool schedule_flip_unflip = false;
            uint64_t flip_end_ms_snapshot = 0;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                constexpr size_t kCoalescePrefix = 24;
                if (!messages_.empty()) {
                    Message& prev = messages_.back();
                    const size_t check =
                        std::min({kCoalescePrefix, prev.count, n});
                    bool same_prefix = (check > 0);
                    for (size_t i = 0; i < check; ++i) {
                        if (prev.cps[i] != m.cps[i]) {
                            same_prefix = false;
                            break;
                        }
                    }
                    if (same_prefix) {
                        byte_budget_used_ -= prev.count * sizeof(uint32_t);
                        prev.cps = std::move(m.cps);
                        prev.count = n;
                        prev.color = m.color;
                        prev.per_cell_colors = std::move(m.per_cell_colors);
                        prev.timestamp_ms = m.timestamp_ms;
                        byte_budget_used_ += n * sizeof(uint32_t);
                        evict_to_budget_locked();
                        dirty_.store(true, std::memory_order_release);
                        poke_compositor();
                        return;
                    }
                }
                byte_budget_used_ += n * sizeof(uint32_t);
                messages_.push_back(std::move(m));
                evict_to_budget_locked();
                ++append_counter_;
                if (flip_easter_egg_
                    && flip_end_ms_ == 0
                    && (std::uniform_int_distribution<uint64_t>(
                            0, 2 * flip_interval_appends_ - 1)(flip_rng_) == 0)) {
                    flip_horizontal_ =
                        (flip_rng_() & 1u) != 0;
                    flip_end_ms_ = now_ms + flip_duration_ms_;
                    schedule_flip_unflip = true;
                    flip_end_ms_snapshot = flip_end_ms_;
                }
                if (band_chance_ > 0
                    && (std::uniform_int_distribution<uint64_t>(
                            0, band_chance_ - 1)(flip_rng_) == 0)) {
                    ColorBand band;
                    band.horizontal = (flip_rng_() & 1u) != 0;
                    const int axis_max = band.horizontal
                        ? std::max(1, static_cast<int>(size_.height))
                        : std::max(1, static_cast<int>(size_.width));
                    band.index = static_cast<int>(
                        std::uniform_int_distribution<int>(
                            0, axis_max - 1)(flip_rng_));
                    static constexpr TerminalColors kBandPalette[] = {
                        TerminalColors::BRIGHT_RED,
                        TerminalColors::BRIGHT_GREEN,
                        TerminalColors::BRIGHT_CYAN,
                        TerminalColors::BRIGHT_PURPLE,
                        TerminalColors::YELLOW,
                        TerminalColors::MAGENTA
                    };
                    band.color = kBandPalette[
                        std::uniform_int_distribution<int>(
                            0, static_cast<int>(
                                sizeof(kBandPalette) / sizeof(kBandPalette[0])
                            ) - 1)(flip_rng_)];
                    band.end_ms = now_ms + band_duration_ms_;
                    bands_.push_back(band);
                    schedule_wake(band.end_ms);
                }
            }
            dirty_.store(true, std::memory_order_release);
            poke_compositor();
            if (schedule_flip_unflip) {
                schedule_wake(flip_end_ms_snapshot);
            }
        }
        void clear() {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                messages_.clear();
                byte_budget_used_ = 0;
            }
            dirty_.store(true, std::memory_order_release);
            poke_compositor();
        }
        void resize(TerminalPosition pos, TerminalSize size) {
            position_ = pos;
            size_ = size;
            delete[] chars_;
            delete[] colors_;
            const size_t cells = cell_count();
            chars_ = new uint32_t[cells]();
            colors_ = new TerminalColors[cells];
            for (size_t i = 0; i < cells; ++i) {
                colors_[i] = TerminalColors::TRANSPARENT;
            }
            dirty_.store(true, std::memory_order_release);
            poke_compositor();
        }
        //---------------------------------------------------------- SpriteBase overrides
        TerminalPosition& position() override { return position_; }
        TerminalSize get_size() const override { return size_; }
        uint32_t* get_buffer() const override { return chars_; }
        TerminalColors* get_colors() const override { return colors_; }
        bool advance_frame(uint64_t epoch_ms) override;
    private:
        size_t cell_count() const {
            return static_cast<size_t>(size_.width)
                * static_cast<size_t>(size_.height);
        }
        void evict_to_budget_locked() {
            while (byte_budget_used_ > byte_budget_cap_ && !messages_.empty()) {
                const Message& front = messages_.front();
                byte_budget_used_ -= front.count * sizeof(uint32_t);
                messages_.pop_front();
            }
        }
    };
    LogMessageQueueSprite* add_bordered_log_window(
        int top_row,
        TerminalColors border_color = TerminalColors::DEFAULT
    ) {
        const int w = std::max(2, get_terminal_width() - 1);
        const int h = std::max(2, get_terminal_height() - top_row);
        auto screen = std::make_unique<TerminalScreen>(w, h);
        screen->enable_border(border_color);
        const TerminalSize inner = screen->interior_size();
        LogMessageQueueSprite* log = screen->add_sprite<LogMessageQueueSprite>(
            TerminalPosition(0.0, 0.0), inner);
        /// The sprite follows the window's interior so log lines rewrap when
        /// the terminal (and with it the window) is resized.
        log->bind_parent(screen.get());
        add_window(TerminalPosition(0.0,
            static_cast<double>(top_row)), std::move(screen),
            [top_row]() -> TerminalSize {
                return TerminalSize(
                    std::max(2, get_terminal_width() - 1),
                    std::max(2, get_terminal_height() - top_row));
            });
        return log;
    }
};
inline void poke_compositor() {
    Terminal::instance().poke();
}
inline void schedule_wake(uint64_t epoch_ms) {
    Terminal::instance().schedule(epoch_ms);
}
class LogoAnimation : public SpriteBase {
public:
    TerminalPosition position_{0.0, 0.0};
    uint32_t* chars = nullptr;
    TerminalColors* colors = nullptr;
    int last_sw_ = 0;
    static constexpr int INTRO_ROWS = 7;
    std::atomic<int64_t> finale_start_ms_{0};
    int64_t logo_animation_time_ = 0;
    std::vector<BraillePoint> braille_points_;
    std::shared_mutex braille_points_mutex_;
    static inline const std::vector<TerminalColors> PALETTE_ = {
        TerminalColors::DARK_MAGENTA,
        TerminalColors::DARK_GRAY,
        TerminalColors::DARK_GRAY,
        TerminalColors::DARK_BLUE,
        TerminalColors::DARK_BLUE,
        TerminalColors::DARK_BLUE,
        TerminalColors::DARK_BLUE,
        TerminalColors::DARK_PURPLE,
        TerminalColors::DARK_BLUE,
        TerminalColors::DARK_BLUE,
        TerminalColors::DARK_PURPLE,
        TerminalColors::DARK_CYAN,
        TerminalColors::DARK_PURPLE,
        TerminalColors::DARK_CYAN,
        TerminalColors::BLUE,
        TerminalColors::BLUE,
        TerminalColors::PURPLE
    };
    /// @brief Total wall-clock duration of the intro animation, in ms.
    static constexpr uint64_t INTRO_DURATION_MS = 17000;
    struct Logo {
        TerminalPosition position;
        std::vector<BraillePoint::CombinedPoint> points;
        std::unordered_set<size_t> white_point_indices;
        std::atomic<bool> all_white{false};
        double render_col_offset = 0;
        double render_row_offset = 0;
        Logo() {
            TerminalScreen screen = TerminalScreen();
            (void)screen;  // size fetched again below; kept for symmetry
            std::vector<std::vector<std::array<BraillePoint::CombinedPoint, 2>>> text_ =
                BrailleSprite::get_text("nebula", TerminalColors::TRANSPARENT);
            size_t total_cols = 0;
            for (const auto& letter : text_) total_cols += letter.size();
            int screen_w = get_terminal_width();
            constexpr int render_h = LogoAnimation::INTRO_ROWS;
            (void)render_h;
            render_col_offset = -22.0;
            render_row_offset = -4.0;
            constexpr int CENTER_NUDGE_CHARS = -4;
            const double target_left_char =
                (static_cast<double>(screen_w)
                - static_cast<double>(total_cols)) / 2.0
                + static_cast<double>(CENTER_NUDGE_CHARS);
            position = TerminalPosition(
                2.0 * target_left_char - render_col_offset,
                12.0);
            for (size_t row = 0; row < 2; row++) {
                for (size_t i = 0; i < text_.size(); ++i) {
                    for (size_t j = 0; j < text_[i].size(); ++j) {
                        points.push_back(text_[i][j][row]);
                    }
                }
            }
        }
        void hitbox(
            BraillePoint::CombinedPoint* screen,
            const std::vector<TerminalColors>& palette,
            int window_lo,
            int window_hi,
            std::mt19937& rng,
            TerminalColors override_color = TerminalColors::NONE_SPECIFIED
        ) {
            const int screen_w = get_terminal_width();
            const int screen_h = LogoAnimation::INTRO_ROWS;
            const int w = width();
            const int top_col = static_cast<int>(
                std::floor((position.column + render_col_offset) / 2.0));
            const int top_row = static_cast<int>(
                std::floor((position.row + render_row_offset) / 4.0));
            for (int i = 0; i < w; ++i) {
                const int col = top_col + i;
                if (col < 0 || col >= screen_w) {
                    continue;
                }
                for (int j = 0; j < 2; ++j) {
                    const int row = top_row + j;
                    if (row < 0 || row >= screen_h) {
                        continue;
                    }
                    auto pt = &points[static_cast<size_t>(j) * static_cast<size_t>(w) + i];
                    if (pt->combined_value == 0) {
                        continue;
                    }
                    auto scpt = &screen[static_cast<size_t>(row) * static_cast<size_t>(screen_w) + col];
                    scpt->combined_value = pt->combined_value;
                    if (override_color != TerminalColors::NONE_SPECIFIED) {
                        scpt->color = override_color;
                    } else {
                        std::uniform_int_distribution<int> pick(window_lo, window_hi);
                        scpt->color = palette[static_cast<size_t>(pick(rng))];
                    }
                }
            }
        }
        int width() const {
            return points.size() / 2;
        }
        void draw_frame(
            BraillePoint::CombinedPoint* screen,
            TerminalColors frame_color,
            int start_col,
            int rule_width
        ) {
            const int screen_w = get_terminal_width();
            const int screen_h = LogoAnimation::INTRO_ROWS;
            const int top_row = static_cast<int>(std::floor(
                (position.row + render_row_offset) / 4.0));
            const int upper_row = top_row - 1;   // one row above the logo
            const int lower_row = top_row + 2;   // one row below the logo
            for (int i = 0; i < rule_width; ++i) {
                const int col = start_col + i;
                if (col < 0 || col >= screen_w) {
                    continue;
                }
                if (upper_row >= 0 && upper_row < screen_h) {
                    auto scpt = &screen[static_cast<size_t>(upper_row)
                        * static_cast<size_t>(screen_w) + static_cast<size_t>(col)];
                    scpt->char_instead = '-';
                    scpt->color = frame_color;
                }
                if (lower_row >= 0 && lower_row < screen_h) {
                    auto scpt = &screen[static_cast<size_t>(lower_row)
                        * static_cast<size_t>(screen_w) + static_cast<size_t>(col)];
                    scpt->char_instead = '-';
                    scpt->color = frame_color;
                }
            }
        }
    } logo_;
    LogoAnimation() {}
    TerminalSize get_size() const override { return TerminalSize(get_terminal_width(), INTRO_ROWS); }
    TerminalPosition& position() override { return position_; }
    uint32_t* get_buffer() const override { return chars; }
    TerminalColors* get_colors() const override { return colors; }
    bool advance_frame(uint64_t epoch_ms) override;
    void spawn_particle(uint64_t epoch_ms, double r, double tilt, double tilt_axis, double base_speed, TerminalColors color, std::mt19937& gen );
    void trigger_finale() {
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        int64_t expected = 0;
        finale_start_ms_.compare_exchange_strong(
            expected, now_ms, std::memory_order_acq_rel);
    }
    bool is_complete() const {
        if (logo_animation_time_ == 0) {
            return false;
        }
        const int64_t finale_start =
            finale_start_ms_.load(std::memory_order_acquire);
        if (finale_start == 0) {
            return false;
        }
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        return now_ms - finale_start >= 4500 + 500;
    }
    void mark_started(uint64_t epoch_ms) {
        if (logo_animation_time_ == 0) {
            logo_animation_time_ =
                static_cast<int64_t>(epoch_ms)
                + static_cast<int64_t>(INTRO_DURATION_MS);
        }
    }
};
struct ProgressBar {
    std::string id;
    std::string header;
    std::string start_time_str;
    float progress;
    int width;
    unsigned long start_line;
    uint64_t start_time;
    uint64_t creation_order = 0;
};
inline std::string RenderProgressBarLine(const ProgressBar& bar);
struct GlobalLoggingContext {
private:
    static std::atomic<uint64_t>& thread_counter() {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }
    bool is_stdout_a_tty;
    bool is_stderr_a_tty;
    std::atomic<std::ostream*> stdout_;
    std::atomic<std::ostream*> stderr_;
    std::atomic<std::ostream*> addtl_log_;
    GlobalLoggingContext() : is_stdout_a_tty(stdout_is_tty()), is_stderr_a_tty(stderr_is_tty()),
        stdout_(&std::cout), stderr_(&std::cerr), addtl_log_(nullptr) {
        const bool use_colors = enable_ansi_colors();
        colored_output().store(use_colors, std::memory_order_release);
        constexpr char32_t banner[] = U"𝔰𝔦𝔪𝔭𝔩𝔢 𝔱𝔥𝔯𝔢𝔞𝔡𝔰𝔞𝔣𝔢 𝔩𝔬𝔤𝔤𝔢𝔯";
        constexpr size_t last_character = sizeof(banner) / sizeof(banner[0]) - 2;
        std::ostringstream banner_output;
        const auto append_braille = [&](bool reverse) {
            constexpr char32_t dots[] = U"⠂⠒⠶⣶⣿";
            constexpr size_t last_dot = sizeof(dots) / sizeof(dots[0]) - 2;
            for (size_t dot = 0; dot <= last_dot; ++dot) {
                const size_t shade = reverse ? last_dot - dot : dot;
                if (use_colors) {
                    const size_t gray = 32 * shade / last_dot;
                    const size_t blue = 40 * shade / last_dot;
                    banner_output << "\033[38;2;" << gray << ';' << gray << ';' << blue << 'm';
                }
                encode_utf8(dots[shade], banner_output);
            }
        };
        append_braille(false);
        banner_output << ' ';
        for (size_t character = 0; character <= last_character; ++character) {
            if (use_colors) {
                const double position = 1.0 - std::abs(2.0 * character / last_character - 1.0);
                const double blend = position * position * (3.0 - 2.0 * position);
                const int red = static_cast<int>(32.0 - 24.0 * blend);
                const int green = static_cast<int>(32.0 - 12.0 * blend);
                const int blue = static_cast<int>(40.0 + 190.0 * blend);
                banner_output << "\033[38;2;" << red << ';' << green << ';' << blue << 'm';
            }
            encode_utf8(banner[character], banner_output);
        }
        banner_output << ' ';
        append_braille(true);
        if (use_colors) banner_output << "\033[0m";
        std::cout << banner_output.str() << std::endl;
    }
public:
    GlobalLoggingContext(const GlobalLoggingContext&) = delete;
    GlobalLoggingContext& operator=(const GlobalLoggingContext&) = delete;
    GlobalLoggingContext(GlobalLoggingContext&&) = delete;
    GlobalLoggingContext& operator=(GlobalLoggingContext&&) = delete;
    ~GlobalLoggingContext() {
        if (addtl_log_.load() != nullptr) {
            std::ostream* file_stream = addtl_log_.load();
            if (file_stream) {
                std::ofstream* ofs = dynamic_cast<std::ofstream*>(file_stream);
                if (ofs) {
                    ofs->flush();
                    ofs->close();
                    delete ofs;
                }
            }
            addtl_log_ = nullptr;
        }
    }
    static GlobalLoggingContext& instance() { static GlobalLoggingContext context; return context; }
    static std::string& thread_context() {
        static thread_local std::string context = std::to_string(thread_counter().fetch_add(1, std::memory_order_relaxed));
        return context;
    }
    static std::atomic<uint64_t>& stdout_current_line() { static std::atomic<uint64_t> line{0}; return line; }
    static std::atomic<uint64_t>& next_ticket() { static std::atomic<uint64_t> ticket{0}; return ticket; }
    static std::atomic<uint64_t>& currently_serving() { static std::atomic<uint64_t> serving{0}; return serving; }
    static Logger::LogLevel& global_log_level() { static Logger::LogLevel level = Logger::LogLevel::TRACE; return level; }
    static std::atomic<bool>& colored_output() { static std::atomic<bool> colored{true}; return colored; }
    static std::atomic<bool>& log_format_json() { static std::atomic<bool> json_mode{false}; return json_mode; }
    static std::atomic<std::ostream*>& stdout_stream() { static std::atomic<std::ostream*> out{&std::cout}; return out; }
    static std::atomic<std::ostream*>& stderr_stream() { static std::atomic<std::ostream*> err{&std::cerr}; return err; }
    static std::atomic<std::ostream*>& addtl_log_stream() { static std::atomic<std::ostream*> addtl{nullptr}; return addtl; }
    static std::string& component() { static std::string comp; return comp; }
    static std::string& instance_name() { static std::string name; return name; }
    static std::mutex& progress_mutex() { static std::mutex mtx; return mtx; }
    static std::unordered_map<std::string, ProgressBar>& progress_bars() { static std::unordered_map<std::string, ProgressBar> bars; return bars; }
    static std::string get_timestamp(uint64_t now_ms = 0) {
        std::chrono::time_point now = std::chrono::system_clock::now();
        time_t timet = std::chrono::system_clock::to_time_t(now);
        if (now_ms == 0) {
            now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        } else {
            now = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(now_ms));
            timet = std::chrono::system_clock::to_time_t(now);
        }
        std::ostringstream oss;
        char time_buffer[20];
        std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&timet));
        oss << time_buffer << '.' << std::setfill('0') << std::setw(3) << (now_ms % 1000);
        return oss.str();
    }
    static std::string get_iso8601_timestamp(uint64_t now_ms = 0) {
        auto now = std::chrono::system_clock::now();
        if (now_ms == 0) now_ms = now.time_since_epoch().count();
        uint64_t ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        time_t timet = static_cast<time_t>(ms_since_epoch / 1000);
        std::tm tm_buffer{};
    #if LOGGING_PLATFORM_WINDOWS
        gmtime_s(&tm_buffer, &timet);
    #else
        gmtime_r(&timet, &tm_buffer);
    #endif
        char time_buffer[24];
        std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%dT%H:%M:%S", &tm_buffer);
        char out_buffer[32];
        std::snprintf(out_buffer, sizeof(out_buffer), "%s.%03dZ",
            time_buffer, static_cast<int>(ms_since_epoch % 1000));
        return std::string(out_buffer);
    }
    static void log_message(Logger::LogLevel level, const std::string& message);
    static bool log_message_route_to_compositor(const std::string& formatted_line) {
        if (!stdout_is_tty()) {
            return false;
        }
        Terminal* t = Terminal::instance_if_alive();
        if (t == nullptr) {
            return false;
        }
        std::string out = formatted_line;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
            out.pop_back();
        }
        if (out.empty()) {
            return true;
        }
        return true;
    }
    static void json_escape_into(const std::string& in, std::string& out) {
        out.reserve(out.size() + in.size() + 8);
        for (unsigned char c : in) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                        out += hex;
                    } else {
                        out += static_cast<char>(c);
                    }
                    break;
            }
        }
    }
    static std::string format_duration(uint64_t duration_ms) {
        if (duration_ms < 1000) {
            return std::to_string(duration_ms) + "ms";
        }
        else if (duration_ms < 60000) {
            return std::to_string(duration_ms / 1000) + "s";
        }
        else if (duration_ms < 3600000) {
            uint64_t minutes = duration_ms / 60000;
            uint64_t seconds = (duration_ms % 60000) / 1000;
            return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
        }
        else {
            uint64_t hours = duration_ms / 3600000;
            uint64_t minutes = (duration_ms % 3600000) / 60000;
            return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
        }
    }
    static void stdout_lock() {
        uint64_t ticket = GlobalLoggingContext::next_ticket().fetch_add(1, std::memory_order_relaxed);
        while (GlobalLoggingContext::currently_serving().load(std::memory_order_acquire) != ticket) {
            std::this_thread::yield();
        }
    }
    static void stdout_unlock() {
        GlobalLoggingContext::currently_serving().fetch_add(1, std::memory_order_release);
    }
    static void UpdateProgressBar(const ProgressBar& bar) {
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
        stdout_lock();
        std::cout << oss.str();
        std::cout.flush();
        stdout_unlock();
    }
    static bool is_claude()  {
        static const char* is_claude = std::getenv("CLAUDECODE");
        return (is_claude != nullptr);
    }
};
inline std::string RenderProgressBarLine(const ProgressBar& bar) {
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    float progress = bar.progress;
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }
    int filled_width = static_cast<int>(bar.width * progress);
    if (filled_width > bar.width) {
        filled_width = bar.width;
    }
    std::ostringstream oss;
    if (progress >= 1.0f) {
        uint64_t duration = now_ms - bar.start_time;
        oss << TTYGREY << bar.start_time_str
            << TTYBRGREEN << " [INFO] " << TTYGREEN << bar.header << " ["
            << TTYBRGREEN << std::string(bar.width, '=')
            << TTYGREEN << "] " << TTYRESET << "Completed in " << GlobalLoggingContext::format_duration(duration);
    }
    else {
        uint64_t elapsed_time = now_ms - bar.start_time;
        uint64_t estimated_time_remaining = (progress > 0.001f)
            ? static_cast<uint64_t>((static_cast<double>(elapsed_time) / progress) - elapsed_time)
            : 0;
        std::string time_remaining = GlobalLoggingContext::format_duration(estimated_time_remaining);
        oss << TTYGREY << bar.start_time_str
            << TTYBRGREEN << " [INFO] " << TTYGREEN << bar.header << " ["
            << TTYBRGREEN << std::string(filled_width, '=')
            << TTYGREY << std::string(bar.width - filled_width, '-')
            << TTYGREEN << "] ";
        char pct_buf[16];
        snprintf(pct_buf, sizeof(pct_buf), "%.1f", static_cast<double>(progress * 100.0f));
        oss << TTYRESET << pct_buf << "%"
            << TTYGREY << " est:" << time_remaining << TTYRESET;
    }
    return oss.str();
}

inline TerminalColors parse_sgr(
    const char* params, size_t plen,
    TerminalColors current
) {
    if (plen == 0) {
        return TerminalColors::DEFAULT;
    }
    int codes[16];
    int code_count = 0;
    int n = 0;
    bool have_digit = false;
    for (size_t i = 0; i <= plen; ++i) {
        const char c = (i < plen) ? params[i] : ';';
        if (c >= '0' && c <= '9') {
            n = n * 10 + (c - '0');
            have_digit = true;
        } else if (c == ';') {
            if (code_count < 16) {
                codes[code_count++] = have_digit ? n : 0;
            }
            n = 0;
            have_digit = false;
        }
    }
    bool bold = false;
    for (int i = 0; i < code_count; ++i) {
        const int code = codes[i];
        if (code == 0) {
            current = TerminalColors::DEFAULT;
            bold = false;
        } else if (code == 1) {
            bold = true;
            if (current == TerminalColors::DARK_RED)     current = TerminalColors::RED;
            else if (current == TerminalColors::DARK_GREEN)   current = TerminalColors::GREEN;
            else if (current == TerminalColors::DARK_YELLOW)  current = TerminalColors::YELLOW;
            else if (current == TerminalColors::DARK_BLUE)    current = TerminalColors::BLUE;
            else if (current == TerminalColors::DARK_MAGENTA) current = TerminalColors::MAGENTA;
            else if (current == TerminalColors::DARK_CYAN)    current = TerminalColors::CYAN;
        } else if (code == 22) {
            bold = false;
        } else if (code >= 30 && code <= 37) {
            switch (code) {
                case 30: current = TerminalColors::DARK_GRAY; break;
                case 31: current = bold ? TerminalColors::RED       : TerminalColors::DARK_RED; break;
                case 32: current = bold ? TerminalColors::GREEN     : TerminalColors::DARK_GREEN; break;
                case 33: current = bold ? TerminalColors::YELLOW    : TerminalColors::DARK_YELLOW; break;
                case 34: current = bold ? TerminalColors::BLUE      : TerminalColors::DARK_BLUE; break;
                case 35: current = bold ? TerminalColors::MAGENTA   : TerminalColors::DARK_MAGENTA; break;
                case 36: current = bold ? TerminalColors::CYAN      : TerminalColors::DARK_CYAN; break;
                case 37: current = TerminalColors::GREY; break;
            }
        } else if (code == 38) {
            if (i + 1 < code_count && codes[i + 1] == 5) {
                i += 2;
            } else if (i + 1 < code_count && codes[i + 1] == 2) {
                i += 4;
            }
        } else if (code == 39) {
            current = TerminalColors::DEFAULT;
        } else if (code >= 90 && code <= 97) {
            switch (code) {
                case 90: current = TerminalColors::DARK_GRAY; break;
                case 91: current = TerminalColors::RED;       break;
                case 92: current = TerminalColors::GREEN;     break;
                case 93: current = TerminalColors::YELLOW;    break;
                case 94: current = TerminalColors::BLUE;      break;
                case 95: current = TerminalColors::MAGENTA;   break;
                case 96: current = TerminalColors::CYAN;      break;
                case 97: current = TerminalColors::WHITE;     break;
            }
        }
    }
    return current;
}
inline void print_table(const std::vector<std::vector<std::string>>& rows, const std::vector<std::string>& final_row = {},
    const char* border_color = "\033[0m", std::vector<size_t> column_widths = {}, std::vector<size_t> final_row_widths = {}) {
    GlobalLoggingContext::stdout_lock();
    static const std::function<std::string(const char*)> color_start =
    [&](const char* color_code = TTYRESET) { if (stdout_is_tty()) return color_code; return ""; };
    static const std::function<std::string()> color_end = []() { if (stdout_is_tty()) return "\033[0m"; return ""; };
    static const std::function<void(std::vector<size_t>, const char*)> print_top_border =
    [&](std::vector<size_t> widths, const char* border_color) {
        std::stringstream ss; ss << color_start(border_color) << "┌";
        for (size_t i = 0; i < widths.size(); ++i) { for (size_t j = 0; j < widths[i]; ++j) ss << "─";
            if (i < widths.size() - 1) ss << "┬"; } ss << "┐" << color_end() << std::endl; std::cout << ss.str(); };
    static const std::function<void(std::vector<size_t>, std::vector<size_t>, const char*)> print_between_border =
    [&](std::vector<size_t> widths_above, std::vector<size_t> widths_below, const char* color_code = TTYRESET) {
        size_t total_width_above = 1; for (const auto& width : widths_above) total_width_above += width + 1;
        size_t total_width_below = 1; for (const auto& width : widths_below) total_width_below += width + 1;
        if (total_width_above != total_width_below) THROW("Widths above and below must be equal when accounting for borders");
        std::stringstream ss; size_t top_cursor = 0, bottom_cursor = 0, top_index = 0, bottom_index = 0;
        for (size_t i = 0; i < total_width_above; ++i) {
            if (i == 0) { ss << color_start(color_code) << "├"; continue; }
            if (i == total_width_above - 1) { ss << "┤" << color_end() << std::endl; continue; }
            bool top_needs_border = true, bottom_needs_border = true;
            if (top_index < widths_above[top_cursor]) top_needs_border = false;
            if (bottom_index < widths_below[bottom_cursor]) bottom_needs_border = false;
            if (top_needs_border || bottom_needs_border) {
                if (top_needs_border && bottom_needs_border) {
                    ss << "┼"; top_index = 0; bottom_index = 0; top_cursor++; bottom_cursor++;
                } else if (top_needs_border) { ss << "┴"; top_index = 0; top_cursor++; bottom_index++;
                } else { ss << "┬"; bottom_index = 0; bottom_cursor++; top_index++; }
            } else { ss << "─"; top_index++; bottom_index++; }
        } std::cout << ss.str(); };
    static const std::function<void(std::vector<size_t>, const char*)> print_bottom_border =
    [&](std::vector<size_t> column_widths, const char* color_code = TTYRESET) {
        std::stringstream ss; ss << color_start(color_code) << "└";
        for (size_t i = 0; i < column_widths.size(); ++i) {
            for (size_t j = 0; j < column_widths[i]; ++j) ss << "─";
            if (i < column_widths.size() - 1) ss << "┴";
        } ss << "┘" << color_end() << std::endl; std::cout << ss.str(); };
    static const std::function<void(std::vector<size_t>, const char*)> print_separator =
    [&](std::vector<size_t> column_widths, const char* color_code = TTYRESET) {
        std::stringstream ss; ss << color_start(color_code) << "├";
        for (size_t i = 0; i < column_widths.size(); ++i) {
            for (size_t j = 0; j < column_widths[i]; ++j) ss << "─";
            if (i < column_widths.size() - 1) ss << "┼"; }
        ss << "┤" << color_end() << std::endl; std::cout << ss.str(); };
    static const std::function<std::pair<size_t, size_t>(const std::string&)> term_size_of = [&](const std::string& str) {
        size_t actual_width = 0, current_width = 0, height = 1;
        for (size_t j = 0; j < str.size(); ++j) {
            if (str[j] == '\033') { if (j + 1 < str.size() && str[j + 1] == '[') while (j < str.size() && str[j] != 'm') ++j; }
            else if ((str[j] & 0xF8) == 0xF0) { current_width += 2; ++j; ++j; ++j; }
            else if ((str[j] & 0xF0) == 0xE0) { current_width += 1; ++j; ++j; }
            else if ((str[j] & 0xE0) == 0xC0) { current_width += 1; ++j; }
            else if (str[j] == '\n') { actual_width = std::max(actual_width, current_width); height++; current_width = 0; }
            else current_width++;
        } return std::make_pair(std::max(actual_width, current_width), height); };
    static const std::function<std::vector<std::string>(const std::string&, size_t)> wrap_cell =
    [&](const std::string& text, size_t width) {
        std::vector<std::string> lines; std::string line, word; size_t line_width = 0, word_width = 0;
        const auto flush_word = [&]() { if (word.empty()) return;
            if (line_width > 0 && line_width + 1 + word_width > width) {
                lines.push_back(line); line.clear(); line_width = 0; }
            if (line_width > 0) { line += ' '; line_width++; }
            line += word; line_width += word_width; word.clear(); word_width = 0; };
        for (size_t j = 0; j < text.size(); ++j) {
            size_t glyph_bytes = 1, glyph_width = 1;
            if (text[j] == '\033' && j + 1 < text.size() && text[j + 1] == '[') {
                while (j + glyph_bytes < text.size() && text[j + glyph_bytes - 1] != 'm') glyph_bytes++;
                glyph_width = 0;
            } else if ((text[j] & 0xF8) == 0xF0) { glyph_bytes = 4; glyph_width = 2;
            } else if ((text[j] & 0xF0) == 0xE0) glyph_bytes = 3;
            else if ((text[j] & 0xE0) == 0xC0) glyph_bytes = 2;
            else if (text[j] == '\n') { flush_word(); lines.push_back(line);
                line.clear(); line_width = 0; continue;
            } else if (text[j] == ' ') { flush_word(); continue; }
            if (word_width + glyph_width > width) flush_word();
            word += text.substr(j, glyph_bytes); word_width += glyph_width; j += glyph_bytes - 1;
        } flush_word(); lines.push_back(line); return lines; };
    static const std::function<std::string(const std::string&, size_t, bool)> pad_cell =
    [&](const std::string& text, size_t width, bool right_justify = false) {
        const size_t text_width = term_size_of(text).first;
        const std::string padding(width > text_width ? width - text_width : 0, ' ');
        return right_justify ? padding + text : text + padding; };
    static const std::function<void(const std::vector<std::string>&,
        const std::vector<size_t>&,bool,const char*)> print_row =
    [&](const std::vector<std::string>& cells, const std::vector<size_t>& column_widths,
        bool right_justify_last, const char* color_code) {
        std::vector<std::vector<std::string>> wrapped(cells.size()); size_t height = 1;
        for (size_t j = 0; j < cells.size(); ++j) {
            wrapped[j] = wrap_cell(cells[j], column_widths[j]);
            height = std::max(height, wrapped[j].size()); }
        for (size_t line = 0; line < height; ++line) {
            std::cout << color_start(color_code) << "│" << color_end();
            for (size_t j = 0; j < cells.size(); ++j) {
                const std::string text = line < wrapped[j].size() ? wrapped[j][line] : "";
                std::cout << pad_cell(text, column_widths[j], right_justify_last && j + 1 == cells.size())
                        << color_start(color_code) << "│" << color_end();
            } std::cout << std::endl; } };
    static const std::function<std::pair<std::vector<size_t>, std::vector<size_t>>(
        const std::vector<std::vector<std::string>>&, std::vector<std::string>)> get_column_widths =
    [&](const std::vector<std::vector<std::string>>& values, std::vector<std::string> final_row) {
        static const std::function<size_t(const std::vector<std::string>&)> width_including_borders =
            [](const std::vector<std::string>& column) { size_t total_width = 1;
            for (const auto& value : column) { std::pair<size_t, size_t> size = term_size_of(value);
                total_width += size.first + 1; } return total_width; };
        static const std::function<size_t(const std::vector<size_t>&)> width_including_borders_from_widths =
            [](const std::vector<size_t>& column_widths) { size_t total_width = 1;
            for (const auto& width : column_widths) total_width += width + 1;
            return total_width; };
        static const std::function<int(const std::vector<size_t>&)> check_current_widths =
            [](const std::vector<size_t>& column_widths) {
            size_t total_width = width_including_borders_from_widths(column_widths);
            if (total_width > static_cast<size_t>(get_terminal_width())) return -1;
            else if (total_width < static_cast<size_t>(get_terminal_width())) return 1;
            return 0; };
        std::vector<std::vector<std::string>> split_columns(values.size());
        std::vector<size_t> column_widths(values.size(), SIZE_MAX);
        struct ColumnStats {
            double width_sum = 0.0, width_mean = 0.0, width_stddev = 0.0;
            double width_max = 0.0, width_min = std::numeric_limits<double>::max();
        };
        std::vector<ColumnStats> column_stats(values.size());
        size_t max_rows = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            const std::vector<std::string>& column_values = values[i];
            double width_sum_of_squares = 0.0;
            for (size_t row = 0; row < column_values.size(); ++row) {
                std::pair<size_t, size_t> size = term_size_of(column_values[row]);
                const double actual_width = static_cast<double>(size.first);
                size_t this_column_max_rows = size.second;
                column_stats[i].width_sum += actual_width;
                width_sum_of_squares += actual_width * actual_width;
                column_stats[i].width_max = std::max(column_stats[i].width_max, actual_width);
                column_stats[i].width_min = std::min(column_stats[i].width_min, actual_width);
                max_rows = std::max(max_rows, this_column_max_rows);
            }
            const double count = static_cast<double>(std::max<size_t>(column_values.size(), 1));
            column_stats[i].width_mean = column_stats[i].width_sum / count;
            column_stats[i].width_stddev = std::sqrt(std::max(
                0.0, width_sum_of_squares / count - column_stats[i].width_mean * column_stats[i].width_mean));
        }
        std::vector<double> double_column_widths(column_stats.size(), 0.0);
        for (size_t i = 0; i < column_stats.size(); ++i) {
            double_column_widths[i] = std::min(column_stats[i].width_max,
                std::ceil(column_stats[i].width_mean + 2 * column_stats[i].width_stddev));
            column_widths[i] = static_cast<size_t>(std::ceil(double_column_widths[i]));
        }
        while (width_including_borders_from_widths(column_widths) > static_cast<size_t>(get_terminal_width())) {
            double current_scale_factor = 0.95; bool all_columns_smallest = true;
            for (size_t i = 0; i < column_stats.size(); ++i) {
                double_column_widths[i] *= current_scale_factor;
                column_widths[i] = std::max(static_cast<size_t>(std::ceil(double_column_widths[i])), size_t(5));
                if (column_widths[i] > 5) all_columns_smallest = false;
            }
            if (all_columns_smallest) { LOG_WARN_STREAM << "Unable to fit table within terminal width."; break; }
        }
        if (final_row.empty()) return std::make_pair(column_widths, std::vector<size_t>());
        std::vector<size_t> final_row_widths(final_row.size(), 0);
        for (size_t i = 0; i < final_row.size(); ++i) {
            std::pair<size_t, size_t> size = term_size_of(final_row[i]);
            final_row_widths[i] = size.first;
        }
        size_t current_width = width_including_borders_from_widths(column_widths);
        size_t final_row_width = width_including_borders_from_widths(final_row_widths);
        if (current_width != final_row_width) {
            std::vector<double> final_row_column_widths_double(final_row.size(), 0.0);
            for (size_t i = 0; i < final_row.size(); ++i) final_row_column_widths_double[i] = static_cast<double>(final_row_widths[i]);
            if (final_row_width > current_width) {
                double current_scale_factor = 0.95;
                while (final_row_width > current_width) {
                    bool all_columns_smallest = true;
                    for (size_t i = 0; i < final_row.size(); ++i) {
                        final_row_column_widths_double[i] *= current_scale_factor;
                        final_row_widths[i] = std::max(
                            static_cast<size_t>(std::ceil(final_row_column_widths_double[i])), size_t(5));
                        if (final_row_widths[i] > 5) all_columns_smallest = false;
                    }
                    if (all_columns_smallest) THROW("Unable to fit final row within terminal width.");
                    final_row_width = width_including_borders_from_widths(final_row_widths);
                }
            } else {
                double current_scale_factor = 1.05; // Scale up by 5% each iteration
                while (final_row_width < current_width) {
                    for (size_t i = 0; i < final_row.size(); ++i) {
                        final_row_column_widths_double[i] *= current_scale_factor;
                        final_row_widths[i] = static_cast<size_t>(std::ceil(final_row_column_widths_double[i]));
                    } final_row_width = width_including_borders_from_widths(final_row_widths); }
                while (final_row_width > current_width) {
                    size_t slackest = 0; for (size_t i = 1; i < final_row.size(); ++i) {
                        if (final_row_widths[i] - term_size_of(final_row[i]).first
                            > final_row_widths[slackest] - term_size_of(final_row[slackest]).first) slackest = i;
                    } final_row_widths[slackest]--; final_row_width--;
                }
            } final_row_widths.back() += current_width - final_row_width;
        } return std::make_pair(column_widths, final_row_widths);
    };
    if (column_widths.empty()) {
        if (final_row.empty()) std::tie(column_widths, final_row_widths) = get_column_widths(rows, {});
        else std::tie(column_widths, final_row_widths) = get_column_widths(rows, final_row);
    }
    print_top_border(column_widths, border_color); size_t row_count = 0;
    for (const auto& column : rows) row_count = std::max(row_count, column.size());
    for (size_t i = 0; i < row_count; ++i) {
        std::vector<std::string> cells(rows.size());
        for (size_t j = 0; j < rows.size(); ++j) cells[j] = i < rows[j].size() ? rows[j][i] : "";
        print_row(cells, column_widths, false, border_color);
        if (i + 1 < row_count) print_separator(column_widths, border_color);
    }
    if (!final_row.empty()) {
        print_between_border(column_widths, final_row_widths, border_color);
        print_row(final_row, final_row_widths, true, border_color);
        print_bottom_border(final_row_widths, border_color);
    } else print_bottom_border(column_widths, border_color);
    GlobalLoggingContext::stdout_unlock();
}
} // namespace threadsafe_logger::logging
