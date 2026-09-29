#include "GameClient.h"
#include "Lang.h"
#include "Platform.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
    std::string ReadRealmlist(fs::path const& file)
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream s(line);
            std::string set, key, value;
            s >> set >> key;
            std::transform(set.begin(), set.end(), set.begin(), ::tolower);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if (set == "set" && key == "realmlist")
            {
                std::getline(s, value);
                value.erase(0, value.find_first_not_of(" \t\""));
                value.erase(value.find_last_not_of(" \t\r\"") + 1);
                return value;
            }
        }
        return {};
    }

    // File version from the PE resource (VS_FIXEDFILEINFO, found by its signature); the same on every platform.
    std::string FileVersion(fs::path const& exe)
    {
        std::ifstream in(exe, std::ios::binary);
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
        static unsigned char const signature[] = { 0xBD, 0x04, 0xEF, 0xFE };
        auto it = std::search(data.begin(), data.end(), std::begin(signature), std::end(signature));
        if (it == data.end() || data.end() - it < 16)
            return {};
        auto word = [&](std::size_t offset) { return unsigned(it[offset]) | unsigned(it[offset + 1]) << 8; };
        // dwFileVersionMS at +8, dwFileVersionLS at +12 (little-endian: low word first)
        return std::to_string(word(10)) + "." + std::to_string(word(8)) + "." + std::to_string(word(14)) + "." + std::to_string(word(12));
    }

    // Where a Windows installer would have put the game.
    std::vector<fs::path> InstallPaths()
    {
        std::vector<fs::path> out;
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        for (wchar_t const* key : { L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment\\World of Warcraft", L"SOFTWARE\\Blizzard Entertainment\\World of Warcraft" })
        {
            DWORD size = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
                out.emplace_back(buf);
        }
#else
        // the default Wine prefix
        if (auto home = Platform::GetEnv("HOME"))
            for (char const* dir : { "Program Files (x86)", "Program Files" })
                out.push_back(fs::path(*home) / ".wine" / "drive_c" / dir / "World of Warcraft");
#endif
        return out;
    }
}

// Linux and macOS file systems may be case-sensitive while the client's names come from Windows ("Data", "data").
fs::path LonelyIce::GameClient::Child(fs::path const& dir, std::string const& name)
{
    std::error_code ec;
    fs::path const exact = dir / name;
    if (fs::exists(exact, ec))
        return exact;
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; };
    std::string const want = lower(name);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (lower(Platform::PathToUtf8(it->path().filename())) == want)
            return it->path();
    return exact;
}
bool LonelyIce::GameClient::IsClientDir(fs::path const& dir)
{
    std::error_code ec;
    return !dir.empty() && fs::exists(Child(dir, "Wow.exe"), ec) && fs::exists(Child(Child(dir, "Data"), "common.MPQ"), ec);
}

fs::path LonelyIce::GameClient::Detect(fs::path const& saved, fs::path const& exeDir)
{
    if (IsClientDir(saved))
        return saved;
    if (IsClientDir(exeDir))
        return exeDir;

    fs::path parent = exeDir.parent_path();
    if (IsClientDir(parent))
        return parent;

    std::error_code ec;
    for (fs::directory_iterator a(parent, ec), end; !ec && a != end; a.increment(ec))
    {
        if (!a->is_directory(ec) || a->path() == exeDir)
            continue;
        if (IsClientDir(a->path()))
            return a->path();
        std::error_code ec2;
        for (fs::directory_iterator b(a->path(), ec2); !ec2 && b != end; b.increment(ec2))
            if (b->is_directory(ec2) && IsClientDir(b->path()))
                return b->path();
    }

    for (fs::path const& p : InstallPaths())
        if (IsClientDir(p))
            return p;

    return {};
}

