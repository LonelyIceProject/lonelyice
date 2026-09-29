#include "Wizard.h"
#include "Lang.h"
#include "Platform.h"
#include "UiBackend.h"
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <thread>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    char const* const StepKeys[] = { "wizard.step.client", "wizard.step.place", "wizard.step.components", "wizard.step.world",
        "wizard.step.game", "wizard.step.install", "wizard.step.done" };
    constexpr int StepCount = 7;

    std::string Utf8(fs::path const& p)
    {
        return Platform::PathToUtf8(p);
    }

    uint64_t FreeBytes(fs::path p)
    {
        std::error_code ec;
        while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p.parent_path() != p)
            p = p.parent_path();
        fs::space_info info = fs::space(p, ec);
        if (ec || info.available == static_cast<std::uintmax_t>(-1))
            return 0;
        return uint64_t(info.available);
    }

#ifndef _WIN32
    // Per-user application data: ~/Library/Application Support/LonelyIce on macOS; $XDG_DATA_HOME/LonelyIce or
    // ~/.local/share/LonelyIce elsewhere. Empty when HOME is not set.
    fs::path UserDataDir()
    {
        std::optional<std::string> home = Platform::GetEnv("HOME");
#ifdef __APPLE__
        if (!home || home->empty())
            return {};
        return Platform::Utf8ToPath(*home) / "Library" / "Application Support" / "LonelyIce";
#else
        std::optional<std::string> xdg = Platform::GetEnv("XDG_DATA_HOME");
        if (xdg && !xdg->empty() && Platform::Utf8ToPath(*xdg).is_absolute())
            return Platform::Utf8ToPath(*xdg) / "LonelyIce";
        if (!home || home->empty())
            return {};
        return Platform::Utf8ToPath(*home) / ".local" / "share" / "LonelyIce";
#endif
    }
#endif

    // "6.0 GB": one decimal, with the language's decimal separator (unit.decimal).
    std::string GbText(double gb)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", gb);
        std::string s = buf;
        std::string sep = Tr("unit.decimal");
        if (sep.size() == 1)
            std::replace(s.begin(), s.end(), '.', sep[0]);
        return Tr("unit.gb", s);
    }

    std::string Gb(uint64_t bytes)
    {
        return GbText(double(bytes) / double(1ull << 30));
    }

    bool SameText(std::string const& a, std::string const& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
    }

    // Entries of dir; with ext (".MPQ"), only those with that extension, whatever its case.
    uint32_t CountFiles(fs::path const& dir, std::string const& ext = {})
    {
        std::error_code ec;
        uint32_t n = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (ext.empty() || SameText(Platform::PathToUtf8(it->path().extension()), ext))
                ++n;
        return n;
    }

    bool Writable(fs::path const& dir)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path probe = dir / ".lonelyice-write-test";
        {
            std::ofstream out(probe);
            if (!out)
                return false;
        }
        fs::remove(probe, ec);
        return true;
    }

    bool ValidLogin(std::string const& s)
    {
        return !s.empty() && s.size() <= 16 && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
    }
}

Wizard::Wizard(Host host) : _host(std::move(host)), _installer([w = _host.wake] { if (w) w(); })
{
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    // A third of the logical CPUs stays free for the rest of the system (4 of 12 cores with SMT).
    _threads = std::to_string(std::max(1u, cores - cores / 3));
    _coresNote = Tr("wizard.cores_note", cores);
    // Where the desktop has no shortcuts the option is hidden (wz_lnk_supported) and stays off.
    _lnkSupported = Platform::DesktopShortcutsSupported();
    if (!_lnkSupported)
        _lnk = false;
}

Wizard::~Wizard() = default;

void Wizard::Relocalize()
{
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    _coresNote = Tr("wizard.cores_note", cores);
    // Lists are built once the wizard has been opened; the client is not inspected again while installing.
    if (!_places.empty())
    {
        if (!_installer.IsRunning())
            Inspect(_clientInput);
        BuildPlaces();
        BuildComponents();
    }
    if (!_done.empty())
        BuildDone();
    RefreshFooter();
    Dirty();
}

void Wizard::Bind(Rml::DataModelConstructor& c)
{
    if (auto s = c.RegisterStruct<CheckRow>())
    {
        s.RegisterMember("cls", &CheckRow::cls);
        s.RegisterMember("text", &CheckRow::text);
        s.RegisterMember("detail", &CheckRow::detail);
    }
    c.RegisterArray<std::vector<CheckRow>>();
    if (auto s = c.RegisterStruct<PlaceRow>())
    {
        s.RegisterMember("id", &PlaceRow::id);
        s.RegisterMember("title", &PlaceRow::title);
        s.RegisterMember("path", &PlaceRow::path);
        s.RegisterMember("free", &PlaceRow::free);
        s.RegisterMember("detail", &PlaceRow::detail);
    }
    c.RegisterArray<std::vector<PlaceRow>>();
    if (auto s = c.RegisterStruct<CompRow>())
    {
        s.RegisterMember("id", &CompRow::id);
        s.RegisterMember("title", &CompRow::title);
        s.RegisterMember("desc", &CompRow::desc);
        s.RegisterMember("size", &CompRow::size);
        s.RegisterMember("note", &CompRow::note);
        s.RegisterMember("on", &CompRow::on);
        s.RegisterMember("locked", &CompRow::locked);
    }
    c.RegisterArray<std::vector<CompRow>>();
    if (auto s = c.RegisterStruct<StepRow>())
    {
        s.RegisterMember("name", &StepRow::name);
        s.RegisterMember("cls", &StepRow::cls);
    }
    c.RegisterArray<std::vector<StepRow>>();
    if (auto s = c.RegisterStruct<InstRow>())
    {
        s.RegisterMember("title", &InstRow::title);
        s.RegisterMember("note", &InstRow::note);
        s.RegisterMember("state", &InstRow::state);
        s.RegisterMember("pct", &InstRow::pct);
    }
    c.RegisterArray<std::vector<InstRow>>();
    c.RegisterArray<std::vector<Rml::String>>();

    c.Bind("wz_open", &_open);
    c.Bind("wz_step", &_step);
    c.Bind("wz_steps", &_steps);
    c.Bind("wz_counter", &_counter);
    c.Bind("wz_error", &_error);
    c.Bind("wz_next_label", &_nextLabel);
    c.Bind("wz_cancel_label", &_cancelLabel);
    c.Bind("wz_next_ok", &_nextOk);
    c.Bind("wz_back_visible", &_backVisible);
    c.Bind("wz_client_path", &_clientPath);
    c.Bind("wz_client_title", &_clientTitle);
    c.Bind("wz_client_found", &_clientFound);
    c.Bind("wz_checks", &_checks);
    c.Bind("wz_place", &_place);
    c.Bind("wz_places", &_places);
    c.Bind("wz_comps", &_comps);
    c.Bind("wz_threads", &_threads);
    c.Bind("wz_cores_note", &_coresNote);
    c.Bind("wz_total", &_total);
    c.Bind("wz_realm", &_realm);
    c.Bind("wz_rate", &_rate);
    c.Bind("wz_bots", &_bots);
    c.Bind("wz_login", &_login);
    c.Bind("wz_pass", &_pass);
    c.Bind("wz_gm", &_gm);
    c.Bind("wz_rl", &_rl);
    c.Bind("wz_wdb", &_wdb);
    c.Bind("wz_accname", &_accName);
    c.Bind("wz_lnk", &_lnk);
    c.Bind("wz_lnk_supported", &_lnkSupported);
    c.Bind("wz_rl_note", &_rlNote);
    c.Bind("wz_inst", &_inst);
    c.Bind("wz_log", &_log);
    c.Bind("wz_done", &_done);

    auto on = [&](char const* name, std::function<void()> fn)
    {
        c.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { fn(); });
    };
    on("wz_next", [this] { Next(); });
    on("wz_back", [this] { Back(); });
    on("wz_cancel", [this] { Cancel(); });
    on("wz_browse_client", [this] { BrowseFolder(true); });
    on("wz_browse_place", [this] { BrowseFolder(false); });
    c.BindEventCallback("wz_pick_place", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const& args)
    {
        if (args.empty())
            return;
        _place = args[0].Get<Rml::String>();
        if (_place == "custom" && _customPlace.empty())
            BrowseFolder(false);
        BuildComponents();
        Dirty();
    });
}

