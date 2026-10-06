#include "Wizard.h"
#include "ConfFile.h"
#include "ConfigEnv.h"
#include "ClientPatch.h"
#include "Lang.h"
#include "Platform.h"
#include "TextUtil.h"
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
    // Moving to another storage: its own page (7), then the install and done pages.
    constexpr int TransferStep = 7;
    struct SwitchStep { int step; char const* key; };
    constexpr SwitchStep SwitchSteps[] = { { TransferStep, "wizard.step.transfer" }, { 5, "wizard.step.install" }, { 6, "wizard.step.done" } };

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

    // A whole-number config value of file, empty when missing.
    std::string ConfNumber(fs::path const& file, std::string const& key)
    {
        ConfFile f;
        if (!f.Load(file))
            return {};
        std::optional<std::string> v = f.Get(key);
        return v && !v->empty() ? std::to_string(std::atoi(v->c_str())) : std::string();
    }
}

Wizard::Wizard(Host host) : _host(std::move(host)), _installer([w = _host.wake] { if (w) w(); }),
    _form("wzs_", { _host.exe, _host.providers,
        [this](StorageChoice const& c)
        {
            if (c.Remote())
                return _host.remoteEnv ? _host.remoteEnv(c) : Platform::Env();
            fs::path const root = PlacePath();
            return root.empty() ? Platform::Env() : LocalDatabaseOverrides(root);
        }, _host.wake })
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
    _form.Relocalize();
    if (_switching)
        BuildTransfer();
    if (!_done.empty())
        BuildDone();
    RefreshFooter();
    Dirty();
}

