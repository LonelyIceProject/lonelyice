#include "Installer.h"
#include "ConfFile.h"
#include "ConfigEnv.h"
#include "GameClient.h"
#include "Pak.h"
#include "Plugins.h"
#include "TextUtil.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <Windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    constexpr std::size_t MaxLog = 400;

    // Rate keys the wizard's preset sets (a superset of the Settings tab's presets).
    char const* const RateKeys[] = {
        "Rate.XP.Kill", "Rate.XP.Quest", "Rate.XP.Quest.DF", "Rate.XP.Explore", "Rate.XP.Pet", "Rate.Rest.InGame",
        "Rate.Rest.Offline.InTavernOrCity", "Rate.Rest.Offline.InWilderness", "Rate.Reputation.Gain", "Rate.Honor",
        "Rate.Drop.Money", "Rate.Drop.Item.Poor", "Rate.Drop.Item.Normal", "Rate.Drop.Item.Uncommon", "Rate.Drop.Item.Rare",
        "Rate.Drop.Item.Epic", "Rate.Drop.Item.Legendary", "Rate.Drop.Item.Artifact", "Rate.Drop.Item.Referenced" };

    std::string Quoted(fs::path const& p)
    {
        return "\"" + WideToUtf8(p.wstring()) + "\"";
    }

    // Text between the first pair of ' or " quotes.
    std::string QuotedName(std::string const& line)
    {
        std::size_t a = line.find_first_of("'\"");
        if (a == std::string::npos)
            return {};
        std::size_t b = line.find(line[a], a + 1);
        return b == std::string::npos ? std::string() : line.substr(a + 1, b - a - 1);
    }

    bool CreateShortcut(fs::path const& target, fs::path const& link, std::string& error)
    {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellLinkW* sl = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))))
        {
            sl->SetPath(target.c_str());
            sl->SetWorkingDirectory(target.parent_path().c_str());
            sl->SetDescription(L"LonelyIce");
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))))
            {
                ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
                pf->Release();
            }
            sl->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        if (!ok)
            error = "не удалось создать ярлык";
        return ok;
    }
}

Installer::~Installer()
{
    Cancel();
    if (_thread.joinable())
        _thread.join();
}

void Installer::Start(InstallOptions const& options)
{
    if (_running)
        return;
    if (_thread.joinable())
        _thread.join();
    _o = options;
    _cancel = false;
    _finished = false;
    _ok = false;
    {
        std::lock_guard<std::mutex> guard(_lock);
        _error.clear();
        _log.clear();
        _steps.clear();
        auto add = [&](bool on, char const* id, char const* title)
        {
            InstallStep s{ id, title };
            if (!on)
                s.state = StepState::Skipped;
            _steps.push_back(s);
        };
        add(_o.db, "db", "Базы данных");
        add(_o.maps, "maps", "DBC и карты");
        add(_o.vmaps, "vmaps", "Модели зданий (vmaps)");
        add(_o.mmaps, "mmaps", "Навигация (mmaps)");
        add(_o.client_prep, "client", "Подготовка клиента");
    }
    _running = true;
    _thread = std::thread([this] { Run(); });
}

void Installer::Cancel()
{
    _cancel = true;
    std::lock_guard<std::mutex> guard(_lock);
    if (_child)
        TerminateProcess(_child, 1);
}

std::vector<InstallStep> Installer::Steps() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _steps;
}

std::vector<std::string> Installer::TakeLog()
{
    std::lock_guard<std::mutex> guard(_lock);
    return std::exchange(_log, {});
}

std::string Installer::Error() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _error;
}

void Installer::SetStep(std::string const& id, StepState state, float progress, std::string const& note)
{
    {
        std::lock_guard<std::mutex> guard(_lock);
        for (InstallStep& s : _steps)
        {
            if (s.id != id)
                continue;
            s.state = state;
            s.progress = std::clamp(progress, 0.f, 1.f);
            if (!note.empty() || state != StepState::Running)
                s.note = note;
        }
    }
    if (_wake)
        _wake();
}

void Installer::Progress(std::string const& id, float progress, std::string const& note)
{
    SetStep(id, StepState::Running, progress, note);
}

void Installer::Log(std::string line)
{
    {
        std::lock_guard<std::mutex> guard(_lock);
        _log.push_back(std::move(line));
        if (_log.size() > MaxLog)
            _log.erase(_log.begin(), _log.begin() + (_log.size() - MaxLog));
    }
    if (_wake)
        _wake();
}