void Wizard::Dirty()
{
    if (_model)
        _model.DirtyAllVariables();
}

void Wizard::Error(std::string text)
{
    _error = std::move(text);
    Dirty();
}

void Wizard::Open(fs::path const& client)
{
    if (_installer.IsRunning())
    {
        _open = true;
        Go(5);
        return;
    }
    _open = true;
    _customPlace.clear();
    Inspect(client);
    fs::path root = _host.currentRoot();
    std::error_code ec;
    if (fs::exists(root / "configs" / "worldserver.conf", ec) && !fs::equivalent(root, _host.exeDir, ec))
        _customPlace = root;
    _place = !_customPlace.empty() ? "custom" : (fs::exists(_host.exeDir / "configs" / "worldserver.conf", ec) ? "exe" : "client");
#ifndef _WIN32
    fs::path user = UserDataDir();
    if (!_customPlace.empty() && !user.empty() && fs::equivalent(_customPlace, user, ec))
    {
        _customPlace.clear();
        _place = "user";
    }
#endif
    BuildPlaces();
    BuildComponents();
    Go(0);
}

void Wizard::Inspect(fs::path const& client)
{
    _clientInput = client;
    _client = client.empty() ? ClientInfo{} : GameClient::Inspect(client);
    _checks.clear();
    _clientFound = _client.valid;
    if (!_client.valid)
    {
        _clientTitle = Tr("wizard.client.not_found");
        _clientPath = client.empty() ? Tr("wizard.client.pick") : Utf8(client);
        _checks.push_back({ "bad", Tr("wizard.check.files"), Tr("wizard.check.files_missing") });
        return;
    }
    std::error_code ec;
    bool nextToExe = fs::equivalent(_client.dir, _host.exeDir, ec) || fs::equivalent(_client.dir, _host.exeDir.parent_path(), ec);
    _clientTitle = nextToExe ? Tr("wizard.client.found_near_exe") : Tr("wizard.client.found");
    _clientPath = Utf8(_client.dir);

    bool build = _client.version == "3.3.5.12340";
    _checks.push_back({ build ? "ok" : "bad", Tr("wizard.check.version"), _client.version.empty() ? Tr("wizard.check.version_unread")
        : Tr("wizard.check.build", _client.version.substr(_client.version.rfind('.') + 1)) });
    uint32_t mpq = CountFiles(GameClient::Child(_client.dir, "Data"), ".MPQ");
    _checks.push_back({ mpq >= 4 ? "ok" : "bad", Tr("wizard.check.mpq"), Tr("wizard.check.mpq_count", mpq) });
    for (ClientLocale const& l : _client.locales)
        _checks.push_back({ "ok", Tr("wizard.check.locale", l.name), Utf8(fs::path("Data") / Platform::Utf8ToPath(l.name)) });
    if (_client.locales.empty())
        _checks.push_back({ "bad", Tr("wizard.check.locale_title"), Tr("wizard.check.locale_none") });
    bool writable = Writable(GameClient::Child(_client.dir, "WTF"));
    _checks.push_back({ writable ? "ok" : "bad", Tr("wizard.check.write"), writable ? Tr("wizard.check.write_ok") : Tr("wizard.check.write_denied") });
    bool running = GameClient::IsRunning(_client.dir);
    _checks.push_back({ running ? "warn" : "ok", running ? Tr("wizard.check.game_running") : Tr("wizard.check.game_stopped"),
        running ? Tr("wizard.check.game_close") : "" });

    std::string list;
    for (ClientLocale const& l : _client.locales)
        list += (list.empty() ? "" : ", ") + l.name;
    _rlNote = list;
}

fs::path Wizard::PlacePath() const
{
    if (_place == "client")
        return _client.valid ? _client.dir / "LonelyIce" : fs::path();
    if (_place == "exe")
        return _host.exeDir;
#ifndef _WIN32
    if (_place == "user")
        return UserDataDir();
#endif
    return _customPlace;
}

