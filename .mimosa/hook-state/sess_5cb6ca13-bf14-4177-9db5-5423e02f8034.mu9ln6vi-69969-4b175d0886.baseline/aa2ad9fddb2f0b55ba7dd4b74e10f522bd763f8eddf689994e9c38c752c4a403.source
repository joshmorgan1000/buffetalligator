/** --------------------------------------------------------------------------------------------------------- Utility Tests
 * @file test_utilities.cpp
 * @brief Tests duration, escaping, UTF-8, terminal geometry, and braille helpers.
 */
#include <loggingutils.hpp>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}
void test_formatting_helpers() {
    expect(threadsafe_logger::json_escape("quote\" slash\\ line\n tab\t") ==
        "quote\\\" slash\\\\ line\\n tab\\t", "json_escape handles JSON metacharacters");
    expect(threadsafe_logger::Logger::format_duration(999) == "999ms", "milliseconds are formatted");
    expect(threadsafe_logger::Logger::format_duration(59000) == "59s", "seconds are formatted");
    expect(threadsafe_logger::Logger::format_duration(125000) == "2m 5s", "minutes are formatted");
    expect(threadsafe_logger::Logger::format_duration(7380000) == "2h 3m", "hours are formatted");
}
void test_utf8_helpers() {
    using namespace threadsafe_logger::logging;
    std::ostringstream encoded;
    encode_utf8(0x41u, encoded);
    encode_utf8(0xA2u, encoded);
    encode_utf8(0x20ACu, encoded);
    encode_utf8(0x1F642u, encoded);
    const std::string expected = "A\xC2\xA2\xE2\x82\xAC\xF0\x9F\x99\x82";
    expect(encoded.str() == expected, "encode_utf8 handles one through four byte code points");
    size_t count = 0;
    std::unique_ptr<uint32_t[]> decoded = decode_utf8(expected, count);
    expect(count == 4, "decode_utf8 reports the decoded code point count");
    expect(decoded[0] == 0x41u && decoded[1] == 0xA2u && decoded[2] == 0x20ACu &&
        decoded[3] == 0x1F642u, "decode_utf8 restores encoded code points");
}
void test_terminal_helpers() {
    using namespace threadsafe_logger::logging;
    TerminalPosition position(8.0, 12.0);
    expect(position.x_pos() == 4 && position.y_pos() == 3, "terminal positions convert to cell coordinates");
    position += 2;
    position -= 1;
    position *= 4;
    position /= 3;
    expect(position == TerminalPosition(9.0, 13.0), "terminal position arithmetic updates both axes");
    TerminalVelocity velocity(8.0, -4.0);
    velocity.dampen(0.25);
    expect(velocity.x == 2.0 && velocity.y == -1.0, "terminal velocity dampening scales both axes");
    expect(parse_sgr("31", 2, TerminalColors::DEFAULT) == TerminalColors::DARK_RED,
        "parse_sgr parses standard colors");
    expect(parse_sgr("1;34", 4, TerminalColors::DEFAULT) == TerminalColors::BLUE,
        "parse_sgr parses bold colors");
}
void test_braille_conversion() {
    using threadsafe_logger::logging::BraillePoint;
    for (unsigned int value = 0; value <= 0xFFu; ++value) {
        const uint8_t encoded = BraillePoint::unicode_to_custom(static_cast<uint8_t>(value));
        const uint8_t decoded = BraillePoint::custom_to_unicode(encoded);
        if (decoded != static_cast<uint8_t>(value)) {
            expect(false, "braille conversion round-trips all byte values");
            return;
        }
    }
}
} // namespace
int main() {
    test_formatting_helpers();
    test_utf8_helpers();
    test_terminal_helpers();
    test_braille_conversion();
    return failures == 0 ? 0 : 1;
}
