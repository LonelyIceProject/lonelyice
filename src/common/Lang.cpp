#include "Lang.h"
#include "Assets.h"
#include <shared_mutex>
#include <unordered_map>
#include <Windows.h>

using namespace LonelyIce;

namespace
{
    using Table = std::unordered_map<std::string, std::string>;

    std::shared_mutex _lock;     // Set may run while worker threads translate
    std::string _code = "en";
    Table _current, _english;

    Table LoadTable(std::string const& code)
    {
        Table t;
        for (std::string const& file : ListAssets("lang/" + code + "/"))
        {
            std::string text;
            if (!ReadAsset(file, text))
                continue;
            if (text.rfind("\xEF\xBB\xBF", 0) == 0)
                text.erase(0, 3);
            std::size_t pos = 0;
            while (pos < text.size())
            {
                std::size_t end = text.find('\n', pos);
                std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? text.size() : end + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::size_t const first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line[first] == '#')
                    continue;
                std::size_t const eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string key = line.substr(first, eq - first);
                key.erase(key.find_last_not_of(" \t") + 1);
                std::string value = line.substr(eq + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                std::string out;
                for (std::size_t i = 0; i < value.size(); ++i)
                {
                    if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n')
                    {
                        out += '\n';
                        ++i;
                    }
                    else
                        out += value[i];
                }
                t[key] = std::move(out);
            }
        }
        return t;
    }

    bool Known(std::string const& code)
    {
        for (Lang::Info const& i : Lang::Available())
            if (code == i.code)
                return true;
        return false;
    }
}

std::vector<Lang::Info> const& Lang::Available()
{
    // Each language names itself in its files (lang.self).
    static std::vector<Info> const list = []
    {
        std::vector<Info> out;
        for (char const* code : { "en", "de", "es", "fr", "ru" })
        {
            Table const t = LoadTable(code);
            auto it = t.find("lang.self");
            out.push_back({ code, it != t.end() ? it->second : code });
        }
        return out;
    }();
    return list;
}

void Lang::Init()
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA("LONELYICE_LANG", buf, sizeof(buf));
    std::string code = n > 0 && n < sizeof(buf) ? buf : "";
    if (code.empty())
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring ini = exe;
        ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"lonelyice.ini";
        wchar_t value[16] = {};
        GetPrivateProfileStringW(L"launcher", L"language", L"", value, 16, ini.c_str());
        for (wchar_t const* c = value; *c; ++c)
            code += char(*c);
    }
    Set(code);
}

void Lang::Set(std::string const& code)
{
    std::string const c = Known(code) ? code : "en";
    Table english = LoadTable("en");
    Table current = c == "en" ? Table() : LoadTable(c);
    {
        std::unique_lock<std::shared_mutex> guard(_lock);
        _code = c;
        _english = std::move(english);
        _current = std::move(current);
    }
    SetEnvironmentVariableA("LONELYICE_LANG", c.c_str());
}

std::string Lang::Code()
{
    std::shared_lock<std::shared_mutex> guard(_lock);
    return _code;
}

std::string Lang::Get(std::string_view key)
{
    std::string const k(key);
    std::shared_lock<std::shared_mutex> guard(_lock);
    if (auto it = _current.find(k); it != _current.end())
        return it->second;
    if (auto it = _english.find(k); it != _english.end())
        return it->second;
    return k;
}

bool Lang::Has(std::string_view key)
{
    std::string const k(key);
    std::shared_lock<std::shared_mutex> guard(_lock);
    return _current.count(k) || _english.count(k);
}

std::string Lang::Pick(std::map<std::string, std::string> const& texts)
{
    std::shared_lock<std::shared_mutex> guard(_lock);
    if (auto it = texts.find(_code); it != texts.end())
        return it->second;
    if (auto it = texts.find("en"); it != texts.end())
        return it->second;
    return texts.empty() ? std::string() : texts.begin()->second;
}