void Wizard::BuildPlaces()
{
    _places.clear();
    // texts: wizard.place.<key>.title / .detail
    auto add = [&](std::string const& id, fs::path const& p, std::string const& key)
    {
        _places.push_back({ id, Tr("wizard.place." + key + ".title"), p.empty() ? Tr("wizard.place.none") : Utf8(p),
            p.empty() ? "" : Tr("wizard.place.free", Gb(FreeBytes(p))), Tr("wizard.place." + key + ".detail") });
    };
    add("client", _client.valid ? _client.dir / "LonelyIce" : fs::path(), "client");
#ifdef _WIN32
    add("exe", _host.exeDir, "exe");
#else
    add("user", UserDataDir(), "user");
    add("exe", _host.exeDir, "exe_posix");
#endif
    add("custom", _customPlace, "custom");
}

void Wizard::BuildComponents()
{
    fs::path root = PlacePath();
    fs::path data = root / "data";
    std::error_code ec;
    bool setup = fs::exists(_host.exeDir / "setup" / "sql.pak", ec) && fs::exists(_host.exeDir / "setup" / "configs.pak", ec);
    bool haveDb = !root.empty() && fs::exists(root / "db" / "world.sqlite", ec);
    uint32_t maps = root.empty() ? 0 : CountFiles(data / "maps");
    uint32_t vmaps = root.empty() ? 0 : CountFiles(data / "vmaps", ".vmtree");
    uint32_t mmaps = root.empty() ? 0 : CountFiles(data / "mmaps");

    auto have = [](uint32_t count, char const* key) { return count ? Tr(key, count) : std::string(); };
    bool newSql = setup && haveDb && _host.sqlStamp && _host.sqlStamp() != Installer::SqlStamp(_host.exeDir / "setup");
    std::vector<CompRow> old = std::move(_comps);
    // Titles are the installer's step titles.
    _comps = {
        { "db", Tr("install.step.db"), Tr("wizard.comp.db.desc"), Tr("wizard.comp.db.size"),
            !setup ? Tr("wizard.comp.db.no_setup") : newSql ? Tr("wizard.comp.db.new_sql") : haveDb ? Tr("wizard.comp.db.have") : std::string(),
            setup && (!haveDb || newSql), !setup },
        { "maps", Tr("install.step.maps"), Tr("wizard.comp.maps.desc"), Tr("wizard.comp.maps.size"), have(maps, "wizard.comp.maps.have"), maps == 0, false },
        { "vmaps", Tr("install.step.vmaps"), Tr("wizard.comp.vmaps.desc"), Tr("wizard.comp.vmaps.size"), have(vmaps, "wizard.comp.vmaps.have"), vmaps == 0, false },
        { "mmaps", Tr("install.step.mmaps"), Tr("wizard.comp.mmaps.desc"), Tr("wizard.comp.mmaps.size"), have(mmaps, "wizard.comp.mmaps.have"), mmaps == 0, false },
        { "client", Tr("install.step.client"), Tr("wizard.comp.client.desc"), Tr("wizard.comp.client.size"), "", true, false },
    };
    // Keep the player's own choices when only the place changed.
    if (old.size() == _comps.size() && _step >= 2)
        for (std::size_t i = 0; i < old.size(); ++i)
            if (!_comps[i].locked)
                _comps[i].on = old[i].on;
    RefreshTotal();
}

void Wizard::RefreshTotal()
{
    static double const gb[] = { 0.4, 0.7, 0.6, 6.0, 0 };
    double need = 0;
    int n = 0;
    for (std::size_t i = 0; i < _comps.size(); ++i)
        if (_comps[i].on)
        {
            need += gb[i];
            ++n;
        }
    _total = Tr("wizard.total", n, _comps.size(), GbText(need));
}

void Wizard::RefreshFooter()
{
    _steps.clear();
    for (int i = 0; i < StepCount; ++i)
        _steps.push_back({ Tr(StepKeys[i]), i < _step ? "done" : i == _step ? "cur" : "" });
    _counter = Tr("wizard.counter", _step + 1, StepCount);
    _backVisible = _step > 0 && _step < 5;
    bool installing = _installer.IsRunning();
    _cancelLabel = Tr(_step == 5 && installing ? "wizard.button.abort" : _step == 6 ? "wizard.button.close" : "wizard.button.later");
    _nextOk = true;
    if (_step == 4)
        _nextLabel = Tr("wizard.button.install");
    else if (_step == 5)
    {
        _nextLabel = Tr(_installer.Finished() && !_installer.Succeeded() ? "wizard.button.retry" : "wizard.button.done");
        _nextOk = _installer.Finished();
    }
    else if (_step == 6)
        _nextLabel = Tr("wizard.button.play");
    else
        _nextLabel = Tr("wizard.button.next");
}

