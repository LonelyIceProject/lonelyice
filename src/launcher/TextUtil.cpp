#include "TextUtil.h"
#include <Windows.h>

namespace
{
    std::wstring ToWide(std::string const& s, UINT cp)
    {
        if (s.empty())
            return {};
        int n = MultiByteToWideChar(cp, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(cp, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }
}

std::wstring LonelyIce::Utf8ToWide(std::string const& s)
{
    return ToWide(s, CP_UTF8);
}

std::string LonelyIce::WideToUtf8(std::wstring const& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string LonelyIce::OemToUtf8(std::string const& s)
{
    return WideToUtf8(ToWide(s, CP_OEMCP));
}

std::string LonelyIce::EscapeRml(std::string const& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        // "{{" would start a data binding inside the data model scope
        if (c == '{' && i + 1 < s.size() && s[i + 1] == '{')
        {
            out += "{ ";
            continue;
        }
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}
