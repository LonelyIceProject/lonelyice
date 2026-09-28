#include "GameClient.h"
#include "TextUtil.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <TlHelp32.h>

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

    std::string FileVersion(fs::path const& exe)
    {
        DWORD dummy = 0;
        DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
        if (!size)
            return {};
        std::vector<char> data(size);
        if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()))
            return {};
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
            return {};
        return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
            std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
    }

    fs::path RegistryInstallPath()
    {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        for (wchar_t const* key : { L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment\\World of Warcraft", L"SOFTWARE\\Blizzard Entertainment\\World of Warcraft" })
        {
            size = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
                return fs::path(buf);
        }
        return {};
    }
}

bool LonelyIce::GameClient::IsClientDir(fs::path const& dir)
{
    std::error_code ec;
    return !dir.empty() && fs::exists(dir / "Wow.exe", ec) && fs::exists(dir / "Data" / "common.MPQ", ec);
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

    fs::path reg = RegistryInstallPath();
    if (IsClientDir(reg))
        return reg;

    return {};
}

LonelyIce::ClientInfo LonelyIce::GameClient::Inspect(fs::path const& dir)
{
    ClientInfo info;
    info.dir = dir;
    if (!IsClientDir(dir))
        return info;

    info.valid = true;
    info.version = FileVersion(dir / "Wow.exe");

    std::error_code ec;
    for (fs::directory_iterator it(dir / "Data", ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
            continue;
        std::string name = it->path().filename().string();
        if (name.size() != 4 || !fs::exists(it->path() / ("locale-" + name + ".MPQ"), ec))
            continue;
        info.locales.push_back({ name, ReadRealmlist(it->path() / "realmlist.wtf") });
    }
    return info;
}

bool LonelyIce::GameClient::WriteRealmlist(ClientInfo const& info, std::string const& host, std::string& error)
{
    for (ClientLocale const& loc : info.locales)
    {
        fs::path file = info.dir / "Data" / loc.name / "realmlist.wtf";
        fs::path bak = file;
        bak += ".bak";

        std::error_code ec;
        if (fs::exists(file, ec) && !fs::exists(bak, ec))
            fs::copy_file(file, bak, ec);

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "Не удалось записать " + WideToUtf8(file.wstring());
            return false;
        }
        out << "set realmlist " << host << "\r\n";
    }

    // Config.wtf may carry its own realmList that wins over realmlist.wtf.
    fs::path config = info.dir / "WTF" / "Config.wtf";
    std::ifstream in(config, std::ios::binary);
    if (in)
    {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::istringstream lines(text);
        std::string line, result;
        bool changed = false;
        while (std::getline(lines, line))
        {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.rfind("set realmlist ", 0) == 0)
            {
                line = "SET realmList \"" + host + "\"\r";
                changed = true;
            }
            result += line + "\n";
        }
        if (changed)
            std::ofstream(config, std::ios::binary | std::ios::trunc) << result;
    }
    return true;
}

void LonelyIce::GameClient::ClearWdb(fs::path const& dir)
{
    std::error_code ec;
    fs::remove_all(dir / "Cache" / "WDB", ec);
}

bool LonelyIce::GameClient::Launch(fs::path const& dir, std::string& error)
{
    std::wstring exe = (dir / "Wow.exe").wstring();
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
    {
        error = "Не удалось запустить Wow.exe (код " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool LonelyIce::GameClient::IsRunning(fs::path const& dir)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    std::wstring want = (dir / "Wow.exe").wstring();
    bool found = false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    {
        if (_wcsicmp(pe.szExeFile, L"Wow.exe") != 0)
            continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p)
            continue;
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &len) && _wcsicmp(path, want.c_str()) == 0)
            found = true;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}
