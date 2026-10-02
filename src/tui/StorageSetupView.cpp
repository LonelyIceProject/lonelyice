#include "StorageSetupView.h"
#include "ConfigEnv.h"
#include "GameClient.h"
#include "Lang.h"
#include "TextUtil.h"
#include "PackageManager.h"
#include <ftxui/component/event.hpp>
#include <algorithm>
#include <charconv>
#include <sstream>

namespace fs = std::filesystem;
using namespace LonelyIce;
using namespace ftxui;

namespace
{
    bool Delimited(std::string const& value) { return value.find_first_of(";\r\n\t") != std::string::npos; }
    bool Controls(std::string const& value) { return value.find_first_of("\r\n\t") != std::string::npos; }
    fs::path Absolute(std::string const& value, fs::path const& base)
    {
        fs::path p = Platform::Utf8ToPath(value);
        return (p.is_absolute() ? p : base / p).lexically_normal();
    }
    bool RemovingGeneratedData(InstallOptions const& o)
    {
        std::error_code ec;
        return o.pack || (o.db && fs::exists(o.root / "sql", ec))
            || (o.unpack && (fs::exists(o.root / "data" / "dbc", ec)
                || fs::exists(o.root / "data" / "maps", ec) || fs::exists(o.root / "data" / "Cameras", ec)))
            || (o.vmaps && fs::exists(o.root / "data" / "vmaps", ec))
            || (o.mmaps && fs::exists(o.root / "data" / "mmaps", ec));
    }
}

StorageSetupView::StorageSetupView(Host host)
    : _host(std::move(host)), _check(_host.wake), _installer(_host.wake)
{
    Reload();
}

StorageSetupView::~StorageSetupView() { Cancel(); }

void StorageSetupView::Cancel()
{
    _wizardCheckPending = false;
    if (_installPending)
        _cancelRequested = true;
    _installer.Cancel();
    _check.Cancel();
    _notice = Tr("tui.storage.cancelling");
}

bool StorageSetupView::SelectProvider(std::string const& id)
{
    if (Busy())
        return false;
    auto it = std::find(_locationIds.begin(), _locationIds.end(), id);
    if (it == _locationIds.end())
        return false;
    _location = int(it - _locationIds.begin());
    if (_location > 0 && _port.empty())
        _port = _providers[_location - 1].provider.port;
    Edited();
    return true;
}

void StorageSetupView::Reload()
{
    if (Busy() || !_host.settings)
        return;
    auto const& s = *_host.settings;
    _providers = _host.providers ? _host.providers() : std::vector<StorageProviderInfo>{};
    _locations = { Tr("tui.storage.local") };
    _locationIds = { "local" };
    _location = 0;
    for (auto const& p : _providers)
    {
        _locations.push_back(p.provider.name);
        _locationIds.push_back(p.provider.id);
        if (s.location == p.provider.id)
            _location = int(_locationIds.size() - 1);
    }
    _notice = s.location != "local" && _location == 0 ? Tr("tui.storage.unavailable", s.location) : std::string{};
    _remoteHost = s.remote.host;
    _port = s.remote.port;
    _user = s.remote.user;
    _password = s.remote.password;
    _prefix = s.remote.prefix;
    _cache = s.dataCache;
    _client = Platform::PathToUtf8(s.clientPath);
    _root = Platform::PathToUtf8(s.dataRoot.empty() ? _host.defaultRoot : s.dataRoot);
    _realm = s.realmName.empty() ? "LonelyIce" : s.realmName;
    std::error_code ec;
    _setup = !fs::exists(Root() / ".runtime" / "configs" / "worldserver.conf", ec);
    _serverOnly = true;
    _vmaps = _mmaps = false;
    _login.clear();
    _accountPassword.clear();
    _log.clear();
    Edited();
    _baseline = _seen = Fingerprint();
}

void StorageSetupView::StartWizard()
{
    if (Busy())
        return;
    _wizard = true;
    _wizardStep = 0;
    _wizardCheckPending = false;
    _setup = true;
    Edited();
    _notice.clear();
    if (_wizardFirst)
        _wizardFirst->TakeFocus();
}