void Wizard::Go(int step)
{
    _step = std::clamp(step, 0, StepCount - 1);
    _error.clear();
    if (_step == 1)
        BuildPlaces();
    RefreshFooter();
    Dirty();
}

void Wizard::Back()
{
    if (_step > 0 && _step < 5)
        Go(_step - 1);
}

void Wizard::Next()
{
    switch (_step)
    {
        case 0:
            if (!_client.valid)
                return Error(Tr("wizard.error.no_client"));
            if (_client.version != "3.3.5.12340")
                return Error(Tr("wizard.error.wrong_build"));
            return Go(1);
        case 1:
        {
            fs::path root = PlacePath();
            if (root.empty())
                return Error(Tr("wizard.error.no_place"));
            if (!Writable(root))
                return Error(Tr("wizard.error.not_writable", Utf8(root)));
            BuildComponents();
            return Go(2);
        }
        case 2:
        {
            RefreshTotal();
            if (std::none_of(_comps.begin(), _comps.end(), [](CompRow const& c) { return c.on; }))
                return Error(Tr("wizard.error.no_components"));
            if (_comps[0].on && _comps[0].locked)
                return Error(Tr("wizard.error.no_setup"));
            int t = std::atoi(_threads.c_str());
            if (t < 1)
                return Error(Tr("wizard.error.threads"));
            Go(3);
            if (FreeBytes(PlacePath()) < (9ull << 30) && _comps[3].on)
                Error(Tr("wizard.error.low_space"));
            return;
        }
        case 3:
            if (_comps[0].on && (!ValidLogin(_login) || _pass.empty()))
                return Error(Tr("wizard.error.login"));
            if (_realm.empty())
                return Error(Tr("wizard.error.realm"));
            return Go(4);
        case 4:
            if (_host.serverRunning && _host.serverRunning())
                return Error(Tr("wizard.error.server_running"));
            StartInstall();
            return Go(5);
        case 5:
            if (!_installer.Finished())
                return;
            if (!_installer.Succeeded())
            {
                // Retry: steps that finished stay done.
                for (InstallStep const& s : _installer.Steps())
                    for (CompRow& c : _comps)
                        if (c.id == s.id && s.state == StepState::Done)
                            c.on = false;
                StartInstall();
                return Go(5);
            }
            return Go(6);
        case 6:
            _open = false;
            Dirty();
            if (_host.play)
                _host.play();
            return;
    }
}

void Wizard::Cancel()
{
    if (_step == 5 && _installer.IsRunning())
    {
        _installer.Cancel();
        return;
    }
    _open = false;
    Dirty();
}

void Wizard::StartInstall()
{
    InstallOptions o;
    o.exe = _host.exe;
    o.setupDir = _host.exeDir / "setup";
    o.root = PlacePath();
    o.client = _client.dir;
    auto on = [&](char const* id) { return std::any_of(_comps.begin(), _comps.end(), [&](CompRow const& c) { return c.id == id && c.on; }); };
    o.db = on("db");
    o.maps = on("maps");
    o.vmaps = on("vmaps");
    o.mmaps = on("mmaps");
    o.client_prep = on("client");
    o.threads = std::max(1, std::atoi(_threads.c_str()));
    o.realmName = _realm;
    o.rate = std::max(1, std::atoi(_rate.c_str()));
    o.bots = std::max(0, std::atoi(_bots.c_str()));
    o.login = _login;
    o.password = _pass;
    o.gmLevel = std::atoi(_gm.c_str());
    o.realmlist = _rl;
    o.clearWdb = _wdb;
    o.accountName = _accName;
    o.shortcut = _lnk && _lnkSupported;
    _options = o;
    _log.clear();
    _installer.Start(o);
    Tick();
}