void Wizard::SetModel(Rml::DataModelHandle model)
{
    _model = model;
    _form.SetModel(model);
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
        s.RegisterMember("required", &CompRow::required);
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
    c.Bind("wz_audit", &_audit);
    c.Bind("wz_existing", &_existing);
    c.Bind("wz_customize", &_customize);
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
    c.Bind("wz_transfer", &_transfer);
    c.Bind("wz_transfer_title", &_transferTitle);
    c.Bind("wz_transfer_note", &_transferNote);
    c.Bind("wz_need_account", &_needAccount);
    c.Bind("wz_ask_account", &_askAccount);
    c.Bind("wz_account_optional", &_accountOptional);
    c.Bind("wz_switching", &_switching);
    _form.Bind(c, true);

    auto on = [&](char const* name, std::function<void()> fn)
    {
        c.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { fn(); });
    };
    on("wz_next", [this] { Next(); });
    on("wz_back", [this] { Back(); });
    on("wz_cancel", [this] { Cancel(); });
    on("wz_browse_client", [this] { BrowseFolder(true); });
    on("wz_browse_place", [this] { BrowseFolder(false); });
    on("wz_customize_changed", [this] { RefreshFooter(); Dirty(); });
    c.BindEventCallback("wz_pick_place", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const& args)
    {
        if (args.empty())
            return;
        _place = args[0].Get<Rml::String>();
        if (_place == "custom" && _customPlace.empty())
            BrowseFolder(false);
        // the built-in databases are in the place's folder
        if (!_form.Choice().Remote())
            _form.Check();
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
    _switching = _updating = false;
    _prefilledFor.clear();
    _preparedFor.clear();
    _cacheDetectedFor.clear();
    _customize = false;
    _login.clear();
    _pass.clear();
    _customPlace.clear();
    Inspect(client);
    fs::path root = _host.currentRoot();
    std::error_code ec;
    if (fs::exists(root / ".runtime" / "configs" / "worldserver.conf", ec) && !fs::equivalent(root, _host.exeDir, ec))
        _customPlace = root;
    _place = !_customPlace.empty() ? "custom" : (fs::exists(_host.exeDir / ".runtime" / "configs" / "worldserver.conf", ec) ? "exe" : "client");
#ifndef _WIN32
    fs::path user = UserDataDir();
    if (!_customPlace.empty() && !user.empty() && fs::equivalent(_customPlace, user, ec))
    {
        _customPlace.clear();
        _place = "user";
    }
#endif
    BuildPlaces();
    _form.Load(_host.storage ? _host.storage() : StorageChoice{});
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
    StorageChoice const storage = _form.Choice();
    std::optional<StorageState> const& checked = _form.Result();
    bool setup = fs::exists(_host.exeDir / "setup" / "sql.pak", ec) && fs::exists(_host.exeDir / "setup" / "configs.pak", ec);
    // File presence identifies an existing installation; only a successful probe confirms usable databases.
    bool const haveDb = checked && checked->reached && checked->databases;
    _dbExists = haveDb;
    _existing = haveDb || (!root.empty() && (fs::exists(root / "db" / "world.sqlite", ec)
        || fs::exists(root / "db" / "auth.sqlite", ec) || fs::exists(root / "db" / "characters.sqlite", ec)));
    bool const haveCache = !root.empty() && HasUnpackedFiles(data) && (checked ? checked->HasDbc() : haveDb);
    uint32_t vmaps = root.empty() ? 0 : CountFiles(data / "vmaps", ".vmtree");
    uint32_t mmaps = root.empty() ? 0 : CountFiles(data / "mmaps", ".mmtile");
    bool const haveVmaps = vmaps > 0 && CountFiles(data / "vmaps", ".vmtile") > 0;
    bool const haveClient = _client.valid && !_client.locales.empty()
        && std::all_of(_client.locales.begin(), _client.locales.end(), [&](ClientLocale const& l)
            { return GameClient::IsLocalRealmlist(l.realmlist, Installer::LoginPort(root)); })
        && ClientPatch::AddonsCurrent(_client.dir, ReadPlugins(_host.exeDir / "plugins"));
    if (_preparedFor != root)
    {
        _preparedFor = root;
        _rl = !haveClient;
        _wdb = !_existing;
        _accName = !_existing;
        _lnk = false;
    }

    auto have = [](uint32_t count, char const* key) { return count ? Tr(key, count) : std::string(); };
    std::string const stamp = _host.sqlStamp ? _host.sqlStamp() : std::string();
    bool const sameRoot = _host.currentRoot && fs::equivalent(root, _host.currentRoot(), ec);
    bool newSql = setup && haveDb && sameRoot && !stamp.empty() && stamp != Installer::SqlStamp(_host.exeDir / "setup");
    std::string dbNote = !setup ? Tr("wizard.comp.db.no_setup") : newSql ? Tr("wizard.comp.db.new_sql")
        : haveDb ? Tr("wizard.audit.found")
        : !checked ? Tr("wizard.comp.db.checking") : std::string();
    bool const oldMmaps = HasComp("mmaps");
    bool const hadComps = !_comps.empty();

    // Titles are the installer's step titles. Required parts go in when they are missing, without a question.
    _comps.clear();
    _comps.push_back({ "db", Tr("install.step.db"), Tr("wizard.comp.db.desc"), Tr("wizard.comp.db.size"), dbNote,
        setup && (!haveDb || newSql), !setup, true, 0.4 });
    if (storage.cache)
        _comps.push_back({ "unpack", Tr("install.step.unpack"), Tr("wizard.comp.unpack.desc"), Tr("wizard.comp.unpack.size"),
            haveCache ? Tr("wizard.comp.unpack.have") : std::string(), !haveCache, false, true, 0.5 });
    _comps.push_back({ "vmaps", Tr("install.step.vmaps"), Tr("wizard.comp.vmaps.desc"), Tr("wizard.comp.vmaps.size"),
        haveVmaps ? have(vmaps, "wizard.comp.vmaps.have") : std::string(),
        !haveVmaps, false, true, 0.6 });
    _comps.push_back({ "client", Tr("install.step.client"), Tr("wizard.comp.client.desc"), Tr("wizard.comp.client.size"), "", !haveClient, false, true, 0 });
    // Paths make the world better but cost the most time and space: the player decides.
    _comps.push_back({ "mmaps", Tr("install.step.mmaps"), Tr("wizard.comp.mmaps.desc"), Tr("wizard.comp.mmaps.size"), have(mmaps, "wizard.comp.mmaps.have"),
        hadComps && _step >= 2 ? oldMmaps : !_existing && mmaps == 0, false, false, 6.0 });
    _audit.clear();
    auto audit = [&](char const* title, bool found, std::string detail = std::string())
    {
        _audit.push_back({ found ? "ok" : "warn", Tr(title), detail.empty()
            ? Tr(found ? "wizard.audit.found" : "wizard.audit.restore") : detail });
    };
    for (auto const& [name, found] : { std::pair{ "auth", checked && checked->auth },
        std::pair{ "characters", checked && checked->characters }, std::pair{ "world", checked && checked->world } })
        _audit.push_back({ !checked ? "warn" : found ? "ok" : "warn", name,
            Tr(!checked ? "wizard.audit.checking" : found ? "wizard.audit.found" : "wizard.audit.restore") });
    audit("wizard.audit.configs", fs::exists(root / ".runtime" / "configs" / "worldserver.conf", ec),
        Tr("wizard.audit.configs_detail"));
    if (storage.cache)
        audit("install.step.unpack", haveCache);
    audit("install.step.vmaps", haveVmaps, haveVmaps ? have(vmaps, "wizard.comp.vmaps.have") : std::string());
    audit("install.step.client", haveClient);
    audit("install.step.mmaps", mmaps > 0, Tr(mmaps > 0 ? "wizard.comp.mmaps.have" : "wizard.audit.optional", mmaps));
    _compsFor = storage;
    _compsChecked = checked.has_value();
    RefreshTotal();
    RefreshFooter();
}

