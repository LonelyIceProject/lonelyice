#ifndef LONELYICE_TEXTUTIL_H
#define LONELYICE_TEXTUTIL_H

#include <string>

namespace LonelyIce
{
    std::wstring Utf8ToWide(std::string const& s);
    std::string WideToUtf8(std::wstring const& s);
    std::string OemToUtf8(std::string const& s);
    std::string EscapeRml(std::string const& s);
}

#endif