void Wizard::Tick()
{
    std::string picked;     // UTF-8
    bool forClient = false;
    {
        std::lock_guard<std::mutex> guard(_pickLock);
        picked.swap(_picked);
        forClient = _pickedForClient;
    }
    if (!picked.empty())
    {
        if (forClient)
        {
            Inspect(Platform::Utf8ToPath(picked));
            BuildPlaces();
        }
        else
        {
            _customPlace = Platform::Utf8ToPath(picked);
            _place = "custom";
            BuildPlaces();
            BuildComponents();
        }
        Dirty();
    }

    if (_open && _step == 2)
    {
        Rml::String before = _total;
        RefreshTotal();
        if (before != _total && _model)
            _model.DirtyVariable("wz_total");
    }

    if (!_installer.IsRunning() && !_installer.Finished())
        return;

    std::vector<InstallStep> steps = _installer.Steps();
    std::vector<InstRow> rows;
    for (InstallStep const& s : steps)
    {
        if (s.state == StepState::Skipped)
            continue;
        char const* st = s.state == StepState::Done ? "done" : s.state == StepState::Running ? "run" : s.state == StepState::Failed ? "fail" : "wait";
        std::string note = s.note;
        if (s.state == StepState::Running && note.empty())
            note = std::to_string(int(s.progress * 100)) + " %";
        // Looked up here rather than taken from s.title, so the rows follow a language switch.
        rows.push_back({ Tr("install.step." + s.id), note, st, int(s.progress * 100.f + 0.5f) });
    }
    bool changed = rows.size() != _inst.size();
    for (std::size_t i = 0; !changed && i < rows.size(); ++i)
        changed = rows[i].title != _inst[i].title || rows[i].note != _inst[i].note || rows[i].state != _inst[i].state || rows[i].pct != _inst[i].pct;
    if (changed)
    {
        _inst = std::move(rows);
        if (_model)
            _model.DirtyVariable("wz_inst");
    }

    std::vector<std::string> lines = _installer.TakeLog();
    if (!lines.empty())
    {
        for (std::string& l : lines)
            _log.push_back(l);
        if (_log.size() > 12)
            _log.erase(_log.begin(), _log.end() - 12);
        if (_model)
            _model.DirtyVariable("wz_log");
    }

    if (_installer.Finished() && !_reported)
    {
        _reported = true;
        if (_installer.Succeeded())
        {
            BuildDone();
            if (_host.installed)
                _host.installed(_options);
        }
        RefreshFooter();
        Dirty();
    }
    if (_installer.IsRunning())
        _reported = false;
}

void Wizard::BuildDone()
{
    _done.clear();
    fs::path root = _options.root;
    if (_options.db)
        _done.push_back({ "ok", Tr("wizard.done.db"), Utf8(root / "db") });
    if (_options.maps || _options.vmaps || _options.mmaps)
        _done.push_back({ "ok", Tr("wizard.done.data"), Utf8(root / "data") });
    if (_options.db && !_options.login.empty())
        _done.push_back({ "ok", _options.gmLevel ? Tr("wizard.done.account_gm", _options.login, _options.gmLevel) : Tr("wizard.done.account", _options.login), "" });
    if (_options.client_prep && _options.realmlist)
        _done.push_back({ "ok", "realmlist.wtf: 127.0.0.1", _rlNote });
}

void Wizard::BrowseFolder(bool forClient)
{
    {
        std::lock_guard<std::mutex> guard(_pickLock);
        _pickedForClient = forClient;
    }
    std::string start = forClient ? (_client.valid ? Utf8(_client.dir) : Utf8(_host.exeDir)) : Utf8(PlacePath().empty() ? _host.exeDir : PlacePath());
    SDL_ShowOpenFolderDialog([](void* self, char const* const* list, int)
    {
        if (!list || !list[0])
            return;
        auto* me = static_cast<Wizard*>(self);
        {
            std::lock_guard<std::mutex> guard(me->_pickLock);
            me->_picked = list[0];
        }
        if (me->_host.wake)
            me->_host.wake();
    }, this, UiBackend::GetWindow(), start.c_str(), false);
}