// Realm, rates and bots as the chosen place has them now (defaults for a new place); read once per place, so what
// the player typed survives going back and forth.
void Wizard::Prefill()
{
    fs::path const root = PlacePath();
    if (root == _prefilledFor)
        return;
    _prefilledFor = root;
    std::error_code ec;
    fs::path const conf = root / ".runtime" / "configs" / "worldserver.conf";
    bool const existing = !root.empty() && fs::exists(conf, ec);
    // the realm name is kept by the launcher for the server folder it uses
    _realmWas = existing && _host.realmName && fs::equivalent(root, _host.currentRoot(), ec) ? _host.realmName() : std::string();
    if (!_realmWas.empty())
        _realm = _realmWas;
    // a value the list does not offer stays as it is unless the player picks another one
    std::string const rate = existing ? ConfNumber(conf, "Rate.XP.Kill") : std::string();
    if (rate == "1" || rate == "2" || rate == "5")
        _rate = rate;
    std::string const bots = existing ? ConfNumber(root / ".runtime" / "configs" / "modules" / "playerbots.conf", "AiPlayerbot.MaxRandomBots") : std::string();
    if (bots == "0" || bots == "100" || bots == "500" || bots == "1000")
        _bots = bots;
    _rateWas = rate.empty() ? std::string() : std::string(_rate);
    _botsWas = bots.empty() ? std::string() : std::string(_bots);
}

bool Wizard::HasComp(char const* id) const
{
    return std::any_of(_comps.begin(), _comps.end(), [&](CompRow const& c) { return c.id == id && c.on; });
}

void Wizard::RefreshTotal()
{
    double need = 0;
    int n = 0;
    for (CompRow const& c : _comps)
        if (c.on)
        {
            need += c.gb;
            ++n;
        }
    _total = Tr("wizard.total", n, _comps.size(), GbText(need));
}

