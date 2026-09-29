#include "Assets.h"
#include "Backup.h"
#include "CommandCatalog.h"
#include "ConfFile.h"
#include "GameClient.h"
#include "LauncherSettings.h"
#include "ServerProcess.h"
#include "SettingsModel.h"
#include "ClientPatch.h"
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
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <thread>
#include <Windows.h>
#include <shellapi.h>

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
    struct GroupView { Rml::String id, name; int changed = 0; };
    struct FieldView
    {
        Rml::String label, key, type, value, apply, apply_text, hint;
        bool on = false, changed = false;
        std::vector<Opt> opts;
        int index = 0;
    };
    struct DataRow { Rml::String title, detail, status, size; };
    struct PluginView
    {
        Rml::String id, name, version, desc, note, update;   // update: newer version in the index
        bool installed = false, enabled = false;
    };

    std::string Now(char const* fmt = "%H:%M")
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), fmt, &tm);
        return buf;
    }

    std::string FormatUptime(uint64_t s)
    {
        if (s < 60)
            return std::to_string(s) + " с";
        uint64_t m = s / 60, h = m / 60, d = h / 24;
        if (d)
            return std::to_string(d) + " д " + std::to_string(h % 24) + " ч";
        if (h)
            return std::to_string(h) + " ч " + std::to_string(m % 60) + " мин";
        return std::to_string(m) + " мин";
    }

    std::string FormatBytes(uint64_t b)
    {
        char buf[32];
        if (b >= (1ull << 30))
            snprintf(buf, sizeof(buf), "%.1f ГБ", double(b) / double(1ull << 30));
        else if (b >= (1ull << 20))
            snprintf(buf, sizeof(buf), "%llu МБ", (unsigned long long)(b >> 20));
        else
            snprintf(buf, sizeof(buf), "%llu КБ", (unsigned long long)((b + 1023) >> 10));
        std::string s = buf;
        std::replace(s.begin(), s.end(), '.', ',');
        return s;
    }

    // "2026-09-28 19:12:40" -> "сегодня, 19:12" or "28.09.2026"
    std::string FormatLogin(std::string const& ts)
    {
        if (ts.size() < 16 || ts.rfind("0000", 0) == 0)
            return "—";
        if (ts.substr(0, 10) == Now("%Y-%m-%d"))
            return "сегодня, " + ts.substr(11, 5);
        return ts.substr(8, 2) + "." + ts.substr(5, 2) + "." + ts.substr(0, 4);
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
        int Run();

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
        void InstallPackages(std::map<std::string, std::string> const& requests, bool update);
        void PluginAction(std::string const& action, int index);
        bool PluginsLocked();
        void StartBackup(bool scheduled);
        void CheckScheduledBackup();

        bool NeedsSetup() const;
        void OpenWizard();
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
        bool _running = false, _authOn = false, _worldOn = false, _soapEnabled = false, _clientOk = false, _playEnabled = true, _closing = false, _backupBusy = false;
        std::vector<LocaleChip> _locales;
        std::vector<EventRow> _events;
        // accounts
        std::vector<AccRow> _accRows;
        Rml::String _accNote = "Запустите сервер, чтобы увидеть аккаунты.", _accLogin, _accPass, _accLevel = "0";
        bool _accLoaded = false;
        // commands
        std::vector<GroupView> _cmdGroups;
        std::vector<CmdCardView> _cmdCards;
        std::vector<Opt> _cmdChars;
        Rml::String _cmdGroup = "srv", _cmdQuery, _cmdQueryShown, _cmdTarget, _cmdLast = "Ответ сервера появится во вкладке «Консоль».";
        // settings
        std::vector<GroupView> _setGroups;
        std::vector<FieldView> _setFields;
        Rml::String _setGroup = "rates", _setHint, _setStatus, _setSaveLabel = "Сохранить";
        bool _setCanSave = false, _setShowKeys = true;
        int _setPreset = 0;
        // data
        std::vector<DataRow> _dataRows;
        Rml::String _dataSum;
        // plugins
        std::unique_ptr<Packages::Manager> _packages;
        std::vector<PluginView> _plRows;
        Rml::String _plStatus = "Список пакетов ещё не загружен.";
        bool _plBusy = false, _plIndexLoaded = false;
        std::thread _plThread;
        std::atomic<bool> _plDone{ false };
        std::string _plError, _plNewStatus;   // written by the plugins thread, taken over in Tick
        std::vector<std::string> _plLog;

        ServerState _lastState = ServerState::Stopped;
        bool _pendingPlay = false, _pendingRestart = false, _quitting = false, _startHidden = false, _scriptClose = false;
        uint64_t _lastStatsTick = 0, _accRefreshAt = 0, _lastBackupCheck = 0;
        HANDLE _game = nullptr;

        std::mutex _asyncLock;
        std::wstring _pickedDir;
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
        for (DatabaseFile const& db : FindDatabases(ServerConfig(), Root()))
            if (db.name == "world" && !db.path.empty() && !fs::exists(db.path, ec))
                return true;
        // An interrupted install leaves the databases but no client data; the server cannot start without DBC.
        fs::path dbc = ConfPath("DataDir", ".") / "dbc";
        return !fs::is_directory(dbc, ec) || fs::is_empty(dbc, ec);
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

    int Launcher::Run()
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        _exe = buf;
        _exeDir = _exe.parent_path();

        for (int i = 1; i < __argc; ++i)
            if (std::string_view(__argv[i]) == "--tray")
                _startHidden = true;

        _settings.file = _exeDir / "lonelyice.ini";
        _packages = std::make_unique<Packages::Manager>(_exeDir / "plugins");
        _settings.Load();

        _server = std::make_unique<ServerProcess>([] { UiBackend::Wake(); });
        _wizard = std::make_unique<Wizard>(Wizard::Host{ _exe, _exeDir, [this] { return Root(); }, [this] { return _server->IsRunning(); },
            [this] { return _settings.sqlStamp; },
            [this](InstallOptions const& o)
            {
                _settings.dataRoot = o.root.wstring();
                _settings.serverConfig.clear();
                _settings.clientPath = o.client.wstring();
                if (o.db)
                {
                    _settings.sqlStamp = Installer::SqlStamp(o.setupDir);
                    _settings.realmName = o.realmName;
                    _settings.pendingRealmName.clear();
                }
                _settings.Save();
                RefreshClient();
                LoadSettingsModel();
                AddEvent("Установка завершена: " + WideToUtf8(o.root.wstring()));
            },
            [this] { Play(); }, [] { UiBackend::Wake(); } });

        if (!UiBackend::Initialize("LonelyIce", 960, 680, _settings.uiScale / 100.f))
        {
            MessageBoxW(nullptr, L"Не удалось создать окно с OpenGL 3.3.", L"LonelyIce", MB_ICONERROR);
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
            MessageBoxW(nullptr, L"Не удалось загрузить интерфейс (ui/launcher.rml).", L"LonelyIce", MB_ICONERROR);
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
                AddEvent("В setup\\sql.pak новые обновления баз: вкладка «Данные», кнопка «Мастер установки…», пункт «Базы данных».");
            if (_startHidden && _settings.trayOnClose)
                UiBackend::HideWindow();
            if (_settings.autoStart)
                StartServer();
        }

        for (;;)
        {
            bool closeRequested = false;
            bool alive = UiBackend::ProcessEvents(_ctx, UiBackend::IsWindowVisible() ? 0.5 : 1.0, closeRequested);
            closeRequested = closeRequested || std::exchange(_scriptClose, false);
            if (!alive)
            {
                // Windows session ends: do not wait for anything.
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
                    _tray.SetTooltip("LonelyIce — " + _stateTitle + ". Меню: правый клик");
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
        if (_game)
            CloseHandle(_game);
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
        if (_tab == "settings")
            BuildSettingsFields();
        Message("Масштаб интерфейса: " + std::to_string(percent) + " %");
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
        _model = c.GetModelHandle();
        _wizard->SetModel(_model);

        _doc = _ctx->LoadDocument("ui/launcher.rml");
        if (!_doc)
            return false;
        _doc->Show();

        _log = _doc->GetElementById("log");
        if (Rml::Element* input = _doc->GetElementById("cmd"))
            input->AddEventListener(Rml::EventId::Keydown, this);
        return true;
    }

    void Launcher::SetupTray()
    {
        _tray.Create(_icon, {
            { "status", "LonelyIce", nullptr },
            { "open", "Открыть LonelyIce", [this] { ShowWindow(); } },
            { "play", "Играть", [this] { Play(); } },
            { "", "", nullptr },
            { "toggle", "Запустить сервер", [this] { ToggleServer(); } },
            { "restart", "Перезапустить сервер", [this] { RestartServer(); } },
            { "", "", nullptr },
            { "tab-console", "Консоль", [this] { ShowWindow(); OpenTab("console"); } },
            { "tab-commands", "Команды", [this] { ShowWindow(); OpenTab("commands"); } },
            { "tab-accounts", "Аккаунты", [this] { ShowWindow(); OpenTab("accounts"); } },
            { "tab-settings", "Настройки", [this] { ShowWindow(); OpenTab("settings"); } },
            { "backup", "Резервная копия сейчас", [this] { StartBackup(false); } },
            { "", "", nullptr },
            { "quit", "Выход", [this] { BeginQuit(); } },
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
            s.RegisterMember("installed", &PluginView::installed);
            s.RegisterMember("enabled", &PluginView::enabled);
        }
        c.RegisterArray<std::vector<PluginView>>();

        if (auto s = c.RegisterStruct<DataRow>())
        {
            s.RegisterMember("title", &DataRow::title);
            s.RegisterMember("detail", &DataRow::detail);
            s.RegisterMember("status", &DataRow::status);
            s.RegisterMember("size", &DataRow::size);
        }
        c.RegisterArray<std::vector<DataRow>>();

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
        onArg("pick_locale", [this](Rml::Variant const& v)
        {
            _settings.locale = v.Get<Rml::String>();
            _settings.Save();
            for (SetValue& sv : _settingsModel.Values())
                if (sv.def->key == "Launcher.Locale")
                    sv.orig = sv.cur = _settings.locale;
            BuildSettingsFields();
            RefreshClient();
            Message("Язык запуска: " + _settings.locale);
        });
        on("open_server_dir", [this] { ShellExecuteW(nullptr, L"open", Root().c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        on("open_logs", [this] { ShellExecuteW(nullptr, L"open", ConfPath("LogsDir", "logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        on("open_data", [this] { ShellExecuteW(nullptr, L"open", ConfPath("DataDir", ".").c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        on("open_backups", [this]
        {
            std::error_code ec;
            fs::create_directories(Root() / "backups", ec);
            ShellExecuteW(nullptr, L"open", (Root() / "backups").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
            Message("Впишите текст в «Объявление в чат» и нажмите «Выполнить».");
        });

        on("acc_create", [this] { CreateAccount(); });
        on("acc_refresh", [this] { if (_server->SendCommand("@@accounts")) _accNote = "Загружаем…"; });

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
        on("pl_check", [this] { CheckPackageIndex(); });
        on("pl_update_all", [this] { PluginAction("update_all", -1); });
        onArg("pl_toggle", [this](Rml::Variant const& v) { PluginAction("toggle", v.Get<int>()); });
        onArg("pl_remove", [this](Rml::Variant const& v) { PluginAction("remove", v.Get<int>()); });
        onArg("pl_install", [this](Rml::Variant const& v) { PluginAction("install", v.Get<int>()); });
        on("open_wizard", [this] { OpenWizard(); });
    }

    // Dev hook for driving the UI without a mouse: with LONELYICE_UI_SCRIPT=<file> set, each line of that file
    // ("click <id>" or "type <id> <text>") is executed and the file is deleted.
    void Launcher::RunUiScript()
    {
        static std::wstring const path = []
        {
            wchar_t buf[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LONELYICE_UI_SCRIPT", buf, MAX_PATH);
            return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring();
        }();
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
                _accRows.push_back({ a.name, a.gmLevel ? "GM " + std::to_string(a.gmLevel) : "игрок", std::to_string(a.characters), FormatLogin(a.lastLogin), a.gmLevel > 0 });
            }
            if (botAccounts)
                _accRows.push_back({ "RNDBOT… (" + std::to_string(botAccounts) + ")", "боты", std::to_string(botChars), "служебные", false });
            _accLoaded = true;

            std::stable_sort(characters.begin(), characters.end(), [](CharacterInfo const& a, CharacterInfo const& b) { return a.online > b.online; });
            _cmdChars.clear();
            for (CharacterInfo const& ch : characters)
                _cmdChars.push_back({ ch.name, ch.name + " · " + std::to_string(ch.level) + (ch.online ? " · в игре" : "") });
            if (!_cmdChars.empty() && std::none_of(_cmdChars.begin(), _cmdChars.end(), [&](Opt const& o) { return o.id == _cmdTarget; }))
                _cmdTarget = _cmdChars.front().id;
            _model.DirtyVariable("acc_rows");
            _model.DirtyVariable("acc_loaded");
            _model.DirtyVariable("cmd_chars");
            _model.DirtyVariable("cmd_target");
        }

        uint64_t tick = GetTickCount64();
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
                _diff = ready ? std::to_string(st.diff) + " мс" : "—";
                _memory = FormatBytes(_server->GetMemoryBytes());
                if (now == ServerState::Starting || now == ServerState::Loading)
                    _stateSub = "идёт загрузка, " + std::to_string((tick - _server->GetStartTick()) / 1000) + " с";
            }
            else
                _uptime = _players = _bots = _diff = _memory = "—";
            for (char const* v : { "uptime", "players", "bots", "diff", "memory", "state_sub" })
                _model.DirtyVariable(v);

            if (_game && WaitForSingleObject(_game, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(_game);
                _game = nullptr;
                AddEvent("Игра закрыта");
                if (_settings.stopWithGame && _server->IsRunning())
                {
                    AddEvent("Сервер останавливается вместе с игрой");
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
            RefreshPlugins();
            LoadSettingsModel();
        }

        if (_syncDone.exchange(false))
        {
            if (_syncThread.joinable())
                _syncThread.join();
            _syncBusy = false;
            for (std::string const& line : _syncResult.log)
                AddEvent("Клиент: " + line);
            if (_syncResult.ok)
                LaunchGame();
            else
            {
                AddEvent("Клиент не подготовлен: " + _syncResult.error);
                Message("Клиент не подготовлен: " + _syncResult.error);
            }
        }

        if (_backupDone.exchange(false))
        {
            if (_backupThread.joinable())
                _backupThread.join();
            _backupBusy = false;
            _model.DirtyVariable("backup_busy");
            if (_backupResult.ok)
                AddEvent("Резервная копия: " + FormatBytes(_backupResult.bytes) + ", backups\\" + WideToUtf8(_backupResult.dir.filename().wstring()));
            else
                AddEvent("Резервная копия не удалась: " + _backupResult.message);
        }

        std::wstring picked;
        {
            std::lock_guard<std::mutex> guard(_asyncLock);
            picked.swap(_pickedDir);
        }
        if (!picked.empty())
        {
            if (GameClient::IsClientDir(picked))
            {
                _settings.clientPath = picked;
                _settings.Save();
                RefreshClient();
                LoadSettingsModel();
                Message("Папка игры сохранена.");
            }
            else
                Message("В этой папке нет Wow.exe и Data\\common.MPQ.");
        }

        if (_tab == "commands")
            SyncCommandCards();
        else if (_tab == "settings")
            SyncSettingsFields();
    }

    void Launcher::OnStateChanged(ServerState prev, ServerState now)
    {
        switch (now)
        {
            case ServerState::Starting:
                AddEvent("Запуск сервера");
                break;
            case ServerState::Ready:
            {
                AddEvent("Мир готов за " + std::to_string((GetTickCount64() - _server->GetStartTick()) / 1000) + " с, вход открыт");
                std::string realm = _server->GetRealmName();
                if (!_settings.pendingRealmName.empty() && _settings.pendingRealmName != realm)
                {
                    _server->SendCommand("@@realmname " + _settings.pendingRealmName);
                    AddEvent("Имя мира изменено на «" + _settings.pendingRealmName + "», виден после перезапуска");
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
                    AddEvent("Сервер остановлен");
                if (_pendingRestart)
                {
                    _pendingRestart = false;
                    StartServer();
                }
                break;
            case ServerState::Failed:
                AddEvent("Сервер завершился с ошибкой: " + _server->GetFailReason() + ". Подробности во вкладке «Консоль».");
                _pendingPlay = _pendingRestart = false;
                break;
            default:
                break;
        }
        if (now != ServerState::Ready)
        {
            _accLoaded = false;
            _accNote = "Запустите сервер, чтобы увидеть аккаунты.";
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
            case ServerState::Stopped: _state = "stopped"; _stateTitle = "Остановлен"; _stateSub = "нажмите «Запустить» или «Играть»"; break;
            case ServerState::Starting: _state = "starting"; _stateTitle = "Запуск"; _stateSub = "конфиг и база данных"; break;
            case ServerState::Loading: _state = "loading"; _stateTitle = "Загрузка"; _stateSub = "загружаем мир"; break;
            case ServerState::Ready: _state = "ready"; _stateTitle = "Работает"; _stateSub = "вход открыт"; break;
            case ServerState::Stopping: _state = "stopping"; _stateTitle = "Остановка"; _stateSub = "сохраняем персонажей"; break;
            case ServerState::Failed: _state = "failed"; _stateTitle = "Ошибка"; _stateSub = _server->GetFailReason(); break;
        }
        _authOn = s == ServerState::Loading || s == ServerState::Ready;
        _worldOn = s == ServerState::Ready;
        _toggleLabel = _running ? "Остановить" : "Запустить";

        ConfFile conf;
        conf.Load(ServerConfig());
        _authPort = conf.Get("RealmServerPort").value_or("3724");
        _worldPort = conf.Get("WorldServerPort").value_or("8085");
        _soapPort = conf.Get("SOAP.Port").value_or("7878");
        std::string soap = conf.Get("SOAP.Enabled").value_or("0");
        _soapEnabled = soap == "1" || soap == "true";
        _logsDir = WideToUtf8(ConfPath("LogsDir", "logs").wstring());

        _playLabel = "ИГРАТЬ";
        if (NeedsSetup())
        {
            _playLabel = "УСТАНОВИТЬ";
            _playSub = "подготовить данные";
            _playEnabled = true;
        }
        else if (!_clientOk)
        {
            _playSub = "укажите папку игры";
            _playEnabled = false;
        }
        else if (s == ServerState::Ready)
        {
            _playSub = "запустить Wow.exe";
            _playEnabled = true;
        }
        else if (s == ServerState::Stopping)
        {
            _playSub = "сервер останавливается";
            _playEnabled = false;
        }
        else if (_pendingPlay)
        {
            _playSub = "игра откроется, когда мир загрузится";
            _playEnabled = false;
        }
        else
        {
            _playSub = "запустить сервер и игру";
            _playEnabled = true;
        }
        _model.DirtyAllVariables();
    }

    void Launcher::RefreshTray()
    {
        _tray.SetLabel("status", "Сервер: " + _stateTitle);
        _tray.SetLabel("toggle", _running ? "Остановить сервер" : "Запустить сервер");
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
            _clientPath = "Клиент 3.3.5a не найден";
            _rlNote = "Укажите папку с Wow.exe кнопкой «Обзор…».";
        }
        else
        {
            _clientPath = WideToUtf8(_client.dir.wstring());
            if (_settings.clientPath.empty())
            {
                _settings.clientPath = _client.dir.wstring();
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
                _rlNote = "Wow.exe " + (_client.version.empty() ? std::string("неизвестной версии") : _client.version) + ", нужен 3.3.5a (12340).";
            else if (_client.locales.empty())
                _rlNote = "В Data\\ не найдено ни одной локали.";
            else if (!launchOk)
                _rlNote = launch + ": чужой адрес" + (_settings.writeRealmlist ? ", лаунчер поправит его при запуске игры." : ".");
            else if (foreign)
                _rlNote = "Выделен язык запуска; ещё в " + std::to_string(foreign) + " чужой адрес.";
            else
                _rlNote = "Выделен язык запуска; везде 127.0.0.1.";
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

    void Launcher::OpenTab(std::string const& tab)
    {
        _tab = tab;
        if (tab == "accounts" || tab == "commands")
        {
            if (_server->GetState() == ServerState::Ready && _server->SendCommand("@@accounts") && !_accLoaded)
                _accNote = "Загружаем…";
        }
        if (tab == "settings" && _settingsModel.ChangedCount() == 0)
            LoadSettingsModel();
        if (tab == "data")
            RefreshData();
        if (tab == "plugins")
        {
            RefreshPlugins();
            if (!_plIndexLoaded && !_plBusy)
                CheckPackageIndex();
        }
        _model.DirtyAllVariables();
    }

    void Launcher::StartServer()
    {
        if (_server->IsRunning())
            return;
        if (_wizard->IsInstalling())
        {
            Message("Идёт установка, сервер запустится после неё.");
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
            Message("Не найден конфиг сервера: " + WideToUtf8(config.wstring()));
            return;
        }
        AppendLog("-- запуск: " + WideToUtf8(config.wstring()), "me");
        EnvList env = ModuleConfigOverrides(config, Root());
        env.emplace_back(L"AC_PLUGINS_DIR", (_exeDir / "plugins").wstring());
        // the server builds the plugins' client patches while it starts
        if (_client.valid)
            env.emplace_back(L"LONELYICE_CLIENT", _client.dir.wstring());
        if (!_server->Start(WideToUtf8(_exe.wstring()), WideToUtf8(config.wstring()), WideToUtf8(Root().wstring()), env))
            Message("Не удалось запустить сервер: " + _server->GetFailReason());
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
            Message("Сначала укажите папку с клиентом 3.3.5a.");
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
            Message("Игра уже запущена.");
            return;
        }
        if (_syncBusy)
            return;

        // Plugin addons first (the server built the client patches when it started), off the UI thread.
        _syncBusy = true;
        Message("Готовлю клиент: аддоны плагинов…");
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
        HANDLE process = nullptr;
        if (GameClient::Launch(_client.dir, error, reinterpret_cast<void**>(&process)))
        {
            if (_game)
                CloseHandle(_game);
            _game = process;
            AddEvent("Запущена игра" + (launch.empty() ? std::string() : " (" + launch + ")"));
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
                Message("realmlist уже указывает на этот компьютер.");
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
        std::string start = _client.valid ? WideToUtf8(_client.dir.wstring()) : WideToUtf8(_exeDir.wstring());
        SDL_ShowOpenFolderDialog([](void* self, char const* const* list, int)
        {
            if (!list || !list[0])
                return;
            auto* me = static_cast<Launcher*>(self);
            {
                std::lock_guard<std::mutex> guard(me->_asyncLock);
                me->_pickedDir = Utf8ToWide(list[0]);
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
            Message("Команды принимаются, когда мир запущен.");
            return;
        }
        if (echo)
            AppendLog("AC> " + cmd, "me");
        if (!_server->SendCommand(cmd))
            Message("Команда не отправлена: сервер не отвечает.");
    }

    // ---- accounts

    void Launcher::CreateAccount()
    {
        if (_server->GetState() != ServerState::Ready)
            return;
        std::string login = _accLogin, pass = _accPass;
        if (login.empty() || pass.empty() || login.find(' ') != std::string::npos || pass.find(' ') != std::string::npos)
        {
            Message("Логин и пароль не должны быть пустыми и не должны содержать пробелов.");
            return;
        }
        RunCommand("account create " + login + " " + pass, false);
        AppendLog("AC> account create " + login + " ********", "me");
        if (_accLevel != "0")
            RunCommand("account set gmlevel " + login + " " + _accLevel + " -1");
        AddEvent("Создан аккаунт " + login + (_accLevel != "0" ? ", GM " + _accLevel : ""));
        _accPass.clear();
        _accLogin.clear();
        _model.DirtyVariable("acc_pass");
        _model.DirtyVariable("acc_login");
        _accRefreshAt = GetTickCount64() + 1500;
    }

    // ---- commands

    void Launcher::BuildCommandCards()
    {
        auto const& catalog = CommandCatalog();
        _cmdGroups.clear();
        for (CmdGroup const& g : catalog)
            _cmdGroups.push_back({ g.id, g.name });

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
                    std::string hay = d.title + " " + d.desc + " " + d.tmpl;
                    std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
                    if (hay.find(q) == std::string::npos)
                        continue;
                }
                CmdCardView card;
                card.title = d.title;
                card.desc = d.desc;
                card.group = q.empty() ? "" : g.name;
                card.danger = d.danger;
                card.groupIdx = gi;
                card.cmdIdx = ci;
                for (CmdArgDef const& a : d.args)
                {
                    CmdArgView av{ a.name, std::string(1, a.type), a.def, {} };
                    for (auto const& [id, label] : a.options)
                        av.opts.push_back({ id, label });
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
            std::string button = card.armed ? "Точно?" : d.danger ? "Выполнить…" : "Выполнить";
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
            Message("Выберите персонажа.");
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
        _cmdLast = "Отправлено: " + cmd + ". Ответ во вкладке «Консоль».";
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
            AddEvent("Настройки плагина не прочитаны: " + e);
        if (std::none_of(_settingsModel.Groups().begin(), _settingsModel.Groups().end(), [&](SetGroup const& g) { return g.id == _setGroup; }))
            _setGroup = "rates";
        _setPreset = 0;
        BuildSettingsFields();
    }

    void Launcher::BuildSettingsFields()
    {
        static std::map<std::string, std::string> const applyText = { { "now", "сразу" }, { "rel", ".reload" }, { "rst", "перезапуск" } };

        _setGroups.clear();
        for (SetGroup const& g : _settingsModel.Groups())
        {
            auto const& vals = _settingsModel.Values();
            if (std::none_of(vals.begin(), vals.end(), [&](SetValue const& v) { return v.def->group == g.id; }))
                continue;
            _setGroups.push_back({ g.id, g.name, _settingsModel.ChangedCount(g.id) });
            if (g.id == _setGroup)
                _setHint = g.hint;
        }

        _setFields.clear();
        auto& values = _settingsModel.Values();
        for (int i = 0; i < int(values.size()); ++i)
        {
            SetValue const& v = values[i];
            if (v.def->group != _setGroup)
                continue;
            FieldView f;
            f.label = v.def->label;
            f.key = v.def->key;
            f.type = std::string(1, v.def->type);
            f.value = v.cur;
            f.on = v.cur == "1";
            f.apply = v.def->apply;
            f.apply_text = applyText.at(v.def->apply);
            f.hint = v.def->hint;
            f.changed = _settingsModel.Changed(v);
            f.index = i;
            for (auto const& [id, label] : v.def->options)
                f.opts.push_back({ id, label });
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
        _setStatus = n ? "Изменено: " + std::to_string(n) + (restart ? ", нужен перезапуск" : "") : "Изменений нет";
        _setSaveLabel = restart ? "Сохранить и перезапустить" : "Сохранить";
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

        std::string what = "Настройки сохранены";
        if (_server->GetState() == ServerState::Ready)
        {
            if (r.reload && !restartNeeded)
            {
                RunCommand("reload config");
                what += ", конфиг перечитан";
            }
            if (restartNeeded)
            {
                RestartServer();
                what += ", сервер перезапускается";
            }
        }
        AddEvent(what);
        Message(what + ".");
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
        std::map<std::string, Packages::Package const*> newest;
        for (Packages::Package const& p : _packages->Available())
            if (!newest.count(p.id) || Packages::Manager::CompareVersions(p.version, newest[p.id]->version) > 0)
                newest[p.id] = &p;

        std::set<std::string> installed;
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
                v.update = it->second->version;
            std::string deps;
            for (auto const& [dep, range] : l.manifest.depends)
                deps += (deps.empty() ? "" : ", ") + dep + " " + range;
            v.note = l.manifest.id + (deps.empty() ? "" : " · нужен " + deps) + (l.enabled ? "" : " · выключен");
            installed.insert(l.manifest.id);
            _plRows.push_back(std::move(v));
        }
        for (auto const& [id, p] : newest)
        {
            if (installed.count(id))
                continue;
            PluginView v;
            v.id = id;
            v.name = p->name;
            v.version = p->version;
            v.desc = p->description;
            v.note = id + " · можно установить";
            _plRows.push_back(std::move(v));
        }
        _model.DirtyVariable("pl_rows");
    }

    bool Launcher::PluginsLocked()
    {
        if (_plBusy)
            return true;
        if (_server->IsRunning())
        {
            Message("Остановите сервер: плагины меняются, пока он выключен.");
            return true;
        }
        return false;
    }

    void Launcher::CheckPackageIndex()
    {
        if (_plBusy)
            return;
        _plBusy = true;
        _plStatus = "Загружаем список пакетов…";
        _model.DirtyVariable("pl_busy");
        _model.DirtyVariable("pl_status");
        _plThread = std::thread([this, index = _settings.packageIndex]
        {
            std::string error;
            _plLog.clear();
            _plError.clear();
            if (_packages->LoadIndex(index, error))
            {
                _plIndexLoaded = true;
                _plNewStatus = "Пакетов в каталоге: " + std::to_string(_packages->Available().size()) + ".";
            }
            else
            {
                _plNewStatus = "Каталог пакетов недоступен, подробности на вкладке «Обзор».";
                _plLog.push_back("Каталог пакетов: " + error);
            }
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
            Message("Всё уже установлено.");
            return;
        }
        _plBusy = true;
        _plStatus = "Устанавливаем…";
        _model.DirtyVariable("pl_busy");
        _model.DirtyVariable("pl_status");
        _plThread = std::thread([this, plan]
        {
            _plLog.clear();
            _plError.clear();
            std::string error;
            bool const ok = _packages->Install(plan, error, [this](std::string const& line) { _plLog.push_back("Плагины: " + line); });
            _plError = ok ? "" : error;
            if (!ok)
                _plLog.push_back("Плагины: " + error);
            _plNewStatus = ok ? "Готово. Базы и клиент обновятся при запуске сервера." : "Не удалось, подробности на вкладке «Обзор».";
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
                Message("Сначала загрузите список пакетов.");
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
                Message(v.name + " нужен плагинам: " + list + ".");
                return;
            }
        }

        bool ok = action == "remove" ? _packages->Remove(id, error) : _packages->SetEnabled(id, !v.enabled, error);
        if (!ok)
        {
            Message(error);
            return;
        }
        std::string const what = action == "remove" ? "удалён" : v.enabled ? "выключен" : "включён";
        AddEvent("Плагин " + v.name + " " + what);
        Message("Плагин " + v.name + " " + what + ". Базы и клиент обновятся при запуске сервера.");
        RefreshPlugins();
        LoadSettingsModel();
    }

    void Launcher::RefreshData()
    {
        _dataRows.clear();
        uint64_t total = 0;
        std::vector<std::string> missing;

        static std::map<std::string, std::pair<std::string, std::string>> const dbTitle = {
            { "auth", { "База входа", "аккаунты, права, список миров" } }, { "characters", { "База персонажей", "персонажи, вещи, почта, гильдии" } },
            { "world", { "База мира", "существа, задания, добыча, предметы" } }, { "playerbots", { "База ботов", "поведение и маршруты ботов" } } };
        for (DatabaseFile const& db : FindDatabases(ServerConfig(), Root()))
        {
            std::error_code ec;
            std::string title = dbTitle.count(db.name) ? dbTitle.at(db.name).first : db.name;
            std::string detail = dbTitle.count(db.name) ? dbTitle.at(db.name).second : std::string();
            if (db.path.empty())
            {
                _dataRows.push_back({ title, "внешний сервер базы данных", "ok", "" });
                continue;
            }
            bool ok = fs::exists(db.path, ec);
            uint64_t size = ok ? fs::file_size(db.path, ec) : 0;
            total += size;
            if (!ok)
                missing.push_back(title);
            _dataRows.push_back({ title, detail, ok ? "ok" : "bad", ok ? FormatBytes(size) : "нет" });
        }

        fs::path data = ConfPath("DataDir", ".");
        struct Part { char const* dir; char const* title; char const* detail; bool required; };
        for (Part const& p : { Part{ "dbc", "DBC", "таблицы клиента", true }, Part{ "maps", "Карты", "карты высот и зон", true },
                 Part{ "Cameras", "Камеры", "ролики и полёты", false }, Part{ "vmaps", "Модели зданий (vmaps)", "линия видимости, пещеры", false },
                 Part{ "mmaps", "Навигация (mmaps)", "пути для монстров и ботов", false } })
        {
            DirStats s = Scan(data / p.dir);
            total += s.bytes;
            bool ok = s.files > 0;
            if (!ok)
                missing.push_back(p.title);
            _dataRows.push_back({ p.title, std::string(p.detail) + (ok ? ", файлов: " + std::to_string(s.files) : ""),
                ok ? "ok" : (p.required ? "bad" : "warn"), ok ? FormatBytes(s.bytes) : "нет" });
        }

        if (missing.empty())
            _dataSum = "всё на месте · " + FormatBytes(total);
        else
        {
            _dataSum = "не хватает: ";
            for (std::size_t i = 0; i < missing.size(); ++i)
                _dataSum += (i ? ", " : "") + missing[i];
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
        if (std::none_of(dbs.begin(), dbs.end(), [](DatabaseFile const& d) { return !d.path.empty(); }))
        {
            Message("Резервная копия работает только с базами в файлах.");
            return;
        }
        _backupBusy = true;
        _model.DirtyVariable("backup_busy");
        if (!scheduled)
            Message("Делаем резервную копию…");
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
        if (_settings.backupTime.size() != 5 || _backupBusy)
            return;
        std::string today = Now("%Y-%m-%d");
        if (_settings.lastBackupDay == today || Now("%H:%M") < _settings.backupTime)
            return;
        _settings.lastBackupDay = today;
        _settings.Save();
        StartBackup(true);
    }
}

int LauncherMain(int /*argc*/, char** /*argv*/)
{
    Launcher launcher;
    return launcher.Run();
}