void StorageSetupView::WizardNext()
{
    if (!CanOperate())
        return;
    _notice = Validate(_wizardStep >= 2);
    if (!_notice.empty())
        return;
    if (_wizardStep < 2)
    {
        ++_wizardStep;
        return;
    }
    if (!_result || _checkedFingerprint != Fingerprint())
    {
        Check();
        _wizardCheckPending = _check.IsRunning();
        return;
    }
    Review();
    if (_review)
        _wizardStep = 3;
}

StorageChoice StorageSetupView::Choice() const
{
    StorageChoice c;
    if (_location >= 0 && _location < int(_locationIds.size()))
        c.location = _locationIds[_location];
    c.cache = c.Remote() || _cache;
    c.remote = { _remoteHost, _port, _user, _password, _prefix };
    return c;
}

fs::path StorageSetupView::Root() const
{
    auto base = _host.settings && !_host.settings->file.empty() ? _host.settings->file.parent_path() : _host.exe.parent_path();
    return Absolute(_root, base);
}

std::string StorageSetupView::Fingerprint() const
{
    // Lengths preserve boundaries without printing or logging passwords.
    std::ostringstream out;
    auto add = [&](std::string const& s) { out << s.size() << ':' << s; };
    add(Choice().location);
    for (auto const* v : { &_remoteHost, &_port, &_user, &_password, &_prefix, &_client, &_root,
             &_realm, &_login, &_accountPassword })
        add(*v);
    out << _cache << _setup << _serverOnly << _vmaps << _mmaps;
    return out.str();
}

bool StorageSetupView::Busy() const { return _check.IsRunning() || _installer.IsRunning() || _installPending; }

bool StorageSetupView::Dirty() const
{
    return _host.settings && Fingerprint() != _baseline;
}

void StorageSetupView::Edited()
{
    _result.reset();
    _checkedFingerprint.clear();
    _review.reset();
    _confirm = _confirmRemoval = false;
}

std::string StorageSetupView::Validate(bool preparing) const
{
    if (_root.empty() || Delimited(_root))
        return Tr("tui.storage.bad_root");
    std::error_code ec;
    auto root = Root();
    if (root == root.root_path() || (fs::exists(root, ec) && !fs::is_directory(root, ec)) || ec)
        return Tr("tui.storage.bad_root");
    auto choice = Choice();
    if (choice.Remote())
    {
        if (std::none_of(_providers.begin(), _providers.end(), [&](auto const& p) { return p.provider.id == choice.location; }))
            return Tr("tui.storage.unavailable", choice.location);
        if (_remoteHost.empty() || _port.empty() || _user.empty() || _prefix.empty())
            return Tr("tui.storage.required");
        for (auto const* v : { &_remoteHost, &_port, &_user, &_password, &_prefix })
            if (Delimited(*v) || v->find('\0') != std::string::npos)
                return Tr("tui.storage.delimiter");
        unsigned port = 0;
        auto [end, err] = std::from_chars(_port.data(), _port.data() + _port.size(), port);
        if (err != std::errc{} || end != _port.data() + _port.size() || port == 0 || port > 65535)
            return Tr("tui.storage.bad_port");
    }
    if (preparing)
    {
        auto const& settings = *_host.settings;
        if (Controls(_client) || Controls(_realm) || Controls(_login) || Controls(_accountPassword))
            return Tr("tui.storage.bad_setup_field");
        if (_setup && (_realm.empty() || (_login.empty() != _accountPassword.empty())))
            return Tr("tui.storage.required_setup");
        if (_setup && !_login.empty() && (!ValidAccountName(_login) || !ValidAccountPassword(_accountPassword)))
            return Tr("wizard.error.login");
        if (!_client.empty())
        {
            auto base = settings.file.empty() ? _host.exe.parent_path() : settings.file.parent_path();
            auto info = GameClient::Inspect(Absolute(_client, base));
            if (!info.valid || (!info.version.empty() && info.version != "3.3.5.12340"))
                return Tr("tui.storage.bad_client");
        }
    }
    return {};
}

bool StorageSetupView::CanOperate()
{
    if (Busy() || (_host.operationBusy && _host.operationBusy()))
    {
        _notice = Tr("tui.storage.busy");
        return false;
    }
    if (_host.serverRunning && _host.serverRunning())
    {
        _notice = Tr("tui.storage.stop_required");
        return false;
    }
    return true;
}

Platform::Env StorageSetupView::Environment(StorageChoice const& choice) const
{
    if (_host.env)
        return _host.env(choice, Root());
    if (!choice.Remote())
        return LocalDatabaseOverrides(Root());
    for (auto const& p : _providers)
        if (p.provider.id == choice.location)
            return RemoteDatabaseOverrides(p.provider, choice.remote, p.dir / "server" / Packages::Manager::Platform());
    return {};
}

void StorageSetupView::Check()
{
    if (!CanOperate())
        return;
    _notice = Validate(false);
    if (!_notice.empty())
        return;
    Edited();
    _seen = _checkedFingerprint = Fingerprint();
    _check.Start(_host.exe, Environment(Choice()));
    _notice = Tr("tui.storage.checking");
}

void StorageSetupView::Review()
{
    if (!CanOperate())
        return;
    _notice = Validate(true);
    if (!_notice.empty())
        return;
    if (!_result || _checkedFingerprint != Fingerprint())
    {
        _notice = Tr("tui.storage.check_required");
        return;
    }
    if (!_result->reached)
    {
        _notice = _result->error;
        return;
    }
    auto const stamp = Installer::SqlStamp(_host.setupDir);
    auto const plan = PlanStorage(*_result, Choice().cache, Root() / "data",
        !stamp.empty() && stamp != _host.settings->sqlStamp);
    InstallOptions o;
    o.exe = _host.exe;
    o.setupDir = _host.setupDir;
    o.root = Root();
    o.profileFile = _host.settings->file;
    auto base = _host.settings->file.empty() ? _host.exe.parent_path() : _host.settings->file.parent_path();
    o.client = _client.empty() ? fs::path{} : Absolute(_client, base);
    o.location = Choice().location;
    o.cache = Choice().cache;
    o.remote = Choice().remote;
    o.locale = _host.settings->locale;
    o.serverEnv = Environment(Choice());
    if (_serverOnly)
        o.serverEnv.push_back({ "LONELYICE_SERVER_ONLY", "1" });
    o.db = plan.db || _setup;
    o.newDatabases = plan.newDatabases;
    o.unpack = plan.unpack;
    o.pack = plan.pack;
    o.vmaps = _setup && _vmaps;
    o.mmaps = _setup && _mmaps;
    o.client_prep = _setup && !_serverOnly;
    o.realmlist = o.clearWdb = o.accountName = o.client_prep;
    o.shortcut = false;
    o.realmName = _setup ? _realm : std::string{};
    o.login = _setup ? _login : std::string{};
    o.password = _setup ? _accountPassword : std::string{};
    // SQL deployment applies client patches, so even a server-only prepare needs a valid client for this step.
    if ((o.db || o.unpack || o.vmaps || o.mmaps || o.client_prep || !o.cache) &&
        (o.client.empty() || !GameClient::Inspect(o.client).valid))
    {
        _notice = Tr("tui.storage.client_required");
        return;
    }
    std::error_code ec;
    if (o.db && (!fs::is_regular_file(o.setupDir / "sql.pak", ec) || !fs::is_regular_file(o.setupDir / "configs.pak", ec)))
    {
        _notice = Tr("tui.storage.setup_missing");
        return;
    }
    _review = std::move(o);
    _reviewedFingerprint = Fingerprint();
    _confirm = _confirmRemoval = false;
    _notice = Tr("tui.storage.review_ready");
}

void StorageSetupView::Apply()
{
    if (!CanOperate())
        return;
    if (!_review || _reviewedFingerprint != Fingerprint() || !_confirm)
    {
        _notice = Tr("tui.storage.confirm_required");
        return;
    }
    if (RemovingGeneratedData(*_review) && !_confirmRemoval)
    {
        _notice = Tr("tui.storage.removal_required");
        return;
    }
    _installPending = true;
    _cancelRequested = false;
    _log.clear();
    _installer.Start(*_review);
    _notice = Tr("tui.storage.preparing");
    if (_wizard)
        _wizardStep = 4;
}

void StorageSetupView::Commit()
{
    auto s = *_host.settings;
    auto const& o = *_review;
    s.location = o.location;
    s.dataCache = o.cache;
    if (o.location != "local")
        s.remote = o.remote;
    s.dataRoot = o.root;
    s.clientPath = o.client;
    s.serverConfig = o.root / ".runtime" / "configs" / "worldserver.conf";
    if (!o.realmName.empty())
        s.realmName = o.realmName;
    if (o.db)
        s.sqlStamp = Installer::SqlStamp(o.setupDir);
    std::string profileError;
    if (!Installer::SaveProfileOptions(o, profileError))
    {
        _notice = Tr("tui.storage.save_failed") + " " + profileError;
        Edited();
        return;
    }
    if (!s.Save())
    {
        _notice = Tr("tui.storage.save_failed");
        Edited();
        return;
    }
    *_host.settings = std::move(s);
    if (_host.committed)
        _host.committed();
    Reload();
    _notice = Tr("tui.storage.saved");
    if (_wizard)
        _wizardStep = 5;
}

bool StorageSetupView::Tick()
{
    bool changed = false;
    auto now = Fingerprint();
    if (now != _seen)
    {
        _seen = now;
        Edited();
        _notice = Tr("tui.storage.edited");
        changed = true;
    }
    if (auto result = _check.Take())
    {
        if (_checkedFingerprint == now)
        {
            _result = std::move(result);
            _notice = !_result->reached ? _result->error : !_result->databases ? Tr("tui.storage.empty")
                : !Choice().cache ? Tr("tui.storage.ready_databases")
                : !_result->HasDbc() ? Tr("tui.storage.missing_cache") : Tr("tui.storage.ready");
        }
        changed = true;
    }
    if (_wizardCheckPending && !_check.IsRunning())
    {
        _wizardCheckPending = false;
        if (_result && _result->reached)
            WizardNext();
        changed = true;
    }
    auto lines = _installer.TakeLog();
    if (!lines.empty())
    {
        _log.insert(_log.end(), lines.begin(), lines.end());
        if (_log.size() > 120)
            _log.erase(_log.begin(), _log.end() - 120);
        changed = true;
    }
    if (_installPending && !_installer.IsRunning() && _installer.Finished())
    {
        _installPending = false;
        if (_installer.Succeeded() && !_cancelRequested)
            Commit();
        else
        {
            _notice = Tr("tui.storage.failed", _cancelRequested ? Tr("install.note.aborted") : _installer.Error());
            // A partial preparation can change the destination: force a fresh check before retrying.
            Edited();
            if (_wizard)
                _wizardStep = 2;
        }
        if (_wizard && _wizardStep == 4)
            _wizardStep = 2; // A failed profile commit must remain retryable too.
        changed = true;
    }
    return changed;
}