void Installer::Fail(std::string const& id, std::string const& why)
{
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_error.empty())
            _error = why;
    }
    SetStep(id, StepState::Failed, 0.f, _cancel ? "прервано" : why);
    Log("Ошибка: " + why);
}

void Installer::Run()
{
    bool ok = true;
    struct Step { bool on; char const* id; bool (Installer::*fn)(); };
    for (Step const& s : { Step{ _o.db, "db", &Installer::RunDatabases }, Step{ _o.maps, "maps", &Installer::RunMaps },
             Step{ _o.vmaps, "vmaps", &Installer::RunVmaps }, Step{ _o.mmaps, "mmaps", &Installer::RunMmaps },
             Step{ _o.client_prep, "client", &Installer::RunClient } })
    {
        if (!s.on)
            continue;
        if (_cancel)
        {
            ok = false;
            break;
        }
        SetStep(s.id, StepState::Running, 0.f);
        if (!(this->*s.fn)())
        {
            ok = false;
            break;
        }
        SetStep(s.id, StepState::Done, 1.f, "готово");
    }
    _ok = ok && !_cancel;
    Log(_ok ? "Готово." : _cancel ? "Установка прервана." : "Установка остановлена из-за ошибки.");
    _finished = true;
    _running = false;
    if (_wake)
        _wake();
}

int Installer::RunChild(std::wstring const& args, fs::path const& dir, std::vector<std::pair<std::wstring, std::wstring>> const& env,
    std::function<void(std::string const&)> const& onLine)
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 1 << 16))
        return -1;
    // stdin stays an empty pipe: tools that wait for a key read EOF instead of hanging.
    if (!CreatePipe(&inRead, &inWrite, &sa, 0))
    {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        return -1;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    std::wstring block;
    if (!env.empty())
    {
        std::vector<std::wstring> vars;
        for (auto const& [k, v] : env)
            vars.push_back(k + L"=" + v);
        if (wchar_t* cur = GetEnvironmentStringsW())
        {
            for (wchar_t const* p = cur; *p; p += wcslen(p) + 1)
            {
                std::wstring e = p;
                std::size_t eq = e.find(L'=', 1);
                bool overridden = std::any_of(env.begin(), env.end(), [&](auto const& kv) { return eq != std::wstring::npos && _wcsicmp(e.substr(0, eq).c_str(), kv.first.c_str()) == 0; });
                if (!overridden)
                    vars.push_back(e);
            }
            FreeEnvironmentStringsW(cur);
        }
        for (std::wstring const& v : vars)
            block += v + L'\0';
        block += L'\0';
    }

    std::wstring cmd = L"\"" + _o.exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT, block.empty() ? nullptr : block.data(),
        dir.c_str(), &si, &pi);
    CloseHandle(outWrite);
    CloseHandle(inRead);
    CloseHandle(inWrite);
    if (!started)
    {
        CloseHandle(outRead);
        return -1;
    }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> guard(_lock);
        _child = pi.hProcess;
    }
    if (_cancel)
        TerminateProcess(pi.hProcess, 1);

    std::string pending;
    char buf[8192];
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(outRead, buf, sizeof(buf), &read, nullptr) || read == 0)
            break;
        pending.append(buf, read);
        std::size_t eol;
        while ((eol = pending.find_first_of("\r\n")) != std::string::npos)
        {
            std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty())
                onLine(OemToUtf8(line));
        }
    }
    if (!pending.empty())
        onLine(OemToUtf8(pending));
    CloseHandle(outRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    {
        std::lock_guard<std::mutex> guard(_lock);
        _child = nullptr;
    }
    CloseHandle(pi.hProcess);
    return int(code);
}

std::string Installer::SqlStamp(fs::path const& setupDir)
{
    std::error_code ec;
    fs::path pak = setupDir / "sql.pak";
    uint64_t size = fs::file_size(pak, ec);
    if (ec)
        return {};
    auto time = fs::last_write_time(pak, ec).time_since_epoch().count();
    return std::to_string(size) + "-" + std::to_string(time);
}

bool Installer::PrepareConfigs(InstallOptions const& o, std::string& error)
{
    fs::path configs = o.root / "configs";
    std::error_code ec;
    for (char const* d : { "configs/modules", "db", "data", "logs", "backups" })
        fs::create_directories(o.root / d, ec);

    std::vector<fs::path> created;
    bool read = Pak::Read(o.setupDir / "configs.pak", [&](std::string const& path, std::string const& data)
    {
        fs::path dist = configs / fs::u8path(path);
        fs::create_directories(dist.parent_path(), ec);
        std::ofstream(dist, std::ios::binary | std::ios::trunc).write(data.data(), std::streamsize(data.size()));
        fs::path conf = dist;
        conf.replace_extension();               // x.conf.dist -> x.conf
        if (!fs::exists(conf, ec))
        {
            fs::copy_file(dist, conf, ec);
            created.push_back(conf);
        }
        return true;
    }, error);
    if (!read)
        return false;

    // Plugin configs (the server falls back to the .dist in the plugin folder, but LonelyIce edits them).
    for (PluginManifest const& plugin : ReadPlugins(o.exe.parent_path() / "plugins"))
    {
        if (plugin.configDist.empty() || !fs::exists(plugin.configDist, ec))
            continue;
        fs::path conf = configs / "modules" / ConfigFileName(plugin);
        if (!fs::exists(conf, ec))
        {
            fs::copy_file(plugin.configDist, conf, ec);
            created.push_back(conf);
        }
    }

    for (fs::path const& conf : created)
    {
        ConfFile f;
        if (!f.Load(conf))
            continue;
        std::string name = conf.filename().string();
        if (name == "worldserver.conf")
        {
            f.Set("LoginDatabaseInfo", "sqlite:db/auth.sqlite");
            f.Set("WorldDatabaseInfo", "sqlite:db/world.sqlite");
            f.Set("CharacterDatabaseInfo", "sqlite:db/characters.sqlite");
            f.Set("DataDir", "data");
            f.Set("LogsDir", "logs");
            f.Set("SourceDirectory", "sql");
            // Updates run only while LonelyIce deploys SQL shipped with a release (it passes the flags then).
            f.Set("Updates.EnableDatabases", "0");
            f.Set("BindIP", "127.0.0.1");
            unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            f.Set("MapUpdate.Threads", std::to_string(std::clamp<int>(int(cores) - 4, 1, 8)));
            for (char const* key : RateKeys)
                f.Set(key, std::to_string(o.rate));
        }
        else if (name == "playerbots.conf")
        {
            f.Set("PlayerbotsDatabaseInfo", "sqlite:db/playerbots.sqlite;attach=characters=db/characters.sqlite");
            f.Set("Playerbots.Updates.EnableDatabases", "0");
            f.Set("AiPlayerbot.MinRandomBots", std::to_string(o.bots));
            f.Set("AiPlayerbot.MaxRandomBots", std::to_string(o.bots));
            for (char const* key : { "AiPlayerbot.CombatStrategies", "AiPlayerbot.NonCombatStrategies",
                     "AiPlayerbot.RandomBotCombatStrategies", "AiPlayerbot.RandomBotNonCombatStrategies" })
                f.Set(key, "+tactics");
        }
        if (!f.Save())
        {
            error = "не удалось записать " + WideToUtf8(conf.wstring());
            return false;
        }
    }
    return true;
}

