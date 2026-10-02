#include "Installer.h"
#include "ClientPatch.h"
#include "ConfFile.h"
#include "LauncherRuntime.h"
#include "ProfileConfig.h"
#include "ConfigEnv.h"
#include "GameClient.h"
#include "Lang.h"
#include "Pak.h"
#include "Platform.h"
#include "Plugins.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

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

    // Text between the first pair of ' or " quotes.
    std::string QuotedName(std::string const& line)
    {
        std::size_t a = line.find_first_of("'\"");
        if (a == std::string::npos)
            return {};
        std::size_t b = line.find(line[a], a + 1);
        return b == std::string::npos ? std::string() : line.substr(a + 1, b - a - 1);
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
        auto add = [&](bool on, std::string const& id)
        {
            InstallStep s{ id, Tr("install.step." + id) };
            if (!on)
                s.state = StepState::Skipped;
            _steps.push_back(s);
        };
        add(_o.db, "db");
        add(_o.unpack, "unpack");
        add(_o.pack, "pack");
        add(_o.vmaps, "vmaps");
        add(_o.mmaps, "mmaps");
        add(_o.client_prep, "client");
    }
    _running = true;
    _thread = std::thread([this] { Run(); });
}

void Installer::Cancel()
{
    _cancel = true;
    std::lock_guard<std::mutex> guard(_lock);
    if (_child)
        _child->Kill();
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
    SetStep(id, StepState::Failed, 0.f, _cancel ? Tr("install.note.aborted") : why);
    Log(Tr("install.log.error", why));
}

void Installer::Run()
{
    bool ok = true;
    try
    {
        std::string error;
        ok = PrepareConfigs(_o, error);
        if (!ok)
            Fail("db", error);
        struct Step { bool on; char const* id; bool (Installer::*fn)(); };
        for (Step const& s : { Step{ _o.db, "db", &Installer::RunDatabases }, Step{ _o.unpack, "unpack", &Installer::RunUnpack },
                 Step{ _o.pack, "pack", &Installer::RunPack }, Step{ _o.vmaps, "vmaps", &Installer::RunVmaps },
                 Step{ _o.mmaps, "mmaps", &Installer::RunMmaps }, Step{ _o.client_prep, "client", &Installer::RunClient } })
        {
            if (!ok)
                break;
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
            SetStep(s.id, StepState::Done, 1.f, Tr("install.note.done"));
        }
    }
    catch (std::exception const& exception)
    {
        ok = false;
        Fail("db", exception.what());
    }
    _ok = ok && !_cancel;
    Log(Tr(_ok ? "install.log.done" : _cancel ? "install.log.aborted" : "install.log.failed"));
    _finished = true;
    _running = false;
    if (_wake)
        _wake();
}

int Installer::RunChild(std::vector<std::string> const& args, fs::path const& dir, Platform::Env const& env,
    std::function<void(std::string const&)> const& onLine)
{
    Platform::ChildOptions o;
    o.exe = _o.exe;
    o.args = args;
    o.workDir = dir;
    o.env = env;
    if (!args.empty() && args.front() == "--server")
    {
        LauncherSettings settings;
        settings.file = _o.profileFile.empty() ? _o.exe.parent_path() / "server.yaml" : _o.profileFile;
        std::string error;
        if (!settings.Load(&error))
        {
            Log(error);
            return -1;
        }
        settings.dataRoot = _o.root;
        settings.clientPath = _o.client;
        settings.location = _o.location;
        settings.dataCache = _o.cache;
        settings.remote = _o.remote;
        if (!_o.realmName.empty())
            settings.realmName = _o.realmName;
        LaunchContext context;
        if (!PrepareLaunch(settings, context, error, false))
        {
            Log(error);
            return -1;
        }
        o.env = std::move(context.env);
        if (_o.rateChanged)
            for (char const* key : RateKeys)
                o.env.emplace_back(EnvName(key), std::to_string(_o.rate));
        if (_o.botsChanged)
            for (char const* key : { "AiPlayerbot.MinRandomBots", "AiPlayerbot.MaxRandomBots" })
                o.env.emplace_back(EnvName(key), std::to_string(_o.bots));
        o.env.insert(o.env.end(), env.begin(), env.end());
    }
    o.lowPriority = true;
    o.pipes = true;
    Platform::Child child;
    std::string error;
    if (!child.Start(o, error))
        return -1;
    // stdin is closed at once: tools that wait for a key read EOF instead of hanging.
    child.CloseInput();
    {
        std::lock_guard<std::mutex> guard(_lock);
        _child = &child;
    }
    if (_cancel)
        child.Kill();

    std::string pending;
    char buf[8192];
    for (;;)
    {
        std::size_t read = child.Read(buf, sizeof(buf));
        if (read == 0)
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
                onLine(Platform::ConsoleToUtf8(line));
        }
    }
    if (!pending.empty())
        onLine(Platform::ConsoleToUtf8(pending));

    child.Wait(-1);
    int code = child.ExitCode();
    {
        std::lock_guard<std::mutex> guard(_lock);
        _child = nullptr;
    }
    return code;
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

