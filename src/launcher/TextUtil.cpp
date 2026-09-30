#include "TextUtil.h"
#include "Platform.h"
#include <algorithm>

namespace
{
    // Code points of UTF-8 text; invalid bytes become U+FFFD.
    std::u32string Decode(std::string const& s)
    {
        std::u32string out;
        for (std::size_t i = 0; i < s.size();)
        {
            unsigned char const c = s[i];
            int const len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (!len || i + len > s.size())
            {
                out += U'\xFFFD';
                ++i;
                continue;
            }
            char32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
            for (int k = 1; k < len; ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            out += cp;
            i += len;
        }
        return out;
    }

    void Encode(char32_t c, std::string& out)
    {
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
        {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
        else
        {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
}

std::wstring LonelyIce::Utf8ToWide(std::string const& s)
{
    std::wstring w;
    for (char32_t c : Decode(s))
    {
        if (sizeof(wchar_t) == 2 && c >= 0x10000)
        {
            w += wchar_t(0xD800 + ((c - 0x10000) >> 10));
            w += wchar_t(0xDC00 + ((c - 0x10000) & 0x3FF));
        }
        else
            w += wchar_t(c);
    }
    return w;
}

std::string LonelyIce::WideToUtf8(std::wstring const& w)
{
    std::string out;
    for (std::size_t i = 0; i < w.size(); ++i)
    {
        char32_t c = char32_t(w[i]);
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < w.size())
            c = 0x10000 + ((c - 0xD800) << 10) + (char32_t(w[++i]) - 0xDC00);
        Encode(c, out);
    }
    return out;
}

std::string LonelyIce::OemToUtf8(std::string const& s)
{
    return Platform::ConsoleToUtf8(s);
}

std::string LonelyIce::FirstLetterUpper(std::string const& s)
{
    std::u32string const text = Decode(s);
    if (text.empty())
        return "?";
    char32_t c = text[0];
    if ((c >= U'a' && c <= U'z') || (c >= U'а' && c <= U'я') || (c >= U'à' && c <= U'þ' && c != U'÷'))
        c -= 0x20;      // Latin, Cyrillic, Latin-1 letters
    else if (c == U'ё')
        c = U'Ё';  // ё
    std::string out;
    Encode(c, out);
    return out;
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
// The wizard's rule, within the core's limits (AccountMgr: login up to 17, password up to 16 characters).
bool LonelyIce::ValidAccountName(std::string const& s)
{
    return !s.empty() && s.size() <= 16 && std::all_of(s.begin(), s.end(), [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

// Not empty, up to 16 characters, no spaces (the console command splits at them).
bool LonelyIce::ValidAccountPassword(std::string const& s)
{
    std::u32string const text = Decode(s);
    return !text.empty() && text.size() <= 16 && text.find_first_of(U" \t\r\n") == std::u32string::npos;
}
