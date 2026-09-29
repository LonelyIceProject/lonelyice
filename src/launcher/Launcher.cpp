#include "Assets.h"
#include "Backup.h"
#include "CommandCatalog.h"
#include "ConfFile.h"
#include "GameClient.h"
#include "GitRevision.h"
#include "Lang.h"
#include "Platform.h"
#include "LauncherSettings.h"
#include "ServerProcess.h"
#include "SettingsModel.h"
#include "StorageForm.h"
#include "ClientPatch.h"
#include "ConfigEnv.h"
#include "PackageManager.h"
#include "TextUtil.h"
#include "Tray.h"
#include "UiBackend.h"
#include "Wizard.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    constexpr int MaxLogLines = 600;
    constexpr char RealmHost[] = "127.0.0.1";

    struct Opt { Rml::String id, label; };
    struct LocaleChip { Rml::String name, cls; bool launch = false; };
    struct EventRow { Rml::String time, text; };
    struct AccRow { Rml::String name, level, chars, last; bool gm = false; };
    struct CmdArgView { Rml::String name, type, value; std::vector<Opt> opts; };
    struct CmdCardView
    {
        Rml::String title, desc, group, preview, button;
        bool danger = false, armed = false;
        std::vector<CmdArgView> args;
        int groupIdx = 0, cmdIdx = 0;
    };
    struct GroupView { Rml::String id, name; int changed = 0; Rml::String section; };   // section: settings side list heading
    struct FieldView
    {
        Rml::String label, key, type, value, apply, apply_text, hint;
        bool on = false, changed = false;
        std::vector<Opt> opts;
        int index = 0;
    };
    struct DataRow { Rml::String title, detail, status, size; };
    struct NewsView { Rml::String kind, title, text, action, button; };   // kind: info, warn, update
    struct NavItem { Rml::String id, name, section; int badge = 0; };
    struct PluginView
    {
        Rml::String id, name, version, desc, note, update;   // update: newer version in the index
        Rml::String icon, letter;                            // icon: absolute path of a PNG; letter: shown without one
        Rml::String settings;                                // settings group the gear opens, empty: none
        bool installed = false, enabled = false;
    };
    struct RepoView
    {
        Rml::String location, title, kind, note;
        bool enabled = false, ok = false, failed = false;
    };

    bool IsRemote(std::string const& location)
    {
        return location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0;
    }

    // First character of a UTF-8 string, upper-cased for Latin and Cyrillic.
    std::string Initial(std::string const& s)
    {
        return FirstLetterUpper(s);
    }

    std::string UiPath(fs::path const& p)
    {
        std::u8string const s = p.generic_u8string();
        return std::string(s.begin(), s.end());
    }

    // Short name of a package index: its own name, else the host or the folder.
    std::string RepoTitle(std::string const& location, std::string const& name)
    {
        if (!name.empty())
            return name;
        if (location == LauncherSettings::DefaultPackageIndex)
            return Tr("repo.official");
        if (IsRemote(location))
        {
            std::size_t const start = location.find("://") + 3;
            return location.substr(start, location.find('/', start) - start);
        }
        fs::path p = fs::u8path(location);
        if (p.filename() == "index.json")
            p = p.parent_path();
        std::u8string const s = p.filename().u8string();
        return s.empty() ? location : std::string(s.begin(), s.end());
    }

    std::string Now(char const* fmt = "%H:%M")
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        tm = Platform::LocalTime(t);
        char buf[32];
        std::strftime(buf, sizeof(buf), fmt, &tm);
        return buf;
    }

    std::string FormatUptime(uint64_t s)
    {
        if (s < 60)
            return Tr("time.seconds", s);
        uint64_t m = s / 60, h = m / 60, d = h / 24;
        if (d)
            return Tr("time.days_hours", d, h % 24);
        if (h)
            return Tr("time.hours_minutes", h, m % 60);
        return Tr("time.minutes", m);
    }

    std::string FormatBytes(uint64_t b)
    {
        char buf[32];
        if (b >= (1ull << 30))
        {
            snprintf(buf, sizeof(buf), "%.1f", double(b) / double(1ull << 30));
            std::string n = buf;
            std::replace(n.begin(), n.end(), '.', Tr("unit.decimal").front());
            return Tr("unit.gb", n);
        }
        if (b >= (1ull << 20))
            return Tr("unit.mb", b >> 20);
        return Tr("unit.kb", (b + 1023) >> 10);
    }

    // "2026-09-28 19:12:40" -> "today, 19:12" or the date
    std::string FormatLogin(std::string const& ts)
    {
        if (ts.size() < 16 || ts.rfind("0000", 0) == 0)
            return "—";
        if (ts.substr(0, 10) == Now("%Y-%m-%d"))
            return Tr("time.today_at", ts.substr(11, 5));
        return Tr("time.date", ts.substr(8, 2), ts.substr(5, 2), ts.substr(0, 4));
    }

    struct DirStats
    {
        uint64_t bytes = 0;
        uint32_t files = 0;
    };

    DirStats Scan(fs::path const& dir)
    {
        DirStats s;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec))
            {
                s.bytes += it->file_size(ec);
                ++s.files;
            }
        }
        return s;
    }

    fs::path DefaultServerConfig(fs::path const& exeDir)
    {
        for (char const* rel : { "configs-sqlite/worldserver.conf", "configs/worldserver.conf" })
            if (fs::exists(exeDir / rel))
                return exeDir / rel;
        return exeDir / "configs" / "worldserver.conf";
    }

    class Launcher : public Rml::EventListener
    {
    public:
        int Run(int argc, char** argv);

        void ProcessEvent(Rml::Event& ev) override
        {
            if (ev.GetId() == Rml::EventId::Keydown && ev.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_RETURN)
                SendConsoleCommand();
        }

    private:
        bool SetupUi();
        void BindModel(Rml::DataModelConstructor& c);
        void SetupTray();
        void Tick();
        void RunUiScript();
        void OnStateChanged(ServerState prev, ServerState now);
        void RefreshServerView();
        void RefreshClient();
        void RefreshTray();
        void AppendLog(std::string const& utf8, char const* cls);
        void AddEvent(std::string text);
        void Message(std::string text);
        void OpenTab(std::string const& tab);
        void OpenPage(std::string const& page, std::string const& section = {});
        bool LoadDocument();
        void Relocalize();
        void RefreshNews();
        void RefreshFooter();
        void RefreshBackups();
        void RefreshAbout();
        void ShowWindow();
        void BeginQuit();
        void SetUiScale(int percent);

        void StartServer();
        void ToggleServer();
        void RestartServer();
        void Play();
        void LaunchGame();
        void FixRealmlist(std::vector<std::string> const& locales, bool quiet);
        std::string LaunchLocale() const;
        void BrowseClient();
        void SendConsoleCommand();
        void RunCommand(std::string const& cmd, bool echo = true);

        void ApplyAccounts();
        void CreateAccount();

        void BuildCommandCards();
        void SyncCommandCards();
        void RunCard(int index);

        void LoadSettingsModel();
        void BuildSettingsFields();
        void SyncSettingsFields();
        void RefreshSettingsStatus();
        void SaveSettings();

        void RefreshData();
        void RefreshPlugins();
        void CheckPackageIndex();
        std::string IndexStatus() const;
        void InstallPackages(std::map<std::string, std::string> const& requests, bool update);
        void PluginAction(std::string const& action, int index);
        bool PluginsLocked();
        void RefreshRepos();
        void RepoAction(std::string const& action, int index);
        void AddRepo(std::string location);
        void SaveRepos(std::vector<std::string> const& on, std::vector<std::string> const& off);
        void StartBackup(bool scheduled);
        void CheckScheduledBackup();

        bool NeedsSetup() const;
        void OpenWizard();
        std::string ServerLocale() const;
        StorageChoice CurrentStorage() const;
        std::vector<StorageProviderInfo> StorageProviders() const;
        std::optional<StorageProviderInfo> FindProvider(std::string const& location) const;
        EnvList RemoteEnv(StorageChoice const& choice) const;
        void LoadStorageForm();
        void ApplyStorage();
        void FinishApplyStorage(StorageState const& state);
        void StorageTick();
        fs::path Root() const;
        fs::path ServerConfig() const;
        fs::path ConfPath(std::string const& key, std::string const& def) const;

        fs::path _exe, _exeDir;
        LauncherSettings _settings;
        std::unique_ptr<ServerProcess> _server;
        std::unique_ptr<Wizard> _wizard;
        ClientInfo _client;
        SettingsModel _settingsModel;
        Tray _tray;
        SDL_Surface* _icon = nullptr;
        std::vector<unsigned char> _iconPixels;

        Rml::Context* _ctx = nullptr;
        Rml::ElementDocument* _doc = nullptr;
        Rml::Element* _log = nullptr;
        Rml::DataModelHandle _model;

        // model: server and client
        Rml::String _state = "stopped", _stateTitle, _stateSub, _toggleLabel, _uptime = "—", _players = "—", _bots = "—", _diff = "—", _memory = "—";
        Rml::String _authPort = "3724", _worldPort = "8085", _soapPort = "7878", _clientPath, _rlNote, _message, _playLabel, _playSub, _tab = "overview", _shownTab = "overview";
        Rml::String _logsDir;
        // pages: main (tabs), settings, plugins, service; the footer and the language switch are on every page
        Rml::String _page = "main", _uiLang = "en", _footStatus;
        bool _footBusy = false;
        std::vector<Opt> _langs;
        std::vector<NewsView> _news;
        std::deque<std::pair<std::string, std::string>> _logLines;   // text, class: re-added after a language switch
        Rml::String _svcSection = "data";
        std::vector<DataRow> _backupRows, _logRows, _aboutRows;
        Rml::String _backupSum;
        bool _running = false, _authOn = false, _worldOn = false, _soapEnabled = false, _clientOk = false, _playEnabled = true, _closing = false, _backupBusy = false;
        std::vector<LocaleChip> _locales;
        std::vector<EventRow> _events;
        // accounts
        std::vector<AccRow> _accRows;
        Rml::String _accNote = Tr("acc.start_server"), _accLogin, _accPass, _accLevel = "0";
        bool _accLoaded = false;
        // commands
        std::vector<GroupView> _cmdGroups;
        std::vector<CmdCardView> _cmdCards;
        std::vector<Opt> _cmdChars;
        Rml::String _cmdGroup = "srv", _cmdQuery, _cmdQueryShown, _cmdTarget, _cmdLast = Tr("cmd.last_hint");
        // settings
        std::vector<GroupView> _setGroups;
        std::vector<FieldView> _setFields;
        Rml::String _setGroup = "rates", _setHint, _setStatus, _setSaveLabel = Tr("set.save");
        bool _setCanSave = false, _setShowKeys = true;
        int _setPreset = 0;
        // data
        std::vector<DataRow> _dataRows;
        Rml::String _dataSum;
        Rml::String _storageTitle, _storageDetail;
        // settings > storage: the form (st_*), what it was loaded from, and an apply waiting for its check
        std::unique_ptr<StorageForm> _storageForm;
        Rml::String _stCurrent, _stStatus;
        bool _stChanged = false, _stBusy = false, _stApplying = false;
        // plugins
        std::unique_ptr<Packages::Manager> _packages;
        std::vector<PluginView> _plRows;
        std::vector<RepoView> _plRepos;
        Rml::String _plView = "installed", _plRepoNew;     // installed, updates, catalog, repos
        int _plUpdates = 0;
        bool _plHasOfficial = true;
        std::string _pickedRepo;
        Rml::String _plStatus = Tr("pl.status.not_loaded");
        bool _plBusy = false, _plIndexLoaded = false;
        std::thread _plThread;
        std::atomic<bool> _plDone{ false };
        std::string _plError, _plNewStatus;   // written by the plugins thread, taken over in Tick
        std::vector<std::string> _plLog;

        ServerState _lastState = ServerState::Stopped;
        bool _pendingPlay = false, _pendingRestart = false, _quitting = false, _startHidden = false, _scriptClose = false;
        uint64_t _lastStatsTick = 0, _accRefreshAt = 0, _lastBackupCheck = 0;
        std::unique_ptr<Platform::Child> _game;

        std::mutex _asyncLock;
        std::string _pickedDir;
        std::thread _backupThread;
        std::atomic<bool> _backupDone{ false };
        BackupResult _backupResult;
        // client addons and patches, prepared before the game starts
        std::thread _syncThread;
        std::atomic<bool> _syncDone{ false };
        bool _syncBusy = false;
        ClientPatch::Result _syncResult;
    };

    // No config or no world database: the wizard has to run first.
    bool Launcher::NeedsSetup() const
    {
        std::error_code ec;
        if (!fs::exists(ServerConfig(), ec))
            return true;
        // On a database server the files of the config are not used.
        if (_settings.location == "local")
            for (DatabaseFile const& db : FindDatabases(ServerConfig(), Root()))
                if (db.name == "world" && !db.path.empty() && !fs::exists(db.path, ec))
                    return true;
        // With the cache, an interrupted install leaves the databases but no maps; reading the client needs nothing more.
        if (_settings.ReadsClient())
            return false;
        fs::path maps = ConfPath("DataDir", ".") / "maps";
        return !fs::is_directory(maps, ec) || fs::is_empty(maps, ec);
    }

    StorageChoice Launcher::CurrentStorage() const
    {
        return { _settings.location, _settings.dataCache, _settings.remote };
    }

    // The storages of the installed (enabled) plugins.
    std::vector<StorageProviderInfo> Launcher::StorageProviders() const
    {
        std::vector<StorageProviderInfo> list;
        for (PluginManifest const& p : ReadPlugins(_exeDir / "plugins"))
            if (p.storage)
                list.push_back({ *p.storage, p.dir });
        return list;
    }

    std::optional<StorageProviderInfo> Launcher::FindProvider(std::string const& location) const
    {
        for (StorageProviderInfo const& p : StorageProviders())
            if (p.provider.id == location)
                return p;
        return std::nullopt;
    }

    // The server's databases on a plugin's database server; empty for the built-in files or a plugin that is gone.
    EnvList Launcher::RemoteEnv(StorageChoice const& choice) const
    {
        std::optional<StorageProviderInfo> const p = choice.Remote() ? FindProvider(choice.location) : std::nullopt;
        if (!p)
            return {};
        return RemoteDatabaseOverrides(p->provider, choice.remote, p->dir / "server" / Packages::Manager::Platform());
    }

    // settings > storage: the form shows the storage in use.
    void Launcher::LoadStorageForm()
    {
        _storageForm->Load(CurrentStorage());
        _stCurrent = _settings.location;
        _stChanged = false;
        _stApplying = false;
        _stStatus = Tr("storage.status.current", _storageForm->LocationTitle(_settings.location),
            Tr(_settings.dataCache ? "storage.status.cache" : "storage.status.client"));
        for (char const* v : { "st_current", "st_changed", "st_status", "st_busy" })
            _model.DirtyVariable(v);
    }

    // Moves the server to the storage of the form: its check says what is missing there. Nothing missing, the
    // settings change at once; otherwise the wizard shows what will be done and does it.
    void Launcher::ApplyStorage()
    {
        if (_stBusy || !_stChanged)
            return;
        if (_server->IsRunning())
        {
            Message(Tr("msg.storage_server_running"));
            return;
        }
        StorageChoice const to = _storageForm->Choice();
        if (!to.cache && !_client.valid)
        {
            Message(Tr("msg.client_data_needs_client"));
            return;
        }
        if (to.Remote() && (to.remote.host.empty() || to.remote.user.empty() || to.remote.prefix.empty()))
        {
            Message(Tr("msg.remote_incomplete"));
            return;
        }
        // checked again: the place may have changed since the form looked
        _stApplying = true;
        _storageForm->Check();
        _stStatus = Tr("storage.status.checking");
        _model.DirtyVariable("st_status");
    }

    void Launcher::FinishApplyStorage(StorageState const& state)
    {
        _stApplying = false;
        StorageChoice const to = _storageForm->Choice();
        if (!state.reached)
        {
            _stStatus = Tr("storage.status.unreachable");
            _model.DirtyVariable("st_status");
            Message(state.error);
            return;
        }
        StoragePlan const plan = PlanStorage(state, to.cache, ConfPath("DataDir", "."));
        if (plan.Empty())
        {
            _settings.location = to.location;
            _settings.dataCache = to.cache;
            if (to.Remote())
                _settings.remote = to.remote;
            _settings.Save();
            AddEvent(Tr("event.storage_switched", _storageForm->LocationTitle(to.location)));
            Message(Tr("msg.storage_switched"));
            LoadStorageForm();
            RefreshData();
            RefreshNews();
            return;
        }
        if (!_client.valid && (plan.db || plan.unpack))
        {
            Message(Tr("msg.client_data_needs_client"));
            return;
        }
        _stStatus = Tr("storage.status.wizard");
        _model.DirtyVariable("st_status");
        ShowWindow();
        _wizard->SwitchStorage(_client.dir, to, _storageForm->LocationTitle(to.location), plan, !state.databases, _settings.realmName);
    }

    // Follows the storage form: its check, what differs from the storage in use, an apply that waits for the check.
    void Launcher::StorageTick()
    {
        if (!_storageForm)
            return;
        bool const checked = _storageForm->Tick();
        bool const changed = !_storageForm->Choice().Same(CurrentStorage());
        bool const busy = _storageForm->Checking() || _wizard->IsInstalling();
        if (changed != _stChanged || busy != _stBusy)
        {
            _stChanged = changed;
            _stBusy = busy;
            _model.DirtyVariable("st_changed");
            _model.DirtyVariable("st_busy");
        }
        if (checked && _stApplying && _storageForm->Result())
            FinishApplyStorage(*_storageForm->Result());
    }

    // Client locale the server reads its data in: the one the game starts in, else the client's own setting.
    std::string Launcher::ServerLocale() const
    {
        if (!_settings.locale.empty())
            return _settings.locale;
        return _client.valid ? GameClient::ReadConfigLocale(_client.dir) : std::string();
    }

    void Launcher::OpenWizard()
    {
        ShowWindow();
        _wizard->Open(_client.valid ? _client.dir : fs::path());
    }

    fs::path Launcher::Root() const
    {
        return _settings.dataRoot.empty() ? _exeDir : fs::path(_settings.dataRoot);
    }

    fs::path Launcher::ServerConfig() const
    {
        if (!_settings.serverConfig.empty())
            return _settings.serverConfig;
        return _settings.dataRoot.empty() ? DefaultServerConfig(_exeDir) : Root() / "configs" / "worldserver.conf";
    }

    // A path option of worldserver.conf, resolved against the server folder.
    fs::path Launcher::ConfPath(std::string const& key, std::string const& def) const
    {
        ConfFile f;
        f.Load(ServerConfig());
        fs::path p = fs::u8path(f.Get(key).value_or(def));
        return p.is_absolute() ? p : Root() / p;
    }

    int Launcher::Run(int argc, char** argv)
    {
        _exe = Platform::ExePath();
        _exeDir = _exe.parent_path();

        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--tray")
                _startHidden = true;

        _settings.file = _exeDir / "lonelyice.ini";
        _packages = std::make_unique<Packages::Manager>(_exeDir / "plugins");
        _settings.Load();
        _uiLang = Lang::Code();
        for (Lang::Info const& l : Lang::Available())
            _langs.push_back({ l.code, l.name });

        _server = std::make_unique<ServerProcess>([] { UiBackend::Wake(); });
        _wizard = std::make_unique<Wizard>(Wizard::Host{ _exe, _exeDir, [this] { return Root(); }, [this] { return _server->IsRunning(); },
            [this] { return _settings.sqlStamp; },
            [this] { return CurrentStorage(); },
            [this] { return StorageProviders(); },
            [this](StorageChoice const& c) { return RemoteEnv(c); },
            [this] { return ServerLocale(); },
            [this](InstallOptions const& o)
            {
                _settings.dataRoot = o.root;
                _settings.serverConfig.clear();
                _settings.clientPath = o.client;
                _settings.location = o.location;
                _settings.dataCache = o.cache;
                if (o.location != "local")
                    _settings.remote = o.remote;
                if (o.db)
                {
                    _settings.sqlStamp = Installer::SqlStamp(o.setupDir);
                    _settings.realmName = o.realmName;
                    _settings.pendingRealmName.clear();
                }
                _settings.Save();
                RefreshClient();
                LoadSettingsModel();
                if (_page == "settings" && _setGroup == "storage")
                    LoadStorageForm();
                RefreshData();
                RefreshNews();
                AddEvent(Tr("event.install_done", Platform::PathToUtf8(o.root)));
            },
            [this] { Play(); }, [] { UiBackend::Wake(); } });
        // the storage form checks the server folder's built-in databases for "local"
        _storageForm = std::make_unique<StorageForm>("st_", StorageForm::Host{ _exe, [this] { return StorageProviders(); },
            [this](StorageChoice const& c) { return c.Remote() ? RemoteEnv(c) : LocalDatabaseOverrides(Root()); },
            [] { UiBackend::Wake(); } });

        if (!UiBackend::Initialize("LonelyIce", 960, 680, _settings.uiScale / 100.f))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LonelyIce", Tr("error.no_opengl").c_str(), nullptr);
            return 1;
        }
        _settings.uiScale = int(UiBackend::GetUiScale() * 100.f + 0.5f);

        static AssetFileInterface assets;
        Rml::SetFileInterface(&assets);
        Rml::SetSystemInterface(UiBackend::GetSystemInterface());
        Rml::SetRenderInterface(UiBackend::GetRenderInterface());
        Rml::Initialise();

        if (!SetupUi())
        {
            Rml::Shutdown();
            UiBackend::Shutdown();
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LonelyIce", Tr("error.no_ui").c_str(), nullptr);
            return 1;
        }

        SetupTray();
        RefreshClient();
        LoadSettingsModel();
        BuildCommandCards();
        RefreshServerView();

        if (NeedsSetup())
            OpenWizard();
        else
        {
            std::string stamp = Installer::SqlStamp(_exeDir / "setup");
            if (!stamp.empty() && stamp != _settings.sqlStamp)
                AddEvent(Tr("event.sql_updates"));
            if (_startHidden && _settings.trayOnClose)
                UiBackend::HideWindow();
            if (_settings.autoStart)
                StartServer();
            CheckPackageIndex();    // news about plugin updates
        }
        RefreshNews();

        for (;;)
        {
            bool closeRequested = false;
            bool alive = UiBackend::ProcessEvents(_ctx, UiBackend::IsWindowVisible() ? 0.5 : 1.0, closeRequested);
            closeRequested = closeRequested || std::exchange(_scriptClose, false);
            if (!alive)
            {
                // The session ends: do not wait for anything.
                _server->Stop();
                break;
            }

            if (closeRequested)
            {
                if (_quitting || _closing)
                {
                    _server->Kill();
                    break;
                }
                if (_settings.trayOnClose)
                {
                    UiBackend::HideWindow();
                    _tray.SetTooltip(Tr("tray.tooltip_hidden", _stateTitle));
                }
                else
                    BeginQuit();
            }

            Tick();

            if (_quitting && !_server->IsRunning())
                break;

            if (UiBackend::IsWindowVisible())
            {
                _ctx->Update();
                if (_tab != _shownTab)
                {
                    // A hidden log has no layout, so jump to the newest lines once it becomes visible.
                    _shownTab = _tab;
                    if (_tab == "console" && _log)
                    {
                        _log->SetScrollTop(_log->GetScrollHeight());
                        _ctx->Update();
                    }
                }
                UiBackend::BeginFrame();
                _ctx->Render();
                UiBackend::PresentFrame();
            }
        }

        if (_backupThread.joinable())
            _backupThread.join();
        if (_syncThread.joinable())
            _syncThread.join();
        if (_plThread.joinable())
            _plThread.join();
        _settings.Save();
        _server.reset();
        _tray.Destroy();
        Rml::Shutdown();
        if (_icon)
            SDL_DestroySurface(_icon);
        UiBackend::Shutdown();
        return 0;
    }

    void Launcher::BeginQuit()
    {
        _quitting = true;
        if (_server->IsRunning())
        {
            ShowWindow();
            _closing = true;
            _pendingPlay = _pendingRestart = false;
            _server->Stop();
            _model.DirtyVariable("closing");
        }
    }

    void Launcher::ShowWindow()
    {
        UiBackend::ShowWindow();
    }

    void Launcher::SetUiScale(int percent)
    {
        UiBackend::SetUiScale(_ctx, percent / 100.f);
        if (percent == _settings.uiScale)
            return;
        _settings.uiScale = percent;
        _settings.Save();
        for (SetValue& sv : _settingsModel.Values())
            if (sv.def->key == "Launcher.UiScale")
                sv.orig = sv.cur = std::to_string(percent);
        if (_page == "settings")
            BuildSettingsFields();
        Message(Tr("msg.ui_scale", percent));
    }

    bool Launcher::SetupUi()
    {
        for (char const* font : { "fonts/PTSans-Regular.ttf", "fonts/PTSans-Bold.ttf", "fonts/PTMono-Regular.ttf", "fonts/Forum-Regular.ttf" })
            if (!Rml::LoadFontFace(font))
                return false;

        // 64x64 RGBA icon for the window and the tray
        if (Rml::FileHandle f = Rml::GetFileInterface()->Open("icons/round-64.rgba"))
        {
            _iconPixels.resize(Rml::GetFileInterface()->Length(f));
            Rml::GetFileInterface()->Read(_iconPixels.data(), _iconPixels.size(), f);
            Rml::GetFileInterface()->Close(f);
            if (_iconPixels.size() == 64 * 64 * 4)
            {
                _icon = SDL_CreateSurfaceFrom(64, 64, SDL_PIXELFORMAT_RGBA32, _iconPixels.data(), 64 * 4);
                UiBackend::SetIcon(_icon);
            }
        }

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(UiBackend::GetWindow(), &w, &h);
        _ctx = Rml::CreateContext("main", Rml::Vector2i(w, h));
        if (!_ctx)
            return false;
        _ctx->SetDensityIndependentPixelRatio(UiBackend::GetDpRatio());
        UiBackend::SetZoomHandler([](void* self, int steps)
        {
            auto* me = static_cast<Launcher*>(self);
            me->SetUiScale(std::clamp((me->_settings.uiScale + 12) / 25 * 25 + steps * 25, 100, 200));
        }, this);

        Rml::DataModelConstructor c = _ctx->CreateDataModel("li");
        if (!c)
            return false;
        BindModel(c);
        _wizard->Bind(c);
        _storageForm->Bind(c, false);       // the wizard's storage form registered the row type
        _model = c.GetModelHandle();
        _wizard->SetModel(_model);
        _storageForm->SetModel(_model);

        return LoadDocument();
    }

    // (Re)loads the page markup; static text is translated while it loads (UiBackend: "@{key}").
    bool Launcher::LoadDocument()
    {
        if (_doc)
        {
            _doc->Close();
            _ctx->Update();
        }
        _doc = _ctx->LoadDocument("ui/launcher.rml");
        if (!_doc)
            return false;
        _doc->Show();

        _log = _doc->GetElementById("log");
        if (Rml::Element* input = _doc->GetElementById("cmd"))
            input->AddEventListener(Rml::EventId::Keydown, this);
        for (auto const& [text, cls] : _logLines)
        {
            Rml::ElementPtr p = _doc->CreateElement("p");
            p->SetInnerRML(EscapeRml(text.empty() ? " " : text));
            if (!cls.empty())
                p->SetClass(cls, true);
            _log->AppendChild(std::move(p));
        }
        return true;
    }

    void Launcher::Relocalize()
    {
        Lang::Set(_uiLang);
        _settings.language = _uiLang;
        _settings.Save();
        LoadDocument();
        RefreshTray();
        RefreshClient();
        LoadSettingsModel();
        BuildCommandCards();
        RefreshPlugins();
        RefreshRepos();
        RefreshData();
        RefreshBackups();
        RefreshAbout();
        RefreshNews();
        _wizard->Relocalize();
        _storageForm->Relocalize();
        if (_page == "settings" && _setGroup == "storage")
            _stStatus = Tr("storage.status.current", _storageForm->LocationTitle(_settings.location),
                Tr(_settings.dataCache ? "storage.status.cache" : "storage.status.client"));
        if (!_plBusy)
            _plStatus = IndexStatus();
        if (_server->GetState() != ServerState::Ready)
            _accNote = Tr("acc.start_server");
        _cmdLast = Tr("cmd.last_hint");
        _model.DirtyAllVariables();
    }

    void Launcher::SetupTray()
    {
        _tray.Create(_icon, {
            { "status", "LonelyIce", nullptr },
            { "open", Tr("tray.open"), [this] { ShowWindow(); } },
            { "play", Tr("tray.play"), [this] { Play(); } },
            { "", "", nullptr },
            { "toggle", Tr("tray.start"), [this] { ToggleServer(); } },
            { "restart", Tr("tray.restart"), [this] { RestartServer(); } },
            { "", "", nullptr },
            { "tab-console", Tr("tab.console"), [this] { ShowWindow(); OpenTab("console"); } },
            { "tab-commands", Tr("tab.commands"), [this] { ShowWindow(); OpenTab("commands"); } },
            { "tab-accounts", Tr("tab.accounts"), [this] { ShowWindow(); OpenTab("accounts"); } },
            { "tab-settings", Tr("page.settings"), [this] { ShowWindow(); OpenPage("settings"); } },
            { "backup", Tr("tray.backup"), [this] { StartBackup(false); } },
            { "", "", nullptr },
            { "quit", Tr("tray.quit"), [this] { BeginQuit(); } },
        });
        _tray.SetEnabled("status", false);
    }

    void Launcher::BindModel(Rml::DataModelConstructor& c)
    {
        if (auto s = c.RegisterStruct<Opt>())
        {
            s.RegisterMember("id", &Opt::id);
            s.RegisterMember("label", &Opt::label);
        }
        c.RegisterArray<std::vector<Opt>>();

        if (auto s = c.RegisterStruct<LocaleChip>())
        {
            s.RegisterMember("name", &LocaleChip::name);
            s.RegisterMember("cls", &LocaleChip::cls);
            s.RegisterMember("launch", &LocaleChip::launch);
        }
        c.RegisterArray<std::vector<LocaleChip>>();

        if (auto s = c.RegisterStruct<EventRow>())
        {
            s.RegisterMember("time", &EventRow::time);
            s.RegisterMember("text", &EventRow::text);
        }
        c.RegisterArray<std::vector<EventRow>>();

        if (auto s = c.RegisterStruct<AccRow>())
        {
            s.RegisterMember("name", &AccRow::name);
            s.RegisterMember("level", &AccRow::level);
            s.RegisterMember("chars", &AccRow::chars);
            s.RegisterMember("last", &AccRow::last);
            s.RegisterMember("gm", &AccRow::gm);
        }
        c.RegisterArray<std::vector<AccRow>>();

        if (auto s = c.RegisterStruct<CmdArgView>())
        {
            s.RegisterMember("name", &CmdArgView::name);
            s.RegisterMember("type", &CmdArgView::type);
            s.RegisterMember("value", &CmdArgView::value);
            s.RegisterMember("opts", &CmdArgView::opts);
        }
        c.RegisterArray<std::vector<CmdArgView>>();

        if (auto s = c.RegisterStruct<CmdCardView>())
        {
            s.RegisterMember("title", &CmdCardView::title);
            s.RegisterMember("desc", &CmdCardView::desc);
            s.RegisterMember("group", &CmdCardView::group);
            s.RegisterMember("preview", &CmdCardView::preview);
            s.RegisterMember("button", &CmdCardView::button);
            s.RegisterMember("danger", &CmdCardView::danger);
            s.RegisterMember("armed", &CmdCardView::armed);
            s.RegisterMember("args", &CmdCardView::args);
        }
        c.RegisterArray<std::vector<CmdCardView>>();

        if (auto s = c.RegisterStruct<GroupView>())
        {
            s.RegisterMember("id", &GroupView::id);
            s.RegisterMember("name", &GroupView::name);
            s.RegisterMember("changed", &GroupView::changed);
            s.RegisterMember("section", &GroupView::section);
        }
        c.RegisterArray<std::vector<GroupView>>();

        if (auto s = c.RegisterStruct<FieldView>())
        {
            s.RegisterMember("label", &FieldView::label);
            s.RegisterMember("key", &FieldView::key);
            s.RegisterMember("type", &FieldView::type);
            s.RegisterMember("value", &FieldView::value);
            s.RegisterMember("apply", &FieldView::apply);
            s.RegisterMember("apply_text", &FieldView::apply_text);
            s.RegisterMember("hint", &FieldView::hint);
            s.RegisterMember("on", &FieldView::on);
            s.RegisterMember("changed", &FieldView::changed);
            s.RegisterMember("opts", &FieldView::opts);
        }
        c.RegisterArray<std::vector<FieldView>>();

        if (auto s = c.RegisterStruct<PluginView>())
        {
            s.RegisterMember("id", &PluginView::id);
            s.RegisterMember("name", &PluginView::name);
            s.RegisterMember("version", &PluginView::version);
            s.RegisterMember("desc", &PluginView::desc);
            s.RegisterMember("note", &PluginView::note);
            s.RegisterMember("update", &PluginView::update);
            s.RegisterMember("icon", &PluginView::icon);
            s.RegisterMember("letter", &PluginView::letter);
            s.RegisterMember("settings", &PluginView::settings);
            s.RegisterMember("installed", &PluginView::installed);
            s.RegisterMember("enabled", &PluginView::enabled);
        }
        c.RegisterArray<std::vector<PluginView>>();

        if (auto s = c.RegisterStruct<RepoView>())
        {
            s.RegisterMember("location", &RepoView::location);
            s.RegisterMember("title", &RepoView::title);
            s.RegisterMember("kind", &RepoView::kind);
            s.RegisterMember("note", &RepoView::note);
            s.RegisterMember("enabled", &RepoView::enabled);
            s.RegisterMember("ok", &RepoView::ok);
            s.RegisterMember("failed", &RepoView::failed);
        }
        c.RegisterArray<std::vector<RepoView>>();

        if (auto s = c.RegisterStruct<DataRow>())
        {
            s.RegisterMember("title", &DataRow::title);
            s.RegisterMember("detail", &DataRow::detail);
            s.RegisterMember("status", &DataRow::status);
            s.RegisterMember("size", &DataRow::size);
        }
        c.RegisterArray<std::vector<DataRow>>();

        if (auto s = c.RegisterStruct<NewsView>())
        {
            s.RegisterMember("kind", &NewsView::kind);
            s.RegisterMember("title", &NewsView::title);
            s.RegisterMember("text", &NewsView::text);
            s.RegisterMember("action", &NewsView::action);
            s.RegisterMember("button", &NewsView::button);
        }
        c.RegisterArray<std::vector<NewsView>>();

        c.Bind("page", &_page);
        c.Bind("ui_lang", &_uiLang);
        c.Bind("langs", &_langs);
        c.Bind("news", &_news);
        c.Bind("foot_status", &_footStatus);
        c.Bind("foot_busy", &_footBusy);
        c.Bind("svc_section", &_svcSection);
        c.Bind("backup_rows", &_backupRows);
        c.Bind("backup_sum", &_backupSum);
        c.Bind("log_rows", &_logRows);
        c.Bind("about_rows", &_aboutRows);
        c.Bind("pl_updates", &_plUpdates);

        c.Bind("state", &_state);
        c.Bind("state_title", &_stateTitle);
        c.Bind("state_sub", &_stateSub);
        c.Bind("toggle_label", &_toggleLabel);
        c.Bind("running", &_running);
        c.Bind("auth_on", &_authOn);
        c.Bind("world_on", &_worldOn);
        c.Bind("soap_enabled", &_soapEnabled);
        c.Bind("auth_port", &_authPort);
        c.Bind("world_port", &_worldPort);
        c.Bind("soap_port", &_soapPort);
        c.Bind("uptime", &_uptime);
        c.Bind("players", &_players);
        c.Bind("bots", &_bots);
        c.Bind("diff", &_diff);
        c.Bind("memory", &_memory);
        c.Bind("client_path", &_clientPath);
        c.Bind("client_ok", &_clientOk);
        c.Bind("locales", &_locales);
        c.Bind("rl_note", &_rlNote);
        c.Bind("events", &_events);
        c.Bind("logs_dir", &_logsDir);
        c.Bind("backup_busy", &_backupBusy);
        c.Bind("message", &_message);
        c.Bind("play_label", &_playLabel);
        c.Bind("play_sub", &_playSub);
        c.Bind("play_enabled", &_playEnabled);
        c.Bind("tab", &_tab);
        c.Bind("closing", &_closing);

        c.Bind("acc_rows", &_accRows);
        c.Bind("acc_note", &_accNote);
        c.Bind("acc_loaded", &_accLoaded);
        c.Bind("acc_login", &_accLogin);
        c.Bind("acc_pass", &_accPass);
        c.Bind("acc_level", &_accLevel);

        c.Bind("cmd_groups", &_cmdGroups);
        c.Bind("cmd_cards", &_cmdCards);
        c.Bind("cmd_chars", &_cmdChars);
        c.Bind("cmd_group", &_cmdGroup);
        c.Bind("cmd_query", &_cmdQuery);
        c.Bind("cmd_target", &_cmdTarget);
        c.Bind("cmd_last", &_cmdLast);

        c.Bind("set_groups", &_setGroups);
        c.Bind("pl_rows", &_plRows);
        c.Bind("pl_status", &_plStatus);
        c.Bind("pl_busy", &_plBusy);
        c.Bind("pl_view", &_plView);
        c.Bind("pl_repos", &_plRepos);
        c.Bind("pl_repo_new", &_plRepoNew);
        c.Bind("pl_has_official", &_plHasOfficial);
        c.Bind("set_fields", &_setFields);
        c.Bind("set_group", &_setGroup);
        c.Bind("set_hint", &_setHint);
        c.Bind("set_status", &_setStatus);
        c.Bind("set_save_label", &_setSaveLabel);
        c.Bind("set_can_save", &_setCanSave);
        c.Bind("set_show_keys", &_setShowKeys);
        c.Bind("set_preset", &_setPreset);

        c.Bind("data_rows", &_dataRows);
        c.Bind("data_sum", &_dataSum);
        c.Bind("storage_title", &_storageTitle);
        c.Bind("storage_detail", &_storageDetail);
        c.Bind("st_current", &_stCurrent);
        c.Bind("st_status", &_stStatus);
        c.Bind("st_changed", &_stChanged);
        c.Bind("st_busy", &_stBusy);

        auto on = [&](char const* name, std::function<void()> fn)
        {
            c.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { fn(); });
        };
        auto onArg = [&](char const* name, std::function<void(Rml::Variant const&)> fn)
        {
            c.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const& args)
            {
                if (!args.empty())
                    fn(args[0]);
            });
        };

        on("toggle_server", [this] { ToggleServer(); });
        on("restart_server", [this] { RestartServer(); });
        on("play", [this] { Play(); });
        on("fix_realmlist", [this] { FixRealmlist({}, false); });
        on("browse_client", [this] { BrowseClient(); });
        on("send_command", [this] { SendConsoleCommand(); });
        onArg("run_command", [this](Rml::Variant const& v) { RunCommand(v.Get<Rml::String>()); });
        onArg("open_tab", [this](Rml::Variant const& v) { OpenTab(v.Get<Rml::String>()); });
        // open_page(page) or open_page(page, section)
        c.BindEventCallback("open_page", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const& args)
        {
            if (!args.empty())
                OpenPage(args[0].Get<Rml::String>(), args.size() > 1 ? args[1].Get<Rml::String>() : std::string());
        });
        onArg("news_action", [this](Rml::Variant const& v)
        {
            std::string const a = v.Get<Rml::String>();
            if (a == "wizard")
                OpenWizard();
            else if (a == "updates")
                OpenPage("plugins", "updates");
            else if (a == "data")
                OpenPage("service", "data");
            else if (a == "backups")
                OpenPage("service", "backups");
        });
        onArg("svc_pick", [this](Rml::Variant const& v) { OpenPage("service", v.Get<Rml::String>()); });
        onArg("pick_locale", [this](Rml::Variant const& v)
        {
            _settings.locale = v.Get<Rml::String>();
            _settings.Save();
            for (SetValue& sv : _settingsModel.Values())
                if (sv.def->key == "Launcher.Locale")
                    sv.orig = sv.cur = _settings.locale;
            BuildSettingsFields();
            RefreshClient();
            Message(Tr("msg.launch_locale", _settings.locale));
        });
        on("open_server_dir", [this] { Platform::OpenInShell(Root()); });
        on("open_logs", [this] { Platform::OpenInShell(ConfPath("LogsDir", "logs")); });
        on("open_data", [this] { Platform::OpenInShell(ConfPath("DataDir", ".")); });
        on("open_backups", [this]
        {
            std::error_code ec;
            fs::create_directories(Root() / "backups", ec);
            Platform::OpenInShell(Root() / "backups");
        });
        on("backup_now", [this] { StartBackup(false); });
        on("announce", [this]
        {
            if (_state != "ready")
                return;
            _cmdGroup = "srv";
            _cmdQuery.clear();
            BuildCommandCards();
            OpenTab("commands");
            Message(Tr("msg.announce_hint"));
        });

        on("acc_create", [this] { CreateAccount(); });
        on("acc_refresh", [this] { if (_server->SendCommand("@@accounts")) _accNote = Tr("acc.loading"); });

        onArg("cmd_group_pick", [this](Rml::Variant const& v)
        {
            _cmdGroup = v.Get<Rml::String>();
            _cmdQuery.clear();
            BuildCommandCards();
        });
        onArg("cmd_run", [this](Rml::Variant const& v) { RunCard(v.Get<int>()); });

        onArg("set_group_pick", [this](Rml::Variant const& v)
        {
            _setGroup = v.Get<Rml::String>();
            BuildSettingsFields();
            if (_setGroup == "about")
                RefreshAbout();
            if (_setGroup == "storage")
                LoadStorageForm();
        });
        onArg("set_preset_pick", [this](Rml::Variant const& v)
        {
            _setPreset = v.Get<int>();
            _settingsModel.ApplyPreset(_setPreset);
            _setGroup = "rates";
            BuildSettingsFields();
        });
        on("set_save", [this] { SaveSettings(); });
        on("data_check", [this] { RefreshData(); });
        on("st_apply", [this] { ApplyStorage(); });
        on("st_revert", [this] { if (!_stBusy) LoadStorageForm(); });
        on("pl_check", [this] { CheckPackageIndex(); });
        on("pl_update_all", [this] { PluginAction("update_all", -1); });
        onArg("pl_toggle", [this](Rml::Variant const& v) { PluginAction("toggle", v.Get<int>()); });
        onArg("pl_remove", [this](Rml::Variant const& v) { PluginAction("remove", v.Get<int>()); });
        onArg("pl_install", [this](Rml::Variant const& v) { PluginAction("install", v.Get<int>()); });
        onArg("pl_view_pick", [this](Rml::Variant const& v)
        {
            _plView = v.Get<Rml::String>();
            RefreshPlugins();
            RefreshRepos();
            _model.DirtyVariable("pl_view");
        });
        on("pl_repo_add", [this] { AddRepo(_plRepoNew); });
        on("pl_repo_official", [this] { AddRepo(LauncherSettings::DefaultPackageIndex); });
        on("pl_repo_browse", [this]
        {
            std::string start = UiPath(_exeDir);
            SDL_ShowOpenFolderDialog([](void* self, char const* const* list, int)
            {
                if (!list || !list[0])
                    return;
                auto* me = static_cast<Launcher*>(self);
                {
                    std::lock_guard<std::mutex> guard(me->_asyncLock);
                    me->_pickedRepo = list[0];
                }
                UiBackend::Wake();
            }, this, UiBackend::GetWindow(), start.c_str(), false);
        });
        onArg("pl_repo_toggle", [this](Rml::Variant const& v) { RepoAction("toggle", v.Get<int>()); });
        onArg("pl_repo_remove", [this](Rml::Variant const& v) { RepoAction("remove", v.Get<int>()); });
        on("open_wizard", [this] { OpenWizard(); });
    }

    // Dev hook for driving the UI without a mouse: with LONELYICE_UI_SCRIPT=<file> set, each line of that file
    // ("click <id>" or "type <id> <text>") is executed and the file is deleted.
    void Launcher::RunUiScript()
    {
        static fs::path const path = Platform::Utf8ToPath(Platform::GetEnv("LONELYICE_UI_SCRIPT").value_or(""));
        if (path.empty() || !fs::exists(path))
            return;

        std::ifstream in{ fs::path(path) };
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);)
            lines.push_back(line);
        in.close();
        std::error_code ec;
        fs::remove(path, ec);

        for (std::string const& l : lines)
        {
            std::istringstream s(l);
            std::string verb, id;
            s >> verb >> id;
            if (verb == "show")
            {
                ShowWindow();
                continue;
            }
            if (verb == "tab")
            {
                OpenTab(id);
                continue;
            }
            if (verb == "tray")
            {
                _tray.Invoke(id);
                continue;
            }
            if (verb == "close")
            {
                _scriptClose = true;
                continue;
            }
            _ctx->Update();
            Rml::Element* el = _doc->GetElementById(id);
            if (!el)
                continue;
            if (verb == "click")
                el->Click();
            else if (verb == "type")
            {
                std::string text;
                std::getline(s, text);
                if (!text.empty() && text[0] == ' ')
                    text.erase(0, 1);
                if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(el))
                {
                    input->SetValue(text);
                    Rml::Dictionary params;
                    params["value"] = text;
                    input->DispatchEvent(Rml::EventId::Change, params);
                }
            }
        }
    }

    void Launcher::Tick()
    {
        RunUiScript();
        _wizard->Tick();
        StorageTick();

        for (std::string const& line : _server->TakeLines())
        {
            char const* cls = nullptr;
            if (line.find("ERROR") != std::string::npos || line.find("rror") != std::string::npos)
                cls = "e";
            else if (line.find("WARN") != std::string::npos || line.find("arning") != std::string::npos)
                cls = "w";
            AppendLog(line, cls);
        }

        ServerState now = _server->GetState();
        if (now != _lastState)
        {
            ServerState prev = _lastState;
            _lastState = now;
            OnStateChanged(prev, now);
        }

        std::vector<AccountInfo> accounts;
        std::vector<CharacterInfo> characters;
        if (_server->TakeAccounts(accounts, characters))
        {
            _accRows.clear();
            uint32_t botAccounts = 0, botChars = 0;
            for (AccountInfo const& a : accounts)
            {
                if (a.bot)
                {
                    ++botAccounts;
                    botChars += a.characters;
                    continue;
                }
                _accRows.push_back({ a.name, a.gmLevel ? Tr("acc.gm", a.gmLevel) : Tr("acc.player"), std::to_string(a.characters), FormatLogin(a.lastLogin), a.gmLevel > 0 });
            }
            if (botAccounts)
                _accRows.push_back({ Tr("acc.bots_row", botAccounts), Tr("acc.bots"), std::to_string(botChars), Tr("acc.service"), false });
            _accLoaded = true;

            std::stable_sort(characters.begin(), characters.end(), [](CharacterInfo const& a, CharacterInfo const& b) { return a.online > b.online; });
            _cmdChars.clear();
            for (CharacterInfo const& ch : characters)
                _cmdChars.push_back({ ch.name, Tr(ch.online ? "cmd.char_online" : "cmd.char", ch.name, ch.level) });
            if (!_cmdChars.empty() && std::none_of(_cmdChars.begin(), _cmdChars.end(), [&](Opt const& o) { return o.id == _cmdTarget; }))
                _cmdTarget = _cmdChars.front().id;
            _model.DirtyVariable("acc_rows");
            _model.DirtyVariable("acc_loaded");
            _model.DirtyVariable("cmd_chars");
            _model.DirtyVariable("cmd_target");
        }

        uint64_t tick = Platform::TickMs();
        if (_accRefreshAt && tick >= _accRefreshAt)
        {
            _accRefreshAt = 0;
            _server->SendCommand("@@accounts");
        }

        if (tick - _lastStatsTick >= 1000)
        {
            _lastStatsTick = tick;
            if (_server->IsRunning())
            {
                ServerStats st = _server->GetStats();
                bool ready = now == ServerState::Ready;
                _uptime = ready ? FormatUptime(st.uptime) : "—";
                _players = ready ? std::to_string(st.players) : "—";
                _bots = ready ? std::to_string(st.chars >= st.players ? st.chars - st.players : 0) : "—";
                _diff = ready ? Tr("time.ms", st.diff) : "—";
                _memory = FormatBytes(_server->GetMemoryBytes());
                if (now == ServerState::Starting || now == ServerState::Loading)
                    _stateSub = Tr("state.loading_for", (tick - _server->GetStartTick()) / 1000);
            }
            else
                _uptime = _players = _bots = _diff = _memory = "—";
            for (char const* v : { "uptime", "players", "bots", "diff", "memory", "state_sub" })
                _model.DirtyVariable(v);

            if (_game && !_game->Running())
            {
                _game.reset();
                AddEvent(Tr("event.game_closed"));
                if (_settings.stopWithGame && _server->IsRunning())
                {
                    AddEvent(Tr("event.stop_with_game"));
                    _server->Stop();
                }
            }
        }

        if (tick - _lastBackupCheck >= 30000)
        {
            _lastBackupCheck = tick;
            CheckScheduledBackup();
        }

        if (_plDone.exchange(false))
        {
            if (_plThread.joinable())
                _plThread.join();
            _plBusy = false;
            _plStatus = _plNewStatus;
            _model.DirtyVariable("pl_status");
            _model.DirtyVariable("pl_busy");
            for (std::string const& line : _plLog)
                AddEvent(line);
            if (!_plError.empty())
                Message(_plError);
            Rml::ReleaseTextures();     // icons of updated plugins keep their paths
            RefreshPlugins();
            RefreshRepos();
            RefreshNews();
            LoadSettingsModel();
        }

        if (_syncDone.exchange(false))
        {
            if (_syncThread.joinable())
                _syncThread.join();
            _syncBusy = false;
            for (std::string const& line : _syncResult.log)
                AddEvent(Tr("event.client_line", line));
            if (_syncResult.ok)
                LaunchGame();
            else
            {
                AddEvent(Tr("event.client_failed", _syncResult.error));
                Message(Tr("event.client_failed", _syncResult.error));
            }
        }

        if (_backupDone.exchange(false))
        {
            if (_backupThread.joinable())
                _backupThread.join();
            _backupBusy = false;
            _model.DirtyVariable("backup_busy");
            if (_backupResult.ok)
                AddEvent(Tr("event.backup_done", FormatBytes(_backupResult.bytes), Platform::PathToUtf8(_backupResult.dir.filename())));
            else
                AddEvent(Tr("event.backup_failed", _backupResult.message));
            RefreshBackups();
            RefreshNews();
        }

        std::string picked, pickedRepo;
        {
            std::lock_guard<std::mutex> guard(_asyncLock);
            picked.swap(_pickedDir);
            pickedRepo.swap(_pickedRepo);
        }
        if (!pickedRepo.empty())
            AddRepo(pickedRepo);
        if (!picked.empty())
        {
            if (GameClient::IsClientDir(Platform::Utf8ToPath(picked)))
            {
                _settings.clientPath = Platform::Utf8ToPath(picked);
                _settings.Save();
                RefreshClient();
                LoadSettingsModel();
                Message(Tr("msg.client_saved"));
            }
            else
                Message(Tr("msg.not_client_dir"));
        }

        if (_page == "main" && _tab == "commands")
            SyncCommandCards();
        else if (_page == "settings")
            SyncSettingsFields();
        if (_uiLang != Lang::Code())
            Relocalize();
        RefreshFooter();
    }

    void Launcher::OnStateChanged(ServerState prev, ServerState now)
    {
        switch (now)
        {
            case ServerState::Starting:
                AddEvent(Tr("event.server_starting"));
                break;
            case ServerState::Ready:
            {
                AddEvent(Tr("event.world_ready", (Platform::TickMs() - _server->GetStartTick()) / 1000));
                std::string realm = _server->GetRealmName();
                if (!_settings.pendingRealmName.empty() && _settings.pendingRealmName != realm)
                {
                    _server->SendCommand("@@realmname " + _settings.pendingRealmName);
                    AddEvent(Tr("event.realm_renamed", _settings.pendingRealmName));
                    realm = _settings.pendingRealmName;
                }
                _settings.pendingRealmName.clear();
                _settings.realmName = realm;
                _settings.Save();
                if (_tab == "accounts" || _tab == "commands")
                    _server->SendCommand("@@accounts");
                if (_pendingPlay)
                {
                    _pendingPlay = false;
                    Play();
                }
                break;
            }
            case ServerState::Stopped:
                if (prev != ServerState::Stopped)
                    AddEvent(Tr("event.server_stopped"));
                if (_pendingRestart)
                {
                    _pendingRestart = false;
                    StartServer();
                }
                break;
            case ServerState::Failed:
                AddEvent(Tr("event.server_failed", _server->GetFailReason()));
                _pendingPlay = _pendingRestart = false;
                break;
            default:
                break;
        }
        if (now != ServerState::Ready)
        {
            _accLoaded = false;
            _accNote = Tr("acc.start_server");
        }
        RefreshServerView();
        RefreshTray();
    }

    void Launcher::RefreshServerView()
    {
        ServerState s = _server->GetState();
        _running = _server->IsRunning();
        switch (s)
        {
            case ServerState::Stopped: _state = "stopped"; _stateTitle = Tr("state.stopped"); _stateSub = Tr("state.stopped.sub"); break;
            case ServerState::Starting: _state = "starting"; _stateTitle = Tr("state.starting"); _stateSub = Tr("state.starting.sub"); break;
            case ServerState::Loading: _state = "loading"; _stateTitle = Tr("state.loading"); _stateSub = Tr("state.loading.sub"); break;
            case ServerState::Ready: _state = "ready"; _stateTitle = Tr("state.ready"); _stateSub = Tr("state.ready.sub"); break;
            case ServerState::Stopping: _state = "stopping"; _stateTitle = Tr("state.stopping"); _stateSub = Tr("state.stopping.sub"); break;
            case ServerState::Failed: _state = "failed"; _stateTitle = Tr("state.failed"); _stateSub = _server->GetFailReason(); break;
        }
        _authOn = s == ServerState::Loading || s == ServerState::Ready;
        _worldOn = s == ServerState::Ready;
        _toggleLabel = Tr(_running ? "server.stop" : "server.start");

        ConfFile conf;
        conf.Load(ServerConfig());
        _authPort = conf.Get("RealmServerPort").value_or("3724");
        _worldPort = conf.Get("WorldServerPort").value_or("8085");
        _soapPort = conf.Get("SOAP.Port").value_or("7878");
        std::string soap = conf.Get("SOAP.Enabled").value_or("0");
        _soapEnabled = soap == "1" || soap == "true";
        _logsDir = Platform::PathToUtf8(ConfPath("LogsDir", "logs"));

        _playLabel = Tr("play.play");
        if (NeedsSetup())
        {
            _playLabel = Tr("play.install");
            _playSub = Tr("play.sub.install");
            _playEnabled = true;
        }
        else if (!_clientOk)
        {
            _playSub = Tr("play.sub.no_client");
            _playEnabled = false;
        }
        else if (s == ServerState::Ready)
        {
            _playSub = Tr("play.sub.ready");
            _playEnabled = true;
        }
        else if (s == ServerState::Stopping)
        {
            _playSub = Tr("play.sub.stopping");
            _playEnabled = false;
        }
        else if (_pendingPlay)
        {
            _playSub = Tr("play.sub.pending");
            _playEnabled = false;
        }
        else
        {
            _playSub = Tr("play.sub.start");
            _playEnabled = true;
        }
        _model.DirtyAllVariables();
    }

    void Launcher::RefreshTray()
    {
        _tray.SetLabel("status", Tr("tray.status", _stateTitle));
        _tray.SetLabel("toggle", Tr(_running ? "tray.stop" : "tray.start"));
        _tray.SetEnabled("restart", _running);
        _tray.SetEnabled("play", _clientOk && _server->GetState() != ServerState::Stopping);
        _tray.SetTooltip("LonelyIce — " + _stateTitle);
    }

    std::string Launcher::LaunchLocale() const
    {
        if (!_settings.locale.empty())
            return _settings.locale;
        return _client.valid ? GameClient::ReadConfigLocale(_client.dir) : std::string();
    }

    void Launcher::RefreshClient()
    {
        fs::path dir = GameClient::Detect(_settings.clientPath, _exeDir);
        _client = GameClient::Inspect(dir);
        _clientOk = _client.valid;
        _locales.clear();

        if (!_client.valid)
        {
            _clientPath = Tr("client.not_found");
            _rlNote = Tr("client.not_found.hint");
        }
        else
        {
            _clientPath = Platform::PathToUtf8(_client.dir);
            if (_settings.clientPath.empty())
            {
                _settings.clientPath = _client.dir;
                _settings.Save();
            }

            std::string launch = LaunchLocale();
            int foreign = 0;
            bool launchOk = true;
            for (ClientLocale const& loc : _client.locales)
            {
                std::string cls = loc.realmlist == RealmHost ? "ok" : loc.realmlist.empty() ? "bad" : "warn";
                if (cls != "ok")
                {
                    ++foreign;
                    if (loc.name == launch)
                        launchOk = false;
                }
                _locales.push_back({ loc.name, cls, loc.name == launch });
            }

            if (_client.version != "3.3.5.12340")
                _rlNote = Tr("client.wrong_version", _client.version.empty() ? Tr("client.unknown_version") : _client.version);
            else if (_client.locales.empty())
                _rlNote = Tr("client.no_locales");
            else if (!launchOk)
                _rlNote = Tr(_settings.writeRealmlist ? "client.rl_foreign_fix" : "client.rl_foreign", launch);
            else if (foreign)
                _rlNote = Tr("client.rl_some_foreign", foreign);
            else
                _rlNote = Tr("client.rl_ok");
        }
        RefreshServerView();
        RefreshTray();
    }

    void Launcher::AppendLog(std::string const& utf8, char const* cls)
    {
        if (!_log)
            return;
        bool atBottom = _log->GetScrollTop() + _log->GetClientHeight() >= _log->GetScrollHeight() - 4;

        Rml::ElementPtr p = _doc->CreateElement("p");
        p->SetInnerRML(EscapeRml(utf8.empty() ? " " : utf8));
        if (cls)
            p->SetClass(cls, true);
        _log->AppendChild(std::move(p));
        _logLines.emplace_back(utf8, cls ? cls : "");
        if (_logLines.size() > std::size_t(MaxLogLines))
            _logLines.pop_front();

        while (_log->GetNumChildren() > MaxLogLines)
            _log->RemoveChild(_log->GetFirstChild());

        if (atBottom)
            _log->SetScrollTop(_log->GetScrollHeight());
    }

    void Launcher::AddEvent(std::string text)
    {
        _events.insert(_events.begin(), { Now(), std::move(text) });
        if (_events.size() > 50)
            _events.pop_back();
        _model.DirtyVariable("events");
    }

    void Launcher::Message(std::string text)
    {
        _message = std::move(text);
        _model.DirtyVariable("message");
    }

    // A tab of the main page.
    void Launcher::OpenTab(std::string const& tab)
    {
        _page = "main";
        _tab = tab;
        if (tab == "accounts" || tab == "commands")
        {
            if (_server->GetState() == ServerState::Ready && _server->SendCommand("@@accounts") && !_accLoaded)
                _accNote = Tr("acc.loading");
        }
        if (tab == "overview")
            RefreshNews();
        _model.DirtyAllVariables();
    }

    // main, settings, plugins or service; section: the settings group, plugin list or service part to show.
    void Launcher::OpenPage(std::string const& page, std::string const& section)
    {
        if (page == "main")
        {
            OpenTab(_tab);
            return;
        }
        _page = page;
        if (page == "settings")
        {
            if (_settingsModel.ChangedCount() == 0)
                LoadSettingsModel();
            if (!section.empty())
            {
                _setGroup = section;
                BuildSettingsFields();
            }
            if (_setGroup == "storage")
                LoadStorageForm();
            RefreshAbout();
        }
        else if (page == "plugins")
        {
            if (!section.empty())
                _plView = section;
            RefreshPlugins();
            RefreshRepos();
            if (!_plIndexLoaded && !_plBusy)
                CheckPackageIndex();
        }
        else if (page == "service")
        {
            if (!section.empty())
                _svcSection = section;
            if (_svcSection == "data")
                RefreshData();
            else
                RefreshBackups();
        }
        _model.DirtyAllVariables();
    }

    void Launcher::RefreshNews()
    {
        _news.clear();
        if (NeedsSetup())
            _news.push_back({ "warn", Tr("news.setup"), Tr("news.setup.text"), "wizard", Tr("news.setup.button") });
        if (_server->GetState() == ServerState::Failed)
            _news.push_back({ "warn", Tr("news.failed"), _server->GetFailReason(), "", "" });
        if (_plUpdates > 0)
            _news.push_back({ "update", Tr("news.updates", _plUpdates), Tr("news.updates.text"), "updates", Tr("news.open") });

        // the newest backup folder
        std::error_code ec;
        fs::path newest;
        fs::file_time_type newestTime{};
        for (fs::directory_iterator it(Root() / "backups", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && it->last_write_time(ec) > newestTime)
            {
                newestTime = it->last_write_time(ec);
                newest = it->path();
            }
        if (!newest.empty())
        {
            DirStats const s = Scan(newest);
            _news.push_back({ "info", Tr("news.backup"), Tr("news.backup.text", Platform::PathToUtf8(newest.filename()), FormatBytes(s.bytes)), "backups", Tr("news.open") });
        }
        else if (!NeedsSetup() && _settings.location == "local")
            _news.push_back({ "warn", Tr("news.no_backup"), Tr("news.no_backup.text"), "backups", Tr("news.open") });
        _model.DirtyVariable("news");
    }

    void Launcher::RefreshFooter()
    {
        std::string status;
        bool busy = true;
        ServerState const s = _server->GetState();
        if (_wizard->IsInstalling())
            status = Tr("foot.installing");
        else if (_syncBusy)
            status = Tr("foot.preparing_client");
        else if (_plBusy)
            status = _plStatus;
        else if (_backupBusy)
            status = Tr("foot.backup");
        else if (s == ServerState::Starting || s == ServerState::Loading || s == ServerState::Stopping)
            status = _stateTitle + " · " + _stateSub;
        else
        {
            busy = false;
            status = s == ServerState::Ready ? Tr("foot.ready", _players, _bots) : _stateTitle + " · " + _stateSub;
        }
        if (status != _footStatus || busy != _footBusy)
        {
            _footStatus = status;
            _footBusy = busy;
            _model.DirtyVariable("foot_status");
            _model.DirtyVariable("foot_busy");
        }
    }

    void Launcher::RefreshBackups()
    {
        std::error_code ec;
        _backupRows.clear();
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(Root() / "backups", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec))
                dirs.push_back(it->path());
        std::sort(dirs.rbegin(), dirs.rend());
        uint64_t total = 0;
        for (fs::path const& d : dirs)
        {
            DirStats const s = Scan(d);
            total += s.bytes;
            _backupRows.push_back({ Platform::PathToUtf8(d.filename()), Tr("backup.files", s.files), "ok", FormatBytes(s.bytes) });
        }
        _backupSum = _settings.backupTime.empty() ? Tr("backup.sum_manual", dirs.size(), FormatBytes(total))
            : Tr("backup.sum", dirs.size(), FormatBytes(total), _settings.backupTime, _settings.backupKeep);

        _logRows.clear();
        std::vector<fs::directory_entry> logs;
        for (fs::directory_iterator it(ConfPath("LogsDir", "logs"), ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                logs.push_back(*it);
        std::sort(logs.begin(), logs.end(), [](auto const& a, auto const& b) { return a.path().filename() < b.path().filename(); });
        for (fs::directory_entry const& e : logs)
            _logRows.push_back({ Platform::PathToUtf8(e.path().filename()), "", "ok", FormatBytes(e.file_size(ec)) });
        for (char const* v : { "backup_rows", "backup_sum", "log_rows" })
            _model.DirtyVariable(v);
    }

    void Launcher::RefreshAbout()
    {
        _aboutRows = {
            { Tr("about.version"), std::string(GitRevision::GetHash()) + " · " + GitRevision::GetDate(), "", "" },
            { Tr("about.core"), Tr("about.core.text"), "", "" },
            { Tr("about.license"), Tr("about.license.text"), "", "" },
            { Tr("about.fonts"), Tr("about.fonts.text"), "", "" },
            { Tr("about.app_dir"), Platform::PathToUtf8(_exeDir), "", "" },
            { Tr("about.data_dir"), Platform::PathToUtf8(Root()), "", "" },
            { Tr("about.config"), Platform::PathToUtf8(ServerConfig()), "", "" },
            { Tr("about.plugins_dir"), Platform::PathToUtf8((_exeDir / "plugins")), "", "" },
        };
        _model.DirtyVariable("about_rows");
    }

    void Launcher::StartServer()
    {
        if (_server->IsRunning())
            return;
        if (_wizard->IsInstalling())
        {
            Message(Tr("msg.installing"));
            return;
        }
        if (NeedsSetup())
        {
            OpenWizard();
            return;
        }
        fs::path config = ServerConfig();
        if (!fs::exists(config))
        {
            Message(Tr("msg.no_config", Platform::PathToUtf8(config)));
            return;
        }
        if (_settings.ReadsClient() && !_client.valid)
        {
            Message(Tr("msg.client_data_needs_client"));
            return;
        }
        std::optional<StorageProviderInfo> const provider = _settings.location != "local" ? FindProvider(_settings.location) : std::nullopt;
        if (_settings.location != "local" && !provider)
        {
            Message(Tr("msg.storage_no_plugin", _settings.location));
            return;
        }
        AppendLog(Tr("log.starting", Platform::PathToUtf8(config)), "me");
        EnvList env = ModuleConfigOverrides(config, Root());
        env.emplace_back("AC_PLUGINS_DIR", Platform::PathToUtf8(_exeDir / "plugins"));
        // the server builds the plugins' client patches while it starts
        if (_client.valid)
        {
            env.emplace_back("LONELYICE_CLIENT", Platform::PathToUtf8(_client.dir));
            env.emplace_back("LONELYICE_LOCALE", ServerLocale());
        }
        // game data: read from the client, or unpacked with the DBC files in the world database
        if (_settings.ReadsClient())
            env.emplace_back("LONELYICE_DATA", "client");
        else
            env.emplace_back(EnvName("DBC.FromDatabase"), "1");
        if (provider)
        {
            EnvList const remote = RemoteEnv(CurrentStorage());
            env.insert(env.end(), remote.begin(), remote.end());
        }
        if (!_server->Start(Platform::PathToUtf8(_exe), Platform::PathToUtf8(config), Platform::PathToUtf8(Root()), env))
            Message(Tr("msg.start_failed", _server->GetFailReason()));
        RefreshServerView();
        RefreshTray();
    }

    void Launcher::ToggleServer()
    {
        if (_server->IsRunning())
        {
            _pendingPlay = _pendingRestart = false;
            _server->Stop();
        }
        else
            StartServer();
        RefreshServerView();
        RefreshTray();
    }

    void Launcher::RestartServer()
    {
        if (!_server->IsRunning())
            return;
        _pendingRestart = true;
        _server->Stop();
        RefreshServerView();
    }

    void Launcher::Play()
    {
        if (NeedsSetup())
        {
            OpenWizard();
            return;
        }
        if (!_client.valid)
        {
            ShowWindow();
            Message(Tr("msg.need_client"));
            return;
        }

        if (_server->GetState() != ServerState::Ready)
        {
            _pendingPlay = true;
            if (!_server->IsRunning())
                StartServer();
            RefreshServerView();
            return;
        }

        if (GameClient::IsRunning(_client.dir))
        {
            Message(Tr("msg.game_running"));
            return;
        }
        if (_syncBusy)
            return;

        // Plugin addons first (the server built the client patches when it started), off the UI thread.
        _syncBusy = true;
        Message(Tr("msg.preparing_client"));
        _syncThread = std::thread([this, client = _client.dir, plugins = ReadPlugins(_exeDir / "plugins")]
        {
            _syncResult = ClientPatch::SyncAddons(client, plugins);
            _syncDone = true;
        });
    }

    void Launcher::LaunchGame()
    {
        std::string launch = LaunchLocale();
        if (_settings.writeRealmlist)
            FixRealmlist(launch.empty() ? std::vector<std::string>{} : std::vector<std::string>{ launch }, true);
        if (!_settings.locale.empty())
            GameClient::SetConfigLocale(_client.dir, _settings.locale);
        if (_settings.clearWdb)
            GameClient::ClearWdb(_client.dir);

        std::string error;
        std::unique_ptr<Platform::Child> process;
        if (GameClient::Launch(_client.dir, _settings.runner, error, process))
        {
            _game = std::move(process);
            AddEvent(launch.empty() ? Tr("event.game_started") : Tr("event.game_started_locale", launch));
            Message("");
        }
        else
            Message(error);
    }

    void Launcher::FixRealmlist(std::vector<std::string> const& locales, bool quiet)
    {
        if (!_client.valid)
            return;

        std::vector<std::string> todo;
        for (ClientLocale const& loc : _client.locales)
            if (loc.realmlist != RealmHost && (locales.empty() || std::find(locales.begin(), locales.end(), loc.name) != locales.end()))
                todo.push_back(loc.name);
        if (todo.empty())
        {
            if (!quiet)
                Message(Tr("msg.realmlist_ok"));
            return;
        }

        std::string error;
        if (!GameClient::WriteRealmlist(_client, RealmHost, todo, error))
        {
            Message(error);
            return;
        }
        std::string list;
        for (std::string const& l : todo)
            list += (list.empty() ? "" : ", ") + l;
        AddEvent("realmlist 127.0.0.1: " + list);
        RefreshClient();
    }

    void Launcher::BrowseClient()
    {
        std::string start = _client.valid ? Platform::PathToUtf8(_client.dir) : Platform::PathToUtf8(_exeDir);
        SDL_ShowOpenFolderDialog([](void* self, char const* const* list, int)
        {
            if (!list || !list[0])
                return;
            auto* me = static_cast<Launcher*>(self);
            {
                std::lock_guard<std::mutex> guard(me->_asyncLock);
                me->_pickedDir = list[0];
            }
            UiBackend::Wake();
        }, this, UiBackend::GetWindow(), start.c_str(), false);
    }

    void Launcher::SendConsoleCommand()
    {
        auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(_doc->GetElementById("cmd"));
        if (!input)
            return;
        Rml::String cmd = input->GetValue();
        if (cmd.empty())
            return;
        RunCommand(cmd);
        input->SetValue("");
    }

    void Launcher::RunCommand(std::string const& cmd, bool echo)
    {
        if (_server->GetState() != ServerState::Ready)
        {
            Message(Tr("msg.cmd_not_ready"));
            return;
        }
        if (echo)
            AppendLog("AC> " + cmd, "me");
        if (!_server->SendCommand(cmd))
            Message(Tr("msg.cmd_not_sent"));
    }

    // ---- accounts

    void Launcher::CreateAccount()
    {
        if (_server->GetState() != ServerState::Ready)
            return;
        std::string login = _accLogin, pass = _accPass;
        if (login.empty() || pass.empty() || login.find(' ') != std::string::npos || pass.find(' ') != std::string::npos)
        {
            Message(Tr("msg.acc_invalid"));
            return;
        }
        RunCommand("account create " + login + " " + pass, false);
        AppendLog("AC> account create " + login + " ********", "me");
        if (_accLevel != "0")
            RunCommand("account set gmlevel " + login + " " + _accLevel + " -1");
        AddEvent(_accLevel != "0" ? Tr("event.acc_created_gm", login, _accLevel) : Tr("event.acc_created", login));
        _accPass.clear();
        _accLogin.clear();
        _model.DirtyVariable("acc_pass");
        _model.DirtyVariable("acc_login");
        _accRefreshAt = Platform::TickMs() + 1500;
    }

    // ---- commands

    void Launcher::BuildCommandCards()
    {
        auto const& catalog = CommandCatalog();
        _cmdGroups.clear();
        for (CmdGroup const& g : catalog)
            _cmdGroups.push_back({ g.id, Tr(g.name) });

        std::string q = _cmdQuery;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);

        _cmdCards.clear();
        for (int gi = 0; gi < int(catalog.size()); ++gi)
        {
            CmdGroup const& g = catalog[gi];
            if (q.empty() && g.id != _cmdGroup)
                continue;
            for (int ci = 0; ci < int(g.cmds.size()); ++ci)
            {
                CmdDef const& d = g.cmds[ci];
                if (!q.empty())
                {
                    // ASCII-only lowering: Cyrillic search matches the case typed
                    std::string hay = Tr(d.title) + " " + Tr(d.desc) + " " + d.tmpl;
                    std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
                    if (hay.find(q) == std::string::npos)
                        continue;
                }
                CmdCardView card;
                card.title = Tr(d.title);
                card.desc = d.desc.empty() ? std::string() : Tr(d.desc);
                card.group = q.empty() ? "" : Tr(g.name);
                card.danger = d.danger;
                card.groupIdx = gi;
                card.cmdIdx = ci;
                for (CmdArgDef const& a : d.args)
                {
                    CmdArgView av{ a.name, std::string(1, a.type), Tr(a.def), {} };
                    for (auto const& [id, label] : a.options)
                        av.opts.push_back({ id, Tr(label) });
                    card.args.push_back(av);
                }
                _cmdCards.push_back(std::move(card));
            }
        }
        _cmdQueryShown = _cmdQuery;
        SyncCommandCards();
        _model.DirtyVariable("cmd_groups");
        _model.DirtyVariable("cmd_cards");
        _model.DirtyVariable("cmd_group");
        _model.DirtyVariable("cmd_query");
    }

    void Launcher::SyncCommandCards()
    {
        if (_cmdQuery != _cmdQueryShown)
        {
            BuildCommandCards();
            return;
        }
        bool dirty = false;
        for (CmdCardView& card : _cmdCards)
        {
            CmdDef const& d = CommandCatalog()[card.groupIdx].cmds[card.cmdIdx];
            std::vector<std::string> values;
            for (CmdArgView const& a : card.args)
                values.push_back(a.value);
            std::string preview = BuildCommand(d, values, _cmdTarget);
            std::string button = Tr(card.armed ? "cmd.sure" : d.danger ? "cmd.run_danger" : "cmd.run");
            if (preview != card.preview || button != card.button)
            {
                card.preview = preview;
                card.button = button;
                dirty = true;
            }
        }
        if (dirty)
            _model.DirtyVariable("cmd_cards");
    }

    void Launcher::RunCard(int index)
    {
        if (index < 0 || index >= int(_cmdCards.size()) || _server->GetState() != ServerState::Ready)
            return;
        CmdCardView& card = _cmdCards[index];
        CmdDef const& d = CommandCatalog()[card.groupIdx].cmds[card.cmdIdx];
        if (d.tmpl.find("{p}") != std::string::npos && _cmdTarget.empty())
        {
            Message(Tr("msg.pick_character"));
            return;
        }
        if (d.danger && !card.armed)
        {
            card.armed = true;
            SyncCommandCards();
            return;
        }
        card.armed = false;
        std::vector<std::string> values;
        for (CmdArgView const& a : card.args)
            values.push_back(a.value);
        std::string cmd = BuildCommand(d, values, _cmdTarget);
        RunCommand(cmd);
        _cmdLast = Tr("cmd.sent", cmd);
        _model.DirtyVariable("cmd_last");
        SyncCommandCards();
    }

    // ---- settings

    void Launcher::LoadSettingsModel()
    {
        std::vector<std::string> locales;
        for (ClientLocale const& l : _client.locales)
            locales.push_back(l.name);
        _settingsModel.Load(ServerConfig(), ReadPlugins(_exeDir / "plugins"), _settings, locales);
        for (std::string const& e : _settingsModel.Errors())
            AddEvent(Tr("event.plugin_settings_bad", e));
        if (_setGroup != "about" && _setGroup != "storage"
            && std::none_of(_settingsModel.Groups().begin(), _settingsModel.Groups().end(), [&](SetGroup const& g) { return g.id == _setGroup; }))
            _setGroup = "rates";
        _setPreset = 0;
        BuildSettingsFields();
    }

    void Launcher::BuildSettingsFields()
    {
        static std::map<std::string, std::string> const applyText = { { "now", "apply.now" }, { "rel", "apply.reload" }, { "rst", "apply.restart" } };

        // Side list: the storage, world groups, plugin groups, then the launcher and "about"; a heading on the first of each.
        _setGroups.clear();
        _setGroups.push_back({ "storage", Tr("set.group.storage"), 0, Tr("set.section.data") });
        if (_setGroup == "storage")
            _setHint = Tr("storage.hint");
        for (int pass = 0; pass < 3; ++pass)
        {
            static char const* const sections[] = { "set.section.world", "set.section.plugins", "set.section.launcher" };
            bool first = true;
            for (SetGroup const& g : _settingsModel.Groups())
            {
                int const kind = g.id.rfind("plugin:", 0) == 0 ? 1 : g.id == "launch" ? 2 : 0;
                auto const& vals = _settingsModel.Values();
                if (kind != pass || std::none_of(vals.begin(), vals.end(), [&](SetValue const& v) { return v.def->group == g.id; }))
                    continue;
                _setGroups.push_back({ g.id, Tr(g.name), _settingsModel.ChangedCount(g.id), first ? Tr(sections[pass]) : "" });
                first = false;
                if (g.id == _setGroup)
                    _setHint = Tr(g.hint);
            }
            if (pass == 2)
                _setGroups.push_back({ "about", Tr("about.title"), 0, first ? Tr(sections[pass]) : "" });
        }
        if (_setGroup == "about")
            _setHint = Tr("about.hint");

        _setFields.clear();
        auto& values = _settingsModel.Values();
        for (int i = 0; i < int(values.size()); ++i)
        {
            SetValue const& v = values[i];
            if (v.def->group != _setGroup)
                continue;
            FieldView f;
            f.label = Tr(v.def->label);
            f.key = v.def->key;
            f.type = std::string(1, v.def->type);
            f.value = v.cur;
            f.on = v.cur == "1";
            f.apply = v.def->apply;
            f.apply_text = Tr(applyText.at(v.def->apply));
            f.hint = v.def->hint.empty() ? std::string() : Tr(v.def->hint);
            f.changed = _settingsModel.Changed(v);
            f.index = i;
            for (auto const& [id, label] : v.def->options)
                f.opts.push_back({ id, Tr(label) });
            _setFields.push_back(std::move(f));
        }
        RefreshSettingsStatus();
        for (char const* v : { "set_groups", "set_fields", "set_group", "set_hint", "set_preset" })
            _model.DirtyVariable(v);
    }

    void Launcher::SyncSettingsFields()
    {
        auto& values = _settingsModel.Values();
        bool dirty = false;
        for (FieldView& f : _setFields)
        {
            SetValue& v = values[f.index];
            std::string cur = f.type == "b" ? (f.on ? "1" : "0") : f.value;
            if (cur != v.cur)
                v.cur = cur;
            bool changed = _settingsModel.Changed(v);
            if (changed != f.changed)
            {
                f.changed = changed;
                dirty = true;
            }
        }
        if (dirty)
        {
            for (GroupView& g : _setGroups)
                g.changed = _settingsModel.ChangedCount(g.id);
            RefreshSettingsStatus();
            _model.DirtyVariable("set_fields");
            _model.DirtyVariable("set_groups");
        }
    }

    void Launcher::RefreshSettingsStatus()
    {
        int n = _settingsModel.ChangedCount();
        bool restart = _settingsModel.NeedsRestart() && _server->IsRunning();
        _setStatus = n ? Tr(restart ? "set.changed_restart" : "set.changed", n) : Tr("set.unchanged");
        _setSaveLabel = Tr(restart ? "set.save_restart" : "set.save");
        _setCanSave = n > 0;
        for (char const* v : { "set_status", "set_save_label", "set_can_save" })
            _model.DirtyVariable(v);
    }

    void Launcher::SaveSettings()
    {
        SyncSettingsFields();
        if (!_settingsModel.ChangedCount())
            return;
        bool restartNeeded = _settingsModel.NeedsRestart();
        SaveResult r = _settingsModel.Save(_settings);
        if (!r.error.empty())
        {
            Message(r.error);
            return;
        }

        std::string what = Tr("set.saved");
        if (_server->GetState() == ServerState::Ready)
        {
            if (r.reload && !restartNeeded)
            {
                RunCommand("reload config");
                what = Tr("set.saved_reloaded");
            }
            if (restartNeeded)
            {
                RestartServer();
                what = Tr("set.saved_restarting");
            }
        }
        AddEvent(what);
        Message(what);
        UiBackend::SetUiScale(_ctx, _settings.uiScale / 100.f);
        RefreshClient();
        BuildSettingsFields();
    }

    // ---- data

    // ---- plugins

    void Launcher::RefreshPlugins()
    {
        if (_plBusy)
            return;     // the plugins thread is changing the index or the folder
        _plRows.clear();
        std::error_code ec;
        std::map<std::string, Packages::Package const*> newest;
        for (Packages::Package const& p : _packages->Available())
            if (!newest.count(p.id) || Packages::Manager::CompareVersions(p.version, newest[p.id]->version) > 0)
                newest[p.id] = &p;

        // The side list picks what is shown: installed plugins, those with an update, or the catalog.
        std::set<std::string> installed;
        _plUpdates = 0;
        for (Packages::Local const& l : _packages->Installed())
        {
            PluginView v;
            v.id = l.manifest.id;
            v.name = l.manifest.name;
            v.version = l.manifest.version;
            v.desc = l.manifest.description;
            v.installed = true;
            v.enabled = l.enabled;
            if (auto it = newest.find(l.manifest.id); it != newest.end() && Packages::Manager::CompareVersions(it->second->version, l.manifest.version) > 0)
            {
                v.update = it->second->version;
                ++_plUpdates;
            }
            std::string deps;
            for (auto const& [dep, range] : l.manifest.depends)
                deps += (deps.empty() ? "" : ", ") + dep + " " + range;
            v.note = l.manifest.id;
            if (!deps.empty())
                v.note += " · " + Tr("pl.note.needs", deps);
            if (!l.enabled)
                v.note += " · " + Tr("pl.note.disabled");
            if (fs::exists(l.manifest.dir / "icon.png", ec))
                v.icon = UiPath(l.manifest.dir / "icon.png");
            v.letter = Initial(v.name);
            // the gear: a storage plugin is set up on the storage page, others in their own settings group
            std::string const group = "plugin:" + l.manifest.id;
            auto const& values = _settingsModel.Values();
            if (l.manifest.storage && l.enabled)
                v.settings = "storage";
            else if (std::any_of(values.begin(), values.end(), [&](SetValue const& sv) { return sv.def->group == group; }))
                v.settings = group;
            installed.insert(l.manifest.id);
            if (_plView == "installed" || (_plView == "updates" && !v.update.empty()))
                _plRows.push_back(std::move(v));
        }
        for (auto const& [id, p] : newest)
        {
            if (installed.count(id) || _plView != "catalog")
                continue;
            PluginView v;
            v.id = id;
            v.name = p->name;
            v.version = p->version;
            v.desc = p->description;
            v.note = id;
            if (_packages->Sources().size() > 1)
                for (Packages::Source const& s : _packages->Sources())
                    if (s.location == p->source)
                        v.note += " · " + RepoTitle(s.location, s.name);
            if (fs::exists(_packages->IconFile(*p), ec))
                v.icon = UiPath(_packages->IconFile(*p));
            v.letter = Initial(v.name);
            _plRows.push_back(std::move(v));
        }
        _model.DirtyVariable("pl_rows");
        _model.DirtyVariable("pl_updates");
    }

    bool Launcher::PluginsLocked()
    {
        if (_plBusy)
            return true;
        if (_server->IsRunning())
        {
            Message(Tr("msg.plugins_locked"));
            return true;
        }
        return false;
    }

    // The package list's status line from the last LoadIndex.
    std::string Launcher::IndexStatus() const
    {
        auto const& sources = _packages->Sources();
        if (sources.empty())
            return Tr(_settings.packageIndex.empty() ? "pl.status.no_repos" : "pl.status.not_loaded");
        std::size_t const failed = std::count_if(sources.begin(), sources.end(), [](Packages::Source const& s) { return !s.ok; });
        if (failed == sources.size())
            return Tr("pl.status.repos_down");
        return failed ? Tr("pl.status.loaded_some", _packages->Available().size(), failed) : Tr("pl.status.loaded", _packages->Available().size());
    }

    void Launcher::CheckPackageIndex()
    {
        if (_plBusy)
            return;
        _plBusy = true;
        _plStatus = Tr("pl.status.loading");
        _model.DirtyVariable("pl_busy");
        _model.DirtyVariable("pl_status");
        _plThread = std::thread([this, index = _settings.packageIndex]
        {
            std::string error;
            _plLog.clear();
            _plError.clear();
            bool const ok = _packages->LoadIndex(index, error);
            for (Packages::Source const& s : _packages->Sources())
                if (!s.ok)
                    _plLog.push_back(Tr("event.repo_failed", s.location, s.error));
            if (ok && !_packages->Sources().empty())
            {
                _plIndexLoaded = true;
                _packages->FetchIcons();
            }
            _plNewStatus = IndexStatus();
            _plDone = true;
        });
    }

    void Launcher::InstallPackages(std::map<std::string, std::string> const& requests, bool update)
    {
        Packages::Plan plan = requests.empty() ? _packages->ResolveUpdates() : _packages->Resolve(requests, update);
        if (!plan.error.empty())
        {
            Message(plan.error);
            return;
        }
        if (plan.steps.empty())
        {
            Message(Tr("msg.all_installed"));
            return;
        }
        _plBusy = true;
        _plStatus = Tr("pl.status.installing");
        _model.DirtyVariable("pl_busy");
        _model.DirtyVariable("pl_status");
        _plThread = std::thread([this, plan]
        {
            _plLog.clear();
            _plError.clear();
            std::string error;
            bool const ok = _packages->Install(plan, error, [this](std::string const& line) { _plLog.push_back(Tr("event.plugins_line", line)); });
            _plError = ok ? "" : error;
            if (!ok)
                _plLog.push_back(Tr("event.plugins_line", error));
            _plNewStatus = Tr(ok ? "pl.status.done" : "pl.status.failed");
            _plDone = true;
        });
    }

    void Launcher::PluginAction(std::string const& action, int index)
    {
        if (PluginsLocked())
            return;
        if (action == "update_all")
        {
            if (!_plIndexLoaded)
            {
                Message(Tr("msg.load_index_first"));
                return;
            }
            InstallPackages({}, true);
            return;
        }
        if (index < 0 || index >= int(_plRows.size()))
            return;
        PluginView const v = _plRows[index];
        std::string const id = v.id;
        std::string error;

        if (action == "install")
        {
            InstallPackages({ { id, "*" } }, v.installed);
            return;
        }

        // Disabling or removing a plugin others need would break them.
        if ((action == "remove" || (action == "toggle" && v.enabled)))
        {
            std::vector<std::string> const deps = _packages->Dependents(id);
            if (!deps.empty())
            {
                std::string list;
                for (std::string const& d : deps)
                    list += (list.empty() ? "" : ", ") + d;
                Message(Tr("msg.plugin_needed_by", v.name, list));
                return;
            }
        }

        bool ok = action == "remove" ? _packages->Remove(id, error) : _packages->SetEnabled(id, !v.enabled, error);
        if (!ok)
        {
            Message(error);
            return;
        }
        char const* const what = action == "remove" ? "event.plugin_removed" : v.enabled ? "event.plugin_disabled" : "event.plugin_enabled";
        AddEvent(Tr(what, v.name));
        Message(Tr(what, v.name) + " " + Tr("pl.applies_on_start"));
        RefreshPlugins();
        LoadSettingsModel();
    }

    void Launcher::RefreshRepos()
    {
        if (_plBusy)
            return;
        _plRepos.clear();
        auto add = [&](std::string const& location, bool enabled)
        {
            RepoView r;
            r.location = location;
            r.enabled = enabled;
            r.kind = Tr(IsRemote(location) ? "repo.remote" : "repo.local");
            std::string name;
            r.note = Tr(enabled ? "repo.not_loaded" : "repo.disabled");
            if (enabled)
                for (Packages::Source const& s : _packages->Sources())
                    if (s.location == location)
                    {
                        name = s.name;
                        r.ok = s.ok;
                        r.failed = !s.ok;
                        r.note = s.ok ? Tr("repo.packages", s.packages) : s.error;
                    }
            r.title = RepoTitle(location, name);
            _plRepos.push_back(std::move(r));
        };
        std::vector<std::string> const on = Packages::Manager::SplitSources(_settings.packageIndex);
        std::vector<std::string> const off = Packages::Manager::SplitSources(_settings.packageIndexOff);
        for (std::string const& l : on)
            add(l, true);
        for (std::string const& l : off)
            add(l, false);
        _plHasOfficial = std::find(on.begin(), on.end(), LauncherSettings::DefaultPackageIndex) != on.end()
            || std::find(off.begin(), off.end(), LauncherSettings::DefaultPackageIndex) != off.end();
        _model.DirtyVariable("pl_repos");
        _model.DirtyVariable("pl_has_official");
    }

    void Launcher::SaveRepos(std::vector<std::string> const& on, std::vector<std::string> const& off)
    {
        auto join = [](std::vector<std::string> const& list)
        {
            std::string s;
            for (std::string const& l : list)
                s += (s.empty() ? "" : ";") + l;
            return s;
        };
        _settings.packageIndex = join(on);
        _settings.packageIndexOff = join(off);
        _settings.Save();
        _plIndexLoaded = false;
        RefreshRepos();
        CheckPackageIndex();
    }

    void Launcher::AddRepo(std::string location)
    {
        location.erase(0, location.find_first_not_of(" \t\""));
        location.erase(location.find_last_not_of(" \t\"") + 1);
        if (location.empty() || _plBusy)
            return;
        if (location.find(';') != std::string::npos)
        {
            Message(Tr("msg.repo_semicolon"));
            return;
        }
        std::error_code ec;
        if (!IsRemote(location) && location.rfind("file://", 0) != 0 && !fs::exists(fs::u8path(location), ec))
        {
            Message(Tr("msg.repo_missing", location));
            return;
        }
        if (!IsRemote(location) && fs::is_directory(fs::u8path(location), ec) && !fs::exists(fs::u8path(location) / "index.json", ec))
        {
            Message(Tr("msg.repo_no_index"));
            return;
        }
        std::vector<std::string> on = Packages::Manager::SplitSources(_settings.packageIndex);
        std::vector<std::string> off = Packages::Manager::SplitSources(_settings.packageIndexOff);
        if (std::find(on.begin(), on.end(), location) != on.end())
        {
            Message(Tr("msg.repo_exists"));
            return;
        }
        off.erase(std::remove(off.begin(), off.end(), location), off.end());
        on.push_back(location);
        _plRepoNew.clear();
        _model.DirtyVariable("pl_repo_new");
        AddEvent(Tr("event.repo_added", location));
        SaveRepos(on, off);
    }

    void Launcher::RepoAction(std::string const& action, int index)
    {
        if (_plBusy || index < 0 || index >= int(_plRepos.size()))
            return;
        RepoView const r = _plRepos[index];
        std::string const loc = r.location;
        std::vector<std::string> on = Packages::Manager::SplitSources(_settings.packageIndex);
        std::vector<std::string> off = Packages::Manager::SplitSources(_settings.packageIndexOff);
        on.erase(std::remove(on.begin(), on.end(), loc), on.end());
        off.erase(std::remove(off.begin(), off.end(), loc), off.end());
        if (action == "toggle")
            (r.enabled ? off : on).push_back(loc);
        AddEvent(Tr(action == "remove" ? "event.repo_removed" : r.enabled ? "event.repo_disabled" : "event.repo_enabled", loc));
        SaveRepos(on, off);
    }

    void Launcher::RefreshData()
    {
        _dataRows.clear();
        uint64_t total = 0;
        std::vector<std::string> missing;

        std::optional<StorageProviderInfo> const provider = _settings.location != "local" ? FindProvider(_settings.location) : std::nullopt;
        bool const onServer = _settings.location != "local";
        std::string const port = _settings.remote.port.empty() && provider ? provider->provider.port : _settings.remote.port;
        std::string const serverAt = _settings.remote.host + ":" + port;
        std::string const serverName = provider ? provider->provider.name : _settings.location;

        // data.db.<name> / data.db.<name>.detail for the known databases
        for (DatabaseFile const& db : FindDatabases(ServerConfig(), Root()))
        {
            std::error_code ec;
            std::string const key = "data.db." + db.name;
            bool const known = Lang::Has(key);
            std::string title = known ? Tr(key) : db.name;
            std::string detail = known ? Tr(key + ".detail") : std::string();
            if (onServer)
            {
                _dataRows.push_back({ title, Tr("data.on_server", _settings.remote.prefix + db.name, serverAt), "ok", "" });
                continue;
            }
            if (db.path.empty())
            {
                _dataRows.push_back({ title, Tr("data.external_db"), "ok", "" });
                continue;
            }
            bool ok = fs::exists(db.path, ec);
            uint64_t size = ok ? fs::file_size(db.path, ec) : 0;
            total += size;
            if (!ok)
                missing.push_back(title);
            _dataRows.push_back({ title, detail, ok ? "ok" : "bad", ok ? FormatBytes(size) : Tr("data.none") });
        }

        fs::path data = ConfPath("DataDir", ".");
        bool const fromClient = _settings.ReadsClient();
        if (fromClient)
        {
            // DBC and cameras stay in the client; terrain tiles are built into data/maps as grids load.
            std::string const title = Tr("data.dbc");
            _dataRows.push_back({ title, Tr("data.from_client"), _client.valid ? "ok" : "bad", "" });
            if (!_client.valid)
                missing.push_back(title);
        }
        struct Part { char const* dir; char const* key; bool required; };
        for (Part const& p : { Part{ "maps", "data.maps", !fromClient }, Part{ "Cameras", "data.cameras", false },
                 Part{ "vmaps", "data.vmaps", false }, Part{ "mmaps", "data.mmaps", false } })
        {
            if (fromClient && std::string_view(p.dir) == "Cameras")
                continue;
            DirStats s = Scan(data / p.dir);
            total += s.bytes;
            bool ok = s.files > 0;
            std::string const title = Tr(p.key);
            std::string detail = Tr(std::string(p.key) + ".detail");
            if (fromClient && std::string_view(p.dir) == "maps")
            {
                // Built on demand: none yet is fine.
                std::error_code ec;
                std::size_t tiles = 0;
                for (fs::directory_iterator it(data / p.dir, ec), end; !ec && it != end; it.increment(ec))
                    tiles += it->path().extension() == ".map";
                _dataRows.push_back({ title, Tr("data.maps.built", tiles), "ok", FormatBytes(s.bytes) });
                continue;
            }
            if (!ok)
                missing.push_back(title);
            _dataRows.push_back({ title, ok ? Tr("data.detail_files", detail, s.files) : detail,
                ok ? "ok" : (p.required ? "bad" : "warn"), ok ? FormatBytes(s.bytes) : Tr("data.none") });
        }

        _storageTitle = onServer ? Tr("data.storage.server", serverName, serverAt) : Tr("data.storage.local");
        _storageDetail = Tr(fromClient ? "data.storage.client.detail" : "data.storage.cache.detail");
        _model.DirtyVariable("storage_title");
        _model.DirtyVariable("storage_detail");

        if (missing.empty())
            _dataSum = Tr("data.all_present", FormatBytes(total));
        else
        {
            std::string list;
            for (std::size_t i = 0; i < missing.size(); ++i)
                list += (i ? ", " : "") + missing[i];
            _dataSum = Tr("data.missing", list);
        }
        _model.DirtyVariable("data_rows");
        _model.DirtyVariable("data_sum");
    }

    // ---- backups

    void Launcher::StartBackup(bool scheduled)
    {
        if (_backupBusy)
            return;
        std::vector<DatabaseFile> dbs = FindDatabases(ServerConfig(), Root());
        // On a database server the files of the config are not the server's databases.
        if (_settings.location != "local" || std::none_of(dbs.begin(), dbs.end(), [](DatabaseFile const& d) { return !d.path.empty(); }))
        {
            Message(Tr("msg.backup_files_only"));
            return;
        }
        _backupBusy = true;
        _model.DirtyVariable("backup_busy");
        if (!scheduled)
            Message(Tr("msg.backup_running"));
        if (_backupThread.joinable())
            _backupThread.join();
        _backupThread = std::thread([this, dbs, root = Root() / "backups", keep = _settings.backupKeep]
        {
            _backupResult = BackupDatabases(dbs, root, keep);
            _backupDone = true;
            UiBackend::Wake();
        });
    }

    void Launcher::CheckScheduledBackup()
    {
        if (_settings.backupTime.size() != 5 || _backupBusy || _settings.location != "local")
            return;
        std::string today = Now("%Y-%m-%d");
        if (_settings.lastBackupDay == today || Now("%H:%M") < _settings.backupTime)
            return;
        _settings.lastBackupDay = today;
        _settings.Save();
        StartBackup(true);
    }
}

int LauncherMain(int argc, char** argv)
{
    Launcher launcher;
    return launcher.Run(argc, argv);
}