std::string Installer::LoginPort(fs::path const& root)
{
    ConfFile f;
    f.Load(root / ".runtime" / "configs" / "worldserver.conf");
    return f.Get("RealmServerPort").value_or("3724");
}

bool Installer::PrepareConfigs(InstallOptions const& o, std::string& error)
{
    LauncherSettings settings;
    settings.file = o.profileFile.empty() ? o.exe.parent_path() / "server.yaml" : o.profileFile;
    if (!settings.Load(&error))
        return false;
    settings.dataRoot = o.root;
    settings.clientPath = o.client;
    settings.location = o.location;
    settings.dataCache = o.cache;
    settings.remote = o.remote;
    if (!GenerateRuntimeConfig(settings, error))
        return false;

    ProfileConfig profile;
    if (!profile.Load(settings.file, error))
        return false;
    // Preserve a received profile's choices on first deployment unless the wizard explicitly changes them.
    if (o.rateChanged || o.newDatabases)
    {
        ConfFile world;
        if (!world.Load(o.root / ".runtime" / "configs" / "worldserver.conf"))
        {
            error = "Could not open generated server configuration";
            return false;
        }
        for (char const* key : RateKeys)
            if (o.rateChanged || !profile.Get({ "server", "settings", key }))
                world.Set(key, std::to_string(o.rate));
        if (!world.Save())
        {
            error = "Could not write generated server configuration";
            return false;
        }
    }
    if (o.botsChanged || o.newDatabases)
    {
        ConfFile bots;
        if (bots.Load(o.root / ".runtime" / "configs" / "modules" / "playerbots.conf"))
        {
            for (char const* key : { "AiPlayerbot.MinRandomBots", "AiPlayerbot.MaxRandomBots" })
                if (o.botsChanged || !profile.Get({ "plugins", "playerbots", "settings", key }))
                    bots.Set(key, std::to_string(o.bots));
            if (!bots.Save())
            {
                error = "Could not write generated playerbots configuration";
                return false;
            }
        }
    }
    return true;
}

bool Installer::SaveProfileOptions(InstallOptions const& o, std::string& error)
{
    ProfileConfig profile;
    fs::path const file = o.profileFile.empty() ? o.exe.parent_path() / "server.yaml" : o.profileFile;
    if (!profile.Load(file, error))
        return false;
    if (o.rateChanged || o.newDatabases)
        for (char const* key : RateKeys)
            if (o.rateChanged || !profile.Get({ "server", "settings", key }))
                profile.SetEffective({ "server", "settings", key }, o.rate);
    if (o.botsChanged || o.newDatabases)
    {
        if (profile.Get({ "plugins", "playerbots" }))
            for (char const* key : { "AiPlayerbot.MinRandomBots", "AiPlayerbot.MaxRandomBots" })
                if (o.botsChanged || !profile.Get({ "plugins", "playerbots", "settings", key }))
                    profile.SetEffective({ "plugins", "playerbots", "settings", key }, o.bots);
    }
    return profile.Save(error);
}

