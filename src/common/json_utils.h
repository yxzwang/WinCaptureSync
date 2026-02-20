#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace wcs::json {

inline std::string Escape(const std::string& input) {
    std::ostringstream out;
    for (unsigned char ch : input) {
        switch (ch) {
            case '\"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20U) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

inline std::string Quote(const std::string& input) {
    return "\"" + Escape(input) + "\"";
}

}  // namespace wcs::json
