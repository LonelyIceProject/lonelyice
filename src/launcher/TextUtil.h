#ifndef LONELYICE_TEXTUTIL_H
#define LONELYICE_TEXTUTIL_H

#include <string>

namespace LonelyIce
{
    std::wstring Utf8ToWide(std::string const& s);
    std::string WideToUtf8(std::wstring const& s);
    // Output of a child console program as UTF-8 (Platform::ConsoleToUtf8).
    std::string OemToUtf8(std::string const& s);
    // First character, upper-cased for Latin and Cyrillic ("?" for empty text).
    std::string FirstLetterUpper(std::string const& s);
    std::string EscapeRml(std::string const& s);
}

#endif