bool Installer::RunDatabases()
{
    std::string error;
    // Unpack the SQL where SourceDirectory = "sql" points; remember sizes to turn "Applying x.sql" lines into progress.
    fs::path sql = _o.root / "sql";
    std::error_code ec;
    fs::remove_all(sql, ec);
    std::map<std::string, uint64_t> sizes;
    uint64_t total = 0;
    Log(Tr("install.log.unpack"));
    bool unpacked = Pak::Read(_o.setupDir / "sql.pak", [&](std::string const& path, std::string const& data)
    {
        fs::path file = sql / Platform::Utf8ToPath(path);
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary | std::ios::trunc).write(data.data(), std::streamsize(data.size()));
        sizes[Platform::PathToUtf8(file.filename())] = data.size();
        total += data.size();
        return !_cancel;
    }, error, [&](uint64_t done, uint64_t all) { Progress("db", 0.1f * float(done) / float(std::max<uint64_t>(all, 1)), Tr("install.note.unpack")); });
    if (!unpacked || _cancel)
    {
        Fail("db", unpacked ? Tr("install.note.aborted") : error);
        return false;
    }
    for (char const* d : { "custom/db_auth", "custom/db_characters", "custom/db_world", "updates/pending_db_auth",
             "updates/pending_db_characters", "updates/pending_db_world" })
        fs::create_directories(sql / "data" / "sql" / d, ec);
    Log(Tr("install.log.sql", sizes.size(), Tr("unit.mb", total >> 20)));

    Platform::Env env = {
        { "AC_DISABLE_INTERACTIVE", "1" },
        { "AC_PLUGINS_DIR", Platform::PathToUtf8(_o.exe.parent_path() / "plugins") },
        { EnvName("Updates.EnableDatabases"), "7" },
        { EnvName("Playerbots.Updates.EnableDatabases"), "1" },
        { "LONELYICE_CLIENT", Platform::PathToUtf8(_o.client) } };
    if (!_o.realmName.empty())
        env.push_back({ "LONELYICE_REALMNAME", _o.realmName });
    if (!_o.login.empty())
        env.push_back({ "LONELYICE_ACCOUNT", _o.login + "\t" + _o.password + "\t" + std::to_string(_o.gmLevel) });
    env.insert(env.end(), _o.serverEnv.begin(), _o.serverEnv.end());

    uint64_t applied = 0;
    std::string current = Tr("install.note.create_db");
    bool deployed = false;
    std::string failure;
    int rc = RunChild({ "--server", "--deploy", "-c", Platform::PathToUtf8(_o.root / ".runtime" / "configs" / "worldserver.conf") }, _o.root, env,
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
                current = Tr("install.note.updates", line.substr(line.find("Updating ") + 9));
            else if (line.find("is empty, auto populating") != std::string::npos)
                current = Tr("install.note.populate", line.substr(line.find("Database ") + 9, line.find(" is empty") - line.find("Database ") - 9));
            if (line.find("ERROR") != std::string::npos || line.find("Creating database") != std::string::npos || line.find("Account") != std::string::npos)
                Log(line);
        });
    if (rc != 0 || !deployed)
    {
        // "state failed <why>" or "deploy failed <what>": the last word says which part failed
        std::string const what = failure.substr(failure.find_last_of(' ') + 1);
        std::string const key = what == "database" || what == "config" || what == "account" || what == "patches" || what == "client"
            ? "install.error.deploy." + what : std::string();
        Fail("db", _cancel ? Tr("install.note.aborted") : !key.empty() ? Tr(key)
            : Tr("install.error.deploy", failure.empty() ? Tr("install.error.code", rc) : failure));
        return false;
    }
    fs::remove_all(sql, ec);
    Log(Tr("install.log.db_ready"));
    return true;
}

// Maps and cameras into data/, the DBC files into dbc_* tables of the world database.
bool Installer::RunUnpack()
{
    fs::path data = _o.root / "data";
    int done = 0, total = 0;
    int rc = 0;
    {
        // the server's tables must be the stock ones, without the plugins' client patches
        ClientPatch::HiddenArchives hidden(_o.client);
        // 5: maps and cameras; the DBC files go to the database below
        rc = RunChild({ "--tool", "maps", Platform::PathToUtf8(_o.client), Platform::PathToUtf8(data), "5" }, _o.root, {},
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
                        Progress("unpack", 0.05f + 0.65f * float(done) / float(std::max(total, 1)), Tr("install.note.map", done, total));
                    }
                    return;
                }
                if (line.rfind("Processing", 0) == 0)
                    return;
                if (line.find("locale") != std::string::npos || line.find("camera") != std::string::npos || line.rfind("@@LI fail", 0) == 0)
                    Log(line);
            });
    }
    if (rc != 0)
    {
        Fail("unpack", Tr("install.error.exit_code", "map_extractor", rc));
        return false;
    }
    Log(Tr("install.log.maps", total));

    // DBC files the old layout extracted next to the maps are not read any more; the maps are the extractor's now,
    // not built tiles of a stamped client.
    std::error_code ec;
    fs::remove_all(data / "dbc", ec);
    fs::remove(data / "maps" / "stamp.txt", ec);
    return RunDbcTables("fill", "unpack", 0.7f, 1.f);
}

