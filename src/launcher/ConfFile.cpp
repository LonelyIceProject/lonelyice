#include "ConfFile.h"
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
    }
}

bool LonelyIce::ConfFile::Load(fs::path const& file)
{
    _path = file;
    _lines.clear();
    _cr.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        _loaded = false;
        return false;
    }
    std::string line;
    while (std::getline(in, line))
    {
        bool cr = !line.empty() && line.back() == '\r';
        if (cr)
        {
            line.pop_back();
            _crlf = true;
        }
        _lines.push_back(line);
        _cr.push_back(cr);
    }
    _loaded = true;
    return true;
}

bool LonelyIce::ConfFile::Save() const
{
    std::ofstream out(_path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    // each line keeps its own ending; appended lines follow the file's style
    for (std::size_t i = 0; i < _lines.size(); ++i)
        out << _lines[i] << ((i < _cr.size() ? _cr[i] : _crlf) ? "\r\n" : "\n");
    return bool(out);
}

int LonelyIce::ConfFile::Find(std::string const& key) const
{
    for (int i = 0; i < int(_lines.size()); ++i)
    {
        std::string t = Trim(_lines[i]);
        if (t.empty() || t[0] == '#' || t[0] == '[')
            continue;
        std::size_t eq = t.find('=');
        if (eq != std::string::npos && Trim(t.substr(0, eq)) == key)
            return i;
    }
    return -1;
}

std::optional<std::string> LonelyIce::ConfFile::Get(std::string const& key) const
{
    int i = Find(key);
    if (i < 0)
        return std::nullopt;
    std::string v = Trim(_lines[i].substr(_lines[i].find('=') + 1));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return v;
}

void LonelyIce::ConfFile::Set(std::string const& key, std::string const& value)
{
    int i = Find(key);
    if (i < 0)
    {
        _lines.push_back(key + " = " + value);
        return;
    }
    std::string& line = _lines[i];
    std::size_t eq = line.find('=');
    std::string old = Trim(line.substr(eq + 1));
    bool quoted = old.size() >= 2 && old.front() == '"';
    line = line.substr(0, eq + 1) + " " + (quoted ? "\"" + value + "\"" : value);
}