Component StorageSetupView::Build()
{
    if (_component)
        return _component;
    auto input = [&](std::string* value, bool password = false)
    {
        InputOption opt;
        opt.password = password;
        opt.multiline = false;
        opt.on_change = [this] { Edited(); };
        return Input(value, opt);
    };
    MenuOption menuOptions;
    menuOptions.on_change = [this]
    {
        if (_location > 0 && _location <= int(_providers.size()) && _port.empty())
            _port = _providers[_location - 1].provider.port;
        Edited();
    };
    auto locations = Menu(&_locations, &_location, menuOptions);
    auto cache = Checkbox(Tr("tui.storage.cache"), &_cache);
    auto host = input(&_remoteHost), port = input(&_port), user = input(&_user);
    auto pass = input(&_password, true), prefix = input(&_prefix);
    auto client = input(&_client), root = input(&_root);
    _wizardFirst = root;
    auto setup = Checkbox(Tr("tui.storage.setup"), &_setup);
    auto serverOnly = Checkbox(Tr("tui.storage.server_only"), &_serverOnly);
    auto vmaps = Checkbox(Tr("tui.storage.vmaps"), &_vmaps), mmaps = Checkbox(Tr("tui.storage.mmaps"), &_mmaps);
    auto realm = input(&_realm), login = input(&_login), accountPass = input(&_accountPassword, true);
    auto confirm = Checkbox(Tr("tui.storage.confirm"), &_confirm);
    auto removal = Checkbox(Tr("tui.storage.confirm_removal"), &_confirmRemoval);
    auto check = Button(Tr("tui.storage.check"), [this] { Check(); });
    auto review = Button(Tr("tui.storage.review"), [this] { Review(); });
    auto apply = Button(Tr("tui.storage.apply"), [this] { Apply(); });
    auto startWizard = Button(Tr("tui.wizard.start"), [this] { StartWizard(); });
    auto back = Button(Tr("tui.wizard.back"), [this]
    {
        if (_wizardStep > 0 && _wizardStep < 4)
            --_wizardStep;
    });
    auto next = Button(Tr("tui.wizard.next"), [this] { WizardNext(); });
    auto closeWizard = Button(Tr("tui.wizard.close"), [this]
    {
        _wizard = false;
        _wizardCheckPending = false;
        _wizardFirst->TakeFocus();
    });
    // One focus list lets Tab leave every group, including path and connection inputs.
    Components fields;
    auto add = [&](Component field, std::function<bool()> visible)
    {
        fields.push_back(Maybe(field, std::move(visible)));
    };
    add(startWizard, [this] { return !_wizard; });
    for (auto field : { root, client })
        add(field, [this] { return !_wizard || _wizardStep == 0; });
    add(locations, [this] { return !_wizard || _wizardStep == 1; });
    add(cache, [this] { return (!_wizard || _wizardStep == 1) && !Choice().Remote(); });
    for (auto field : { host, port, user, pass, prefix })
        add(field, [this] { return (!_wizard || _wizardStep == 1) && Choice().Remote(); });
    add(setup, [this] { return !_wizard; });
    for (auto field : { serverOnly, vmaps, mmaps, realm, login, accountPass })
        add(field, [this] { return _setup && (!_wizard || _wizardStep == 2); });
    for (auto field : { check, review })
        add(field, [this] { return !_wizard; });
    add(confirm, [this] { return _review && (!_wizard || _wizardStep == 3); });
    add(removal, [this] { return _review && (!_wizard || _wizardStep == 3) && RemovingGeneratedData(*_review); });
    add(apply, [this] { return _review && (!_wizard || _wizardStep == 3); });
    add(back, [this] { return _wizard && _wizardStep > 0 && _wizardStep < 4; });
    add(next, [this] { return _wizard && _wizardStep < 3; });
    add(closeWizard, [this] { return _wizard; });
    auto content = Container::Vertical(std::move(fields));
    auto render = Renderer(content, [=, this]
    {
        auto row = [](std::string const& key, Component const& field)
        {
            return hbox({ text(Tr(key) + ": ") | size(WIDTH, EQUAL, 22), field->Render() | flex });
        };
        Elements out;
        if (_wizard)
        {
            out.push_back(text(Tr("tui.wizard.title", _wizardStep + 1, 6)) | bold);
            out.push_back(paragraph(Tr("tui.wizard.step." + std::to_string(_wizardStep))));
            out.push_back(separator());
        }
        else
        {
            out.push_back(text(Tr("tui.storage.title")) | bold);
            out.push_back(startWizard->Render());
            out.push_back(paragraph(Tr("tui.storage.switch_warning")));
        }
        if (!_wizard || _wizardStep == 0)
        {
            out.push_back(paragraph(Tr("tui.wizard.paths_note")));
            out.push_back(row("tui.storage.root", root));
            out.push_back(row("tui.storage.client", client));
        }
        if (!_wizard || _wizardStep == 1)
        {
            out.push_back(locations->Render());
            if (Choice().Remote())
            {
                out.push_back(paragraph(Tr("tui.storage.remote_note")));
                for (auto const& item : std::vector<std::pair<std::string, Component>>{
                    { "tui.storage.host", host }, { "tui.storage.port", port }, { "tui.storage.user", user },
                    { "tui.storage.password", pass }, { "tui.storage.prefix", prefix } })
                    out.push_back(row(item.first, item.second));
            }
            else
                out.push_back(cache->Render());
        }
        if (!_wizard)
            out.push_back(setup->Render());
        if (_setup && (!_wizard || _wizardStep == 2))
        {
            out.push_back(serverOnly->Render());
            out.push_back(paragraph(Tr("tui.storage.client_note")));
            out.push_back(hbox({ vmaps->Render(), text("   "), mmaps->Render() }));
            out.push_back(row("tui.storage.realm", realm));
            out.push_back(row("tui.storage.login", login));
            out.push_back(row("tui.storage.account_password", accountPass));
        }
        if (!_wizard)
            out.push_back(hbox({ check->Render(), review->Render() }));
        out.push_back(paragraph(_notice));
        if (_review && (!_wizard || _wizardStep == 3))
        {
            auto const& o = *_review;
            out.push_back(separator());
            out.push_back(paragraph(Tr("tui.storage.summary", _locations[_location], Platform::PathToUtf8(o.root),
                Tr(o.db ? "tui.storage.yes" : "tui.storage.no"), Tr(o.unpack ? "tui.storage.yes" : "tui.storage.no"),
                Tr(o.pack ? "tui.storage.yes" : "tui.storage.no"), Tr(o.vmaps ? "tui.storage.yes" : "tui.storage.no"),
                Tr(o.mmaps ? "tui.storage.yes" : "tui.storage.no"), Tr(o.client_prep ? "tui.storage.yes" : "tui.storage.no"))));
            if (Choice().Remote())
                out.push_back(paragraph(Tr("tui.storage.connection_summary", o.remote.host, o.remote.port, o.remote.user, o.remote.prefix)));
            if (_setup)
                out.push_back(paragraph(Tr("tui.storage.setup_summary", o.realmName, o.login.empty() ? Tr("tui.storage.no_account") : o.login,
                    Platform::PathToUtf8(o.client))));
            out.push_back(confirm->Render());
            if (RemovingGeneratedData(*_review))
                out.push_back(removal->Render());
            out.push_back(apply->Render());
        }
        if (!_wizard || _wizardStep >= 4)
            for (auto const& step : _installer.Steps())
                out.push_back(hbox({ text(step.title + " ") | size(WIDTH, EQUAL, 24), gauge(step.progress) | flex,
                    text(" " + Tr("tui.storage.step." + std::to_string(int(step.state)))) }));
        if ((!_wizard || _wizardStep >= 4) && !_log.empty())
            for (auto it = _log.begin() + std::max<int>(0, int(_log.size()) - 4); it != _log.end(); ++it)
                out.push_back(paragraph(*it) | dim);
        if (_wizard)
        {
            if (_wizardStep == 5)
                out.push_back(paragraph(Tr("tui.wizard.done", Platform::PathToUtf8(_host.settings->file))));
            Elements navigation;
            if (_wizardStep > 0 && _wizardStep < 4)
                navigation.push_back(back->Render());
            if (_wizardStep < 3)
                navigation.push_back(next->Render());
            navigation.push_back(closeWizard->Render());
            out.push_back(hbox(std::move(navigation)));
        }
        if (Busy())
            out.push_back(text(Tr("tui.storage.escape")) | bold);
        return vbox(std::move(out)) | vscroll_indicator | frame | flex;
    });
    _component = CatchEvent(render, [this](Event event)
    {
        if (!Busy())
            return false;
        if (event == Event::Escape)
            Cancel();
        return true;
    });
    return _component;
}