void Wizard::RefreshFooter()
{
    _steps.clear();
    if (_switching)
    {
        // a database update has no page of its own before the install page
        SwitchStep const* const list = _updating ? SwitchSteps + 1 : SwitchSteps;
        int const count = int(std::size(SwitchSteps)) - (_updating ? 1 : 0);
        int cur = 0;
        for (int i = 0; i < count; ++i)
            if (list[i].step == _step)
                cur = i;
        for (int i = 0; i < count; ++i)
            _steps.push_back({ Tr(list[i].key), i < cur ? "done" : i == cur ? "cur" : "" });
        _counter = Tr("wizard.counter", cur + 1, count);
    }
    else
    {
        std::vector<int> visible;
        for (int i = 0; i < StepCount; ++i)
        {
            if (i == 3 && _dbExists && !_customize)
                continue;
            if (i == 4 && !HasComp("client"))
                continue;
            visible.push_back(i);
            _steps.push_back({ Tr(StepKeys[i]), i < _step ? "done" : i == _step ? "cur" : "" });
        }
        auto const cur = std::find(visible.begin(), visible.end(), _step);
        _counter = Tr("wizard.counter", int(std::distance(visible.begin(), cur)) + 1, visible.size());
    }
    _backVisible = !_switching && _step > 0 && _step < 5;
    bool installing = _installer.IsRunning();
    _cancelLabel = Tr(_step == 5 && installing ? "wizard.button.abort" : _step == 6 || (_step == 5 && _installer.Finished()) ? "wizard.button.close"
        : _step == TransferStep ? "wizard.button.cancel" : "wizard.button.later");
    _nextOk = true;
    if (_step == 4)
        _nextLabel = Tr("wizard.button.install");
    else if (_step == TransferStep)
        _nextLabel = Tr("wizard.button.start");
    else if (_step == 2 && _waitCheck)
    {
        _nextLabel = Tr("wizard.button.checking");
        _nextOk = false;
    }
    else if (_step == 2 && _dbExists && !_customize)
    {
        bool const repairs = std::any_of(_comps.begin(), _comps.end(), [](CompRow const& c) { return c.on; });
        _nextLabel = Tr(repairs ? "wizard.button.restore" : "wizard.button.connect");
    }
    else if (_step == 5)
    {
        _nextLabel = Tr(_installer.Finished() && !_installer.Succeeded() ? "wizard.button.retry" : "wizard.button.done");
        _nextOk = _installer.Finished();
    }
    else if (_step == 6)
        _nextLabel = Tr(_switching ? "wizard.button.close" : "wizard.button.play");
    else
        _nextLabel = Tr("wizard.button.next");
}

void Wizard::Go(int step)
{
    _step = step == TransferStep ? step : std::clamp(step, 0, StepCount - 1);
    _error.clear();
    _waitCheck = false;
    if (_step == 1)
        BuildPlaces();
    _askAccount = HasComp("db") || _customize;
    _accountOptional = _dbExists;
    RefreshFooter();
    Dirty();
}

void Wizard::Back()
{
    if (!_switching && _step > 0 && _step < 5)
        Go(_step == 4 && _dbExists && !_customize ? 2 : _step - 1);
}

void Wizard::NextPreparation()
{
    if (HasComp("client"))
        return Go(4);
    if (_host.serverRunning && _host.serverRunning())
        return Error(Tr("wizard.error.server_running"));
    StartInstall();
    Go(5);
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
            // The client may have been selected after opening the wizard, when its storage path was still empty.
            if (!_form.Choice().Remote())
            {
                StorageChoice choice = _form.Choice();
                if (_cacheDetectedFor != root)
                {
                    _cacheDetectedFor = root;
                    if (HasUnpackedFiles(root / "data"))
                        choice.cache = true;
                }
                if (choice != _form.Choice())
                    _form.Load(choice);
                else
                    _form.Check();
            }
            BuildComponents();
            Prefill();
            return Go(2);
        }
        case 2:
        {
            int t = std::atoi(_threads.c_str());
            if (t < 1)
                return Error(Tr("wizard.error.threads"));
            // the chosen storage is checked before going on: a database server must answer
            std::optional<StorageState> const& checked = _form.Result();
            if (!checked)
            {
                if (!_form.Checking())
                    _form.Check();
                _waitCheck = true;
                RefreshFooter();
                Dirty();
                return;
            }
            if (!checked->reached)
                return Error(checked->error);
            BuildComponents();
            if (_comps[0].on && _comps[0].locked)
                return Error(Tr("wizard.error.no_setup"));
            if (FreeBytes(PlacePath()) < (9ull << 30) && HasComp("mmaps"))
                return Error(Tr("wizard.error.low_space"));
            if (_dbExists && !_customize)
                NextPreparation();
            else
                Go(3);
            return;
        }
        case 3:
            // new databases need the player's account; existing ones have theirs, a login given is added if missing
            if (_askAccount && (!_dbExists || !_login.empty() || !_pass.empty())
                && (!ValidAccountName(_login) || !ValidAccountPassword(_pass)))
                return Error(Tr(_dbExists ? "wizard.error.login_optional" : "wizard.error.login"));
            if (_realm.empty())
                return Error(Tr("wizard.error.realm"));
            return NextPreparation();
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
                if (_switching)
                {
                    _installer.Start(_options);
                    return Go(5);
                }
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
            if (!_switching && _host.play)
                _host.play();
            return;
        case TransferStep:
            if (_needAccount && (!ValidAccountName(_login) || !ValidAccountPassword(_pass)))
                return Error(Tr("wizard.error.login"));
            if (_host.serverRunning && _host.serverRunning())
                return Error(Tr("wizard.error.server_running"));
            StartSwitch();
            return Go(5);
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