// Back to reading the client: the unpacked DBC tables and files go (built terrain tiles take the maps' place).
bool Installer::RunPack()
{
    if (!RunDbcTables("drop", "pack", 0.f, 0.8f))
        return false;
    std::error_code ec;
    fs::path const data = _o.root / "data";
    for (char const* dir : { "dbc", "Cameras", "maps" })
        fs::remove_all(data / dir, ec);
    Log(Tr("install.log.packed"));
    return true;
}

// LonelyIce --server --dbc fill|drop on the world database.
bool Installer::RunDbcTables(std::string const& action, std::string const& step, float from, float to)
{
    Platform::Env env = {
        { "AC_DISABLE_INTERACTIVE", "1" },
        { "AC_PLUGINS_DIR", Platform::PathToUtf8(_o.exe.parent_path() / "plugins") },
        { "LONELYICE_CLIENT", Platform::PathToUtf8(_o.client) },
        { "LONELYICE_LOCALE", _o.locale } };
    env.insert(env.end(), _o.serverEnv.begin(), _o.serverEnv.end());

    bool finished = false;
    std::string failure;
    Progress(step, from, Tr(action == "fill" ? "install.note.dbc" : "install.note.dbc_drop"));
    int rc = RunChild({ "--server", "--dbc", action, "-c", Platform::PathToUtf8(_o.root / ".runtime" / "configs" / "worldserver.conf") }, _o.root, env,
        [&](std::string const& line)
        {
            // "@@LI dbc <done> <total> <table>"
            if (line.rfind("@@LI dbc ", 0) == 0)
            {
                int d = 0, t = 0;
                if (std::sscanf(line.c_str() + 9, "%d %d", &d, &t) == 2 && t > 0)
                    Progress(step, from + (to - from) * float(d) / float(t), Tr("install.note.dbc_table", d, t));
                else if (line.find("dbc ok") != std::string::npos)
                    finished = true;
                else if (line.find("failed") != std::string::npos)
                    failure = line.substr(5);
                return;
            }
            if (line.rfind("@@LI ", 0) == 0)
            {
                if (line.find("failed") != std::string::npos)
                    failure = line.substr(5);
                return;
            }
            if (line.find("ERROR") != std::string::npos || line.find("DBC") != std::string::npos)
                Log(line);
        });
    if (rc != 0 || !finished)
    {
        Fail(step, _cancel ? Tr("install.note.aborted") : Tr("install.error.dbc", failure.empty() ? Tr("install.error.code", rc) : failure));
        return false;
    }
    return true;
}

// Terrain tiles built from the client into data/maps, the way the server builds them (the mmaps generator reads them).
bool Installer::RunTiles(std::string const& step, float to)
{
    fs::path data = _o.root / "data";
    int rc = RunChild({ "--tool", "tiles", Platform::PathToUtf8(_o.client), Platform::PathToUtf8(data), _o.locale }, _o.root, {},
        [&](std::string const& line)
        {
            int d = 0, t = 0;
            if (std::sscanf(line.c_str(), "@@LI tiles %d %d", &d, &t) == 2 && t > 0)
                Progress(step, to * float(d) / float(t), Tr("install.note.tiles", d, t));
            else if (line.rfind("@@LI fail", 0) == 0)
                Log(line);
        });
    if (rc != 0)
    {
        Fail(step, Tr("install.error.exit_code", "tiles", rc));
        return false;
    }
    return true;
}