LonelyIce::ClientInfo LonelyIce::GameClient::Inspect(fs::path const& dir)
{
    ClientInfo info;
    info.dir = dir;
    if (!IsClientDir(dir))
        return info;

    info.valid = true;
    info.version = FileVersion(Child(dir, "Wow.exe"));

    std::error_code ec;
    for (fs::directory_iterator it(Child(dir, "Data"), ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
            continue;
        std::string name = it->path().filename().string();
        if (name.size() != 4 || !fs::exists(it->path() / ("locale-" + name + ".MPQ"), ec))
            continue;
        info.locales.push_back({ name, ReadRealmlist(Child(it->path(), "realmlist.wtf")) });
    }
    return info;
}

namespace
{
    // Rewrites "SET <key> ..." lines of WTF\Config.wtf (case-insensitive key); appends the line if missing.
    bool SetConfigWtf(fs::path const& dir, std::string const& key, std::string const& value, bool appendIfMissing)
    {
        fs::path config = LonelyIce::GameClient::Child(LonelyIce::GameClient::Child(dir, "WTF"), "Config.wtf");
        std::string text;
        {
            std::ifstream in(config, std::ios::binary);
            if (in)
                text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            else if (!appendIfMissing)
                return true;
        }

        std::string prefix = "set " + key + " ";
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
        std::istringstream lines(text);
        std::string line, result;
        bool found = false;
        while (std::getline(lines, line))
        {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.rfind(prefix, 0) == 0)
            {
                line = "SET " + key + " \"" + value + "\"\r";
                found = true;
            }
            result += line + "\n";
        }
        if (!found)
        {
            if (!appendIfMissing)
                return true;
            result += "SET " + key + " \"" + value + "\"\r\n";
        }

        std::error_code ec;
        fs::create_directories(config.parent_path(), ec);
        std::ofstream out(config, std::ios::binary | std::ios::trunc);
        out << result;
        return bool(out);
    }
}

std::string LonelyIce::GameClient::ReadConfigLocale(fs::path const& dir)
{
    std::ifstream in(Child(Child(dir, "WTF"), "Config.wtf"));
    std::string line;
    while (std::getline(in, line))
    {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.rfind("set locale ", 0) != 0)
            continue;
        std::size_t a = line.find('"'), b = line.rfind('"');
        if (a != std::string::npos && b > a)
            return line.substr(a + 1, b - a - 1);
    }
    return {};
}

bool LonelyIce::GameClient::SetConfigLocale(fs::path const& dir, std::string const& locale)
{
    return SetConfigWtf(dir, "locale", locale, true);
}

bool LonelyIce::GameClient::SetConfigValue(fs::path const& dir, std::string const& key, std::string const& value)
{
    return SetConfigWtf(dir, key, value, true);
}

bool LonelyIce::GameClient::WriteRealmlist(ClientInfo const& info, std::string const& host, std::vector<std::string> const& locales, std::string& error)
{
    for (ClientLocale const& loc : info.locales)
    {
        if (!locales.empty() && std::find(locales.begin(), locales.end(), loc.name) == locales.end())
            continue;
        fs::path file = Child(Child(Child(info.dir, "Data"), loc.name), "realmlist.wtf");
        fs::path bak = file;
        bak += ".bak";

        std::error_code ec;
        if (fs::exists(file, ec) && !fs::exists(bak, ec))
            fs::copy_file(file, bak, ec);

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = Tr("client.error.write", Platform::PathToUtf8(file));
            return false;
        }
        out << "set realmlist " << host << "\r\n";
    }

    // Config.wtf may carry its own realmList that wins over realmlist.wtf.
    SetConfigWtf(info.dir, "realmList", host, false);
    return true;
}

void LonelyIce::GameClient::ClearWdb(fs::path const& dir)
{
    std::error_code ec;
    fs::remove_all(Child(Child(dir, "Cache"), "WDB"), ec);
}

bool LonelyIce::GameClient::Launch(fs::path const& dir, std::string const& runner, std::string& error, std::unique_ptr<Platform::Child>& process)
{
    Platform::ChildOptions o;
    o.workDir = dir;
    o.pipes = false;
#ifdef _WIN32
    (void)runner;
    o.exe = Child(dir, "Wow.exe");
#else
    // Wow.exe runs through Wine (or what [client] runner names, arguments separated by spaces).
    std::istringstream words(runner.empty() ? std::string("wine") : runner);
    for (std::string w; words >> w;)
    {
        if (o.exe.empty())
            o.exe = w;
        else
            o.args.push_back(w);
    }
    o.args.push_back(Platform::PathToUtf8(Child(dir, "Wow.exe")));
#endif
    auto child = std::make_unique<Platform::Child>();
    std::string why;
    if (!child->Start(o, why))
    {
        error = Tr("client.error.start", why);
        return false;
    }
    process = std::move(child);
    return true;
}

bool LonelyIce::GameClient::IsRunning(fs::path const& dir)
{
    return Platform::IsProcessRunning(Child(dir, "Wow.exe"));
}