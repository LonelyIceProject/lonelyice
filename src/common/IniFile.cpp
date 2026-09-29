#include "IniFile.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>

using namespace LonelyIce;

namespace
{
    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        return s;
    }

    bool SameName(std::string const& a, std::string const& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
    }

    // UTF-16 LE text (after its BOM) to UTF-8.
    std::string FromUtf16(std::string const& raw)
    {
        std::string out;
        for (std::size_t i = 2; i + 1 < raw.size(); i += 2)
        {
            uint32_t c = uint8_t(raw[i]) | uint32_t(uint8_t(raw[i + 1])) << 8;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size())
            {
                uint32_t const lo = uint8_t(raw[i + 2]) | uint32_t(uint8_t(raw[i + 3])) << 8;
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
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
        return out;
    }

    // "[name]" -> name, else empty
    std::string SectionOf(std::string const& line)
    {
        std::string const t = Trim(line);
        return t.size() >= 2 && t.front() == '[' && t.back() == ']' ? Trim(t.substr(1, t.size() - 2)) : std::string();
    }
}

bool IniFile::Load(std::filesystem::path const& path)
{
    _lines.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::string text((std::istreambuf_iterator<char>(in)), {});
    if (text.size() >= 2 && uint8_t(text[0]) == 0xFF && uint8_t(text[1]) == 0xFE)
        text = FromUtf16(text);
    else if (text.rfind("\xEF\xBB\xBF", 0) == 0)
        text.erase(0, 3);
    for (std::size_t pos = 0; pos < text.size();)
    {
        std::size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        _lines.push_back(std::move(line));
        pos = end == std::string::npos ? text.size() : end + 1;
    }
    return true;
}

bool IniFile::Save(std::filesystem::path const& path) const
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (std::string const& l : _lines)
        out << l << "\n";
    return bool(out);
}

std::optional<std::string> IniFile::Get(std::string const& section, std::string const& key) const
{
    bool in = false;
    for (std::string const& l : _lines)
    {
        std::string const s = SectionOf(l);
        if (!s.empty())
        {
            in = SameName(s, section);
            continue;
        }
        if (!in)
            continue;
        std::string const t = Trim(l);
        if (t.empty() || t[0] == ';' || t[0] == '#')
            continue;
        std::size_t const eq = t.find('=');
        if (eq != std::string::npos && SameName(Trim(t.substr(0, eq)), key))
            return Trim(t.substr(eq + 1));
    }
    return std::nullopt;
}

std::string IniFile::Get(std::string const& section, std::string const& key, std::string const& def) const
{
    return Get(section, key).value_or(def);
}

void IniFile::Set(std::string const& section, std::string const& key, std::string const& value)
{
    std::string const line = key + "=" + value;
    bool in = false;
    std::size_t lastInSection = std::string::npos;
    for (std::size_t i = 0; i < _lines.size(); ++i)
    {
        std::string const s = SectionOf(_lines[i]);
        if (!s.empty())
        {
            in = SameName(s, section);
            if (in)
                lastInSection = i;
            continue;
        }
        if (!in)
            continue;
        std::string const t = Trim(_lines[i]);
        if (!t.empty())
            lastInSection = i;
        std::size_t const eq = t.find('=');
        if (!t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos && SameName(Trim(t.substr(0, eq)), key))
        {
            _lines[i] = line;
            return;
        }
    }
    if (lastInSection != std::string::npos)
        _lines.insert(_lines.begin() + std::ptrdiff_t(lastInSection) + 1, line);
    else
    {
        _lines.push_back("[" + section + "]");
        _lines.push_back(line);
    }
}

void IniFile::Remove(std::string const& section, std::string const& key)
{
    bool in = false;
    for (std::size_t i = 0; i < _lines.size(); ++i)
    {
        std::string const s = SectionOf(_lines[i]);
        if (!s.empty())
        {
            in = SameName(s, section);
            continue;
        }
        std::string const t = Trim(_lines[i]);
        std::size_t const eq = t.find('=');
        if (in && !t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos && SameName(Trim(t.substr(0, eq)), key))
            _lines.erase(_lines.begin() + std::ptrdiff_t(i--));
    }
}

// The section's header and every line up to the next section.
void IniFile::RemoveSection(std::string const& section)
{
    bool in = false;
    for (std::size_t i = 0; i < _lines.size(); ++i)
    {
        std::string const s = SectionOf(_lines[i]);
        if (!s.empty())
            in = SameName(s, section);
        if (in)
            _lines.erase(_lines.begin() + std::ptrdiff_t(i--));
    }
}