bool Installer::RunVmaps()
{
    fs::path data = _o.root / "data";
    int maps = 0, total = _mapCount > 0 ? _mapCount : 140;
    ClientPatch::HiddenArchives hidden(_o.client);
    int rc = RunChild({ "--tool", "vmaps", Platform::PathToUtf8(_o.client), Platform::PathToUtf8(data) }, data, {},
        [&](std::string const& line)
        {
            if (line.rfind("Processing Map", 0) == 0)
            {
                ++maps;
                Progress("vmaps", 0.75f * float(std::min(maps, total)) / float(total), Tr("install.note.vmap", maps, total));
            }
            else if (line.find("GameObject") != std::string::npos || line.rfind("@@LI fail", 0) == 0 || line.find("rror") != std::string::npos)
                Log(line);
        });
    if (rc != 0)
    {
        Fail("vmaps", Tr("install.error.exit_code", "vmap4_extractor", rc));
        return false;
    }

    int trees = 0;
    rc = RunChild({ "--tool", "assemble", Platform::PathToUtf8(data) }, data, {},
        [&](std::string const& line)
        {
            if (line.rfind("Creating map tree", 0) == 0)
            {
                ++trees;
                Progress("vmaps", 0.75f + 0.25f * float(std::min(trees, total)) / float(total), Tr("install.note.assemble"));
            }
            else if (line.rfind("@@LI fail", 0) == 0)
                Log(line);
        });
    if (rc != 0)
    {
        Fail("vmaps", Tr("install.error.assemble", rc));
        return false;
    }
    Log(Tr("install.log.vmaps_ready"));
    return true;
}

bool Installer::RunMmaps()
{
    fs::path data = _o.root / "data";
    // The generator reads every terrain tile: reading the client, they are built first.
    float const tiles = _o.cache ? 0.f : 0.1f;
    if (tiles > 0.f && !RunTiles("mmaps", tiles))
        return false;

    int rc = RunChild({ "--tool", "mmaps", Platform::PathToUtf8(data), std::to_string(std::max(1, _o.threads)) }, data, {},
        [&](std::string const& line)
        {
            // "37% [Map 0571] Building tile [31,22]"
            std::size_t pct = line.find("% [Map ");
            if (pct != std::string::npos && pct > 0 && pct < 4)
            {
                std::size_t m = pct + 7;
                Progress("mmaps", tiles + (1.f - tiles) * float(atoi(line.c_str())) / 100.f,
                    Tr("install.note.mmap", line.substr(m, line.find(']', m) - m), line.substr(0, pct)));
                return;
            }
            if (line.find("We have") != std::string::npos || line.find("threads") != std::string::npos || line.rfind("@@LI fail", 0) == 0)
                Log(line);
        });
    if (rc != 0)
    {
        Fail("mmaps", Tr("install.error.exit_code", "mmaps_generator", rc));
        return false;
    }
    Log(Tr("install.log.mmaps_ready"));
    return true;
}

bool Installer::RunClient()
{
    ClientInfo info = GameClient::Inspect(_o.client);
    if (!info.valid)
    {
        Fail("client", Tr("install.error.no_client"));
        return false;
    }
    std::string error;
    if (_o.realmlist)
    {
        std::string const host = GameClient::LocalRealmlist(LoginPort(_o.root));
        if (!GameClient::WriteRealmlist(info, host, {}, error))
        {
            Fail("client", error);
            return false;
        }
        Log("realmlist.wtf: " + host);
    }
    Progress("client", 0.4f);
    if (_o.clearWdb)
    {
        GameClient::ClearWdb(_o.client);
        Log(Tr("install.log.wdb"));
    }
    // Addons of the installed plugins (their client patches were built by the database step).
    ClientPatch::Result sync = ClientPatch::SyncAddons(_o.client, ReadPlugins(_o.exe.parent_path() / "plugins"));
    for (std::string const& line : sync.log)
        Log(line);
    if (!sync.ok)
    {
        Fail("client", sync.error);
        return false;
    }
    if (_o.accountName && !_o.login.empty())
    {
        GameClient::SetConfigValue(_o.client, "accountName", _o.login);
        Log("Config.wtf: accountName " + _o.login);
    }
    Progress("client", 0.8f);
    if (_o.shortcut && Platform::DesktopShortcutsSupported())
    {
        if (Platform::CreateDesktopShortcut("LonelyIce", _o.exe, {}, {}, error))
            Log(Tr("install.log.shortcut"));
        else
            Log(error.empty() ? Tr("install.error.shortcut") : error);
    }
    return true;
}