bool Installer::RunDatabases()
{
    std::string error;
    if (!PrepareConfigs(_o, error))
    {
        Fail("db", error);
        return false;
    }

    // Unpack the SQL where SourceDirectory = "sql" points; remember sizes to turn "Applying x.sql" lines into progress.
    fs::path sql = _o.root / "sql";
    std::error_code ec;
    fs::remove_all(sql, ec);
    std::map<std::string, uint64_t> sizes;
    uint64_t total = 0;
    Log("Распаковка SQL…");
    bool unpacked = Pak::Read(_o.setupDir / "sql.pak", [&](std::string const& path, std::string const& data)
    {
        fs::path file = sql / fs::u8path(path);
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary | std::ios::trunc).write(data.data(), std::streamsize(data.size()));
        sizes[file.filename().string()] = data.size();
        total += data.size();
        return !_cancel;
    }, error, [&](uint64_t done, uint64_t all) { Progress("db", 0.1f * float(done) / float(std::max<uint64_t>(all, 1)), "распаковка SQL"); });
    if (!unpacked || _cancel)
    {
        Fail("db", unpacked ? "прервано" : error);
        return false;
    }
    for (char const* d : { "custom/db_auth", "custom/db_characters", "custom/db_world", "updates/pending_db_auth",
             "updates/pending_db_characters", "updates/pending_db_world" })
        fs::create_directories(sql / "data" / "sql" / d, ec);
    Log("SQL: " + std::to_string(sizes.size()) + " файлов, " + std::to_string(total >> 20) + " МБ");

    std::vector<std::pair<std::wstring, std::wstring>> env = {
        { L"AC_DISABLE_INTERACTIVE", L"1" },
        { L"AC_PLUGINS_DIR", (_o.exe.parent_path() / "plugins").wstring() },
        { Utf8ToWide(EnvName("Updates.EnableDatabases")), L"7" },
        { Utf8ToWide(EnvName("Playerbots.Updates.EnableDatabases")), L"1" },
        { L"LONELYICE_REALMNAME", Utf8ToWide(_o.realmName) } };
    if (!_o.login.empty())
        env.push_back({ L"LONELYICE_ACCOUNT", Utf8ToWide(_o.login + "\t" + _o.password + "\t" + std::to_string(_o.gmLevel)) });

    uint64_t applied = 0;
    std::string current = "создание баз";
    bool deployed = false;
    std::string failure;
    int rc = RunChild(L"--server --deploy -c \"" + (_o.root / "configs" / "worldserver.conf").wstring() + L"\"", _o.root, env,
        [&](std::string const& line)
        {
            if (line.rfind("@@LI ", 0) == 0)
            {
                if (line.find("deploy ok") != std::string::npos)
                    deployed = true;
                else if (line.find("failed") != std::string::npos)
                    failure = line.substr(5);
                return;
            }
            if (line.find(">> Applying") != std::string::npos)
            {
                std::string name = QuotedName(line);
                auto it = sizes.find(name);
                if (it != sizes.end())
                    applied += it->second;
                Progress("db", 0.1f + 0.9f * float(applied) / float(std::max<uint64_t>(total, 1)), current);
                return;
            }
            if (line.find("Updating ") != std::string::npos && line.find("database") != std::string::npos)
                current = "обновления: " + line.substr(line.find("Updating ") + 9);
            else if (line.find("is empty, auto populating") != std::string::npos)
                current = "заполнение: " + line.substr(line.find("Database ") + 9, line.find(" is empty") - line.find("Database ") - 9);
            if (line.find("ERROR") != std::string::npos || line.find("Creating database") != std::string::npos || line.find("Account") != std::string::npos)
                Log(line);
        });
    if (rc != 0 || !deployed)
    {
        Fail("db", _cancel ? "прервано" : "сервер не смог создать базы (" + (failure.empty() ? "код " + std::to_string(rc) : failure) + "), подробности в logs\\");
        return false;
    }
    fs::remove_all(sql, ec);
    Log("Базы данных готовы.");
    return true;
}

bool Installer::RunMaps()
{
    fs::path data = _o.root / "data";
    int done = 0, total = 0;
    int rc = RunChild(L"--tool maps \"" + _o.client.wstring() + L"\" \"" + data.wstring() + L"\"", _o.root, {},
        [&](std::string const& line)
        {
            // "Extract <name> (12/144)"
            if (line.rfind("Extract ", 0) == 0 && line.back() == ')')
            {
                std::size_t p = line.rfind('('), s = line.rfind('/');
                if (p != std::string::npos && s != std::string::npos && s > p)
                {
                    done = atoi(line.c_str() + p + 1);
                    total = atoi(line.c_str() + s + 1);
                    _mapCount = total;
                    Progress("maps", 0.1f + 0.9f * float(done) / float(std::max(total, 1)), "карта " + std::to_string(done) + " из " + std::to_string(total));
                }
                return;
            }
            if (line.rfind("Processing", 0) == 0)
                return;
            if (line.find("DBC") != std::string::npos || line.find("locale") != std::string::npos || line.find("camera") != std::string::npos || line.rfind("@@LI fail", 0) == 0)
            {
                Log(line);
                Progress("maps", 0.05f, "DBC и камеры");
            }
        });
    if (rc != 0)
    {
        Fail("maps", "map_extractor завершился с кодом " + std::to_string(rc));
        return false;
    }
    Log("Карты извлечены: " + std::to_string(total));
    return true;
}