void Wizard::SwitchStorage(fs::path const& client, StorageChoice const& to, std::string const& title, StoragePlan const& plan,
    std::string const& realmName)
{
    _open = true;
    if (_installer.IsRunning())
        return Go(5);

    Inspect(client);
    _switching = true;
    _updating = false;
    _switchTo = to;
    _switchTitle = title;
    _switchPlan = plan;
    _needAccount = plan.newDatabases;
    if (!realmName.empty())
        _realm = realmName;
    BuildTransfer();
    Go(TransferStep);
}

void Wizard::UpdateDatabases(fs::path const& client)
{
    _open = true;
    if (_installer.IsRunning())
        return Go(5);

    Inspect(client);
    _switching = _updating = true;
    _switchTo = _host.storage ? _host.storage() : StorageChoice{};
    _switchTitle.clear();
    _switchPlan = {};
    _switchPlan.db = true;
    _needAccount = false;
    _inst.clear();
    StartSwitch();
    Go(5);
}

void Wizard::BuildTransfer()
{
    _transferTitle = Tr(_switchTo.cache ? "wizard.transfer.to_cache" : "wizard.transfer.to", _switchTitle);
    // the databases left behind are mentioned only when the location changes
    _transferNote = _switchTo.location != (_host.storage ? _host.storage().location : "local") ? Tr("wz.transfer.note") : std::string();
    _transfer.clear();
    if (_switchPlan.db)
        _transfer.push_back({ "ok", Tr(_needAccount ? "wizard.transfer.db_new" : "wizard.transfer.db_update"), "" });
    if (_switchPlan.unpack)
        _transfer.push_back({ "ok", Tr("wizard.transfer.unpack"), Tr("wizard.comp.unpack.size") });
    if (_switchPlan.pack)
        _transfer.push_back({ "ok", Tr("wizard.transfer.pack"), "" });
}

void Wizard::StartSwitch()
{
    InstallOptions o;
    o.exe = _host.exe;
    o.profileFile = _host.profileFile ? _host.profileFile() : _host.exeDir / "server.yaml";
    o.setupDir = _host.exeDir / "setup";
    o.root = _host.currentRoot();
    o.client = _client.dir;
    o.location = _switchTo.location;
    o.cache = _switchTo.cache;
    o.remote = _switchTo.remote;
    o.locale = _host.serverLocale ? _host.serverLocale() : std::string();
    if (_switchTo.Remote() && _host.remoteEnv)
        o.serverEnv = _host.remoteEnv(_switchTo);
    o.db = _switchPlan.db;
    o.unpack = _switchPlan.unpack;
    o.pack = _switchPlan.pack;
    o.vmaps = o.mmaps = o.client_prep = false;
    // new databases get the realm's current name; existing ones keep theirs
    o.realmName = _needAccount ? std::string(_realm) : std::string();
    o.newDatabases = _needAccount;
    if (_needAccount)
    {
        o.login = _login;
        o.password = _pass;
        o.gmLevel = std::atoi(_gm.c_str());
    }
    _options = o;
    _log.clear();
    _installer.Start(o);
    Tick();
}