bool Installer::RunVmaps()
{
    fs::path data = _o.root / "data";
    int maps = 0, total = _mapCount > 0 ? _mapCount : 140;
    int rc = RunChild(L"--tool vmaps \"" + _o.client.wstring() + L"\" \"" + data.wstring() + L"\"", data, {},
        [&](std::string const& line)
        {
            if (line.rfind("Processing Map", 0) == 0)
            {
                ++maps;
                Progress("vmaps", 0.75f * float(std::min(maps, total)) / float(total), "модели карты " + std::to_string(maps) + " из " + std::to_string(total));
            }
            else if (line.find("GameObject") != std::string::npos || line.rfind("@@LI fail", 0) == 0 || line.find("rror") != std::string::npos)
                Log(line);
        });
    if (rc != 0)
    {
        Fail("vmaps", "vmap4_extractor завершился с кодом " + std::to_string(rc));
        return false;
    }

    int trees = 0;
    rc = RunChild(L"--tool assemble \"" + data.wstring() + L"\"", data, {},
        [&](std::string const& line)
        {
            if (line.rfind("Creating map tree", 0) == 0)
            {
                ++trees;
                Progress("vmaps", 0.75f + 0.25f * float(std::min(trees, total)) / float(total), "сборка тайлов");
            }
            else if (line.rfind("@@LI fail", 0) == 0)
                Log(line);
        });
    if (rc != 0)
    {
        Fail("vmaps", "сборка vmaps завершилась с кодом " + std::to_string(rc));
        return false;
    }
    Log("vmaps готовы.");
    return true;
}

bool Installer::RunMmaps()
{
    fs::path data = _o.root / "data";
    int rc = RunChild(L"--tool mmaps \"" + data.wstring() + L"\" " + std::to_wstring(std::max(1, _o.threads)), data, {},
        [&](std::string const& line)
        {
            // "37% [Map 0571] Building tile [31,22]"
            std::size_t pct = line.find("% [Map ");
            if (pct != std::string::npos && pct > 0 && pct < 4)
            {
                std::size_t m = pct + 7;
                Progress("mmaps", float(atoi(line.c_str())) / 100.f, "карта " + line.substr(m, line.find(']', m) - m) + ", " + line.substr(0, pct) + " %");
                return;
            }
            if (line.find("We have") != std::string::npos || line.find("threads") != std::string::npos || line.rfind("@@LI fail", 0) == 0)
                Log(line);
        });
    if (rc != 0)
    {
        Fail("mmaps", "mmaps_generator завершился с кодом " + std::to_string(rc));
        return false;
    }
    Log("mmaps готовы.");
    return true;
}

bool Installer::RunClient()
{
    ClientInfo info = GameClient::Inspect(_o.client);
    if (!info.valid)
    {
        Fail("client", "клиент не найден");
        return false;
    }
    std::string error;
    if (_o.realmlist)
    {
        if (!GameClient::WriteRealmlist(info, "127.0.0.1", {}, error))
        {
            Fail("client", error);
            return false;
        }
        Log("realmlist.wtf → 127.0.0.1");
    }
    Progress("client", 0.4f);
    if (_o.clearWdb)
    {
        GameClient::ClearWdb(_o.client);
        Log("Кэш клиента очищен");
    }
    // Client addons of the installed plugins.
    std::error_code ec;
    for (PluginManifest const& plugin : ReadPlugins(_o.exe.parent_path() / "plugins"))
    {
        for (fs::path const& addon : plugin.addons)
        {
            fs::path dst = _o.client / "Interface" / "AddOns" / addon.filename();
            fs::create_directories(dst, ec);
            fs::copy(addon, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            Log(ec ? "Аддон " + addon.filename().string() + " не скопирован: " + ec.message() : "Аддон " + addon.filename().string());
        }
    }
    if (_o.accountName && !_o.login.empty())
    {
        GameClient::SetConfigValue(_o.client, "accountName", _o.login);
        Log("Config.wtf: accountName " + _o.login);
    }
    Progress("client", 0.8f);
    if (_o.shortcut)
    {
        wchar_t* desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        {
            if (CreateShortcut(_o.exe, fs::path(desktop) / L"LonelyIce.lnk", error))
                Log("Ярлык на рабочем столе");
            else
                Log(error);
            CoTaskMemFree(desktop);
        }
    }
    return true;
}