void Wizard::StartInstall()
{
    _switching = false;
    InstallOptions o;
    o.exe = _host.exe;
    o.profileFile = _host.profileFile ? _host.profileFile() : _host.exeDir / "server.yaml";
    o.setupDir = _host.exeDir / "setup";
    o.root = PlacePath();
    o.client = _client.dir;
    StorageChoice const storage = _form.Choice();
    o.location = storage.location;
    o.cache = storage.cache;
    o.remote = storage.remote;
    if (storage.Remote() && _host.remoteEnv)
        o.serverEnv = _host.remoteEnv(storage);
    o.db = HasComp("db") || (_customize && !_login.empty());
    o.unpack = HasComp("unpack");
    // a cache left in the place by an earlier install goes when the server reads the client
    std::optional<StorageState> const& checked = _form.Result();
    o.pack = !storage.cache && (HasUnpackedFiles(o.root / "data") || (checked && checked->dbcTables > 0));
    o.locale = _host.serverLocale ? _host.serverLocale() : std::string();
    o.vmaps = HasComp("vmaps");
    o.mmaps = HasComp("mmaps");
    o.client_prep = HasComp("client");
    o.threads = std::max(1, std::atoi(_threads.c_str()));
    // On databases that exist the realm is renamed only when the name was changed here; rates and bots go into
    // configs that exist only when changed here too (new configs always get them).
    o.realmName = !_dbExists || (!_realmWas.empty() && _realm != _realmWas) ? std::string(_realm) : std::string();
    o.rate = std::max(1, std::atoi(_rate.c_str()));
    o.bots = std::max(0, std::atoi(_bots.c_str()));
    o.rateChanged = !_rateWas.empty() && _rate != _rateWas;
    o.botsChanged = !_botsWas.empty() && _bots != _botsWas;
    o.login = _login;
    o.password = _pass;
    o.gmLevel = std::atoi(_gm.c_str());
    o.newDatabases = !_dbExists;
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
            if (!_form.Choice().Remote())
                _form.Check();
            BuildComponents();
        }
        Dirty();
    }

    if (_open && !_switching && _step <= 2)
    {
        // the components follow the storage form: its choice and what the check found there
        bool const checkChanged = _form.Tick();
        if (_step == 2 && (checkChanged || _compsFor != _form.Choice() || _compsChecked != _form.Result().has_value()))
        {
            BuildComponents();
            Dirty();
        }
        if (_waitCheck && !_form.Checking() && _form.Result())
        {
            _waitCheck = false;
            RefreshFooter();
            Next();
        }
    }
    if (_open && _step == 2)
    {
        Rml::String before = _total;
        RefreshTotal();
        if (before != _total && _model)
        {
            _model.DirtyVariable("wz_total");
            RefreshFooter();
            Dirty();
        }
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
            // a database update closes by itself: the launcher goes on with the start that asked for it
            if (_updating)
                _open = false;
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
    bool const remote = _options.location != "local";
    if (_options.db)
        _done.push_back({ "ok", Tr(remote ? "wizard.done.db_remote" : "wizard.done.db"),
            remote ? _options.remote.host + ":" + _options.remote.port : Utf8(root / "db") });
    if (_options.unpack || _options.vmaps || _options.mmaps)
        _done.push_back({ "ok", Tr("wizard.done.data"), Utf8(root / "data") });
    _done.push_back({ "ok", Tr(_options.cache ? "wizard.done.cache" : "wizard.done.client"), "" });
    if (_options.db && !_options.login.empty())
        _done.push_back({ "ok", !_options.newDatabases ? Tr("wizard.done.account_kept", _options.login)
            : _options.gmLevel ? Tr("wizard.done.account_gm", _options.login, _options.gmLevel) : Tr("wizard.done.account", _options.login), "" });
    if (_options.client_prep && _options.realmlist)
        _done.push_back({ "ok", "realmlist.wtf: " + GameClient::LocalRealmlist(Installer::LoginPort(_options.root)), _rlNote });
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
