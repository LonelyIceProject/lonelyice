#include "Assets.h"
#include "GameClient.h"
#include "ServerProcess.h"
#include "TextUtil.h"
#include "UiBackend.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <Windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    constexpr int MaxLogLines = 600;
    constexpr char RealmHost[] = "127.0.0.1";

    struct Chip
    {
        Rml::String text;
        bool ok = false;
    };

    struct EventRow
    {
        Rml::String time;
        Rml::String text;
    };

    // Plain key=value file next to the exe.
    struct Settings
    {
        fs::path file;
        std::wstring clientPath;
        std::wstring serverConfig;

        void Load()
        {
            wchar_t buf[1024];
            GetPrivateProfileStringW(L"client", L"path", L"", buf, 1024, file.c_str());
            clientPath = buf;
            GetPrivateProfileStringW(L"server", L"config", L"", buf, 1024, file.c_str());
            serverConfig = buf;
        }

        void Save() const
        {
            WritePrivateProfileStringW(L"client", L"path", clientPath.c_str(), file.c_str());
            WritePrivateProfileStringW(L"server", L"config", serverConfig.c_str(), file.c_str());
        }
    };

    std::string Now()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", &tm);
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
        else
            snprintf(buf, sizeof(buf), "%llu МБ", (unsigned long long)(b >> 20));
        std::string s = buf;
        for (char& c : s)
            if (c == '.')
                c = ',';
        return s;
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
        void Tick();
        void OnStateChanged(ServerState prev, ServerState now);
        void RefreshServerView();
        void RefreshClient();
        void AppendLog(std::string const& utf8, char const* cls);
        void AddEvent(std::string text);
        void Message(std::string text);

        void StartServer();
        void ToggleServer();
        void RestartServer();
        void Play();
        void FixRealmlist(bool quiet);
        void BrowseClient();
        void SendConsoleCommand();
        void RunCommand(std::string const& cmd);

        fs::path _exe, _exeDir;
        Settings _settings;
        std::unique_ptr<ServerProcess> _server;
        ClientInfo _client;

        Rml::Context* _ctx = nullptr;
        Rml::ElementDocument* _doc = nullptr;
        Rml::Element* _log = nullptr;
        Rml::DataModelHandle _model;

        // model
        Rml::String _state = "stopped", _stateTitle, _stateSub, _toggleLabel, _uptime = "—", _players = "—", _diff = "—", _memory = "—";
        Rml::String _authPort = "3724", _worldPort = "8085", _clientPath, _serverDirText, _message, _playLabel, _playSub, _tab = "overview", _shownTab = "overview";
        bool _running = false, _authOn = false, _worldOn = false, _clientOk = false, _playEnabled = true, _closing = false;
        std::vector<Chip> _chips;
        std::vector<EventRow> _events;

        ServerState _lastState = ServerState::Stopped;
        bool _pendingPlay = false, _pendingRestart = false;
        uint64_t _lastStatsTick = 0;

        std::mutex _dialogLock;
        std::wstring _pickedDir;
    };

    fs::path DefaultServerConfig(fs::path const& exeDir)
    {
        for (char const* rel : { "configs-sqlite/worldserver.conf", "configs/worldserver.conf" })
            if (fs::exists(exeDir / rel))
                return exeDir / rel;
        return exeDir / "configs" / "worldserver.conf";
    }

    int Launcher::Run()
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        _exe = buf;
        _exeDir = _exe.parent_path();
        _serverDirText = WideToUtf8(_exeDir.wstring());

        _settings.file = _exeDir / "lonelyice.ini";
        _settings.Load();

        _server = std::make_unique<ServerProcess>([] { UiBackend::Wake(); });

        if (!UiBackend::Initialize("LonelyIce", 900, 640))
        {
            MessageBoxW(nullptr, L"Не удалось создать окно с OpenGL 3.3.", L"LonelyIce", MB_ICONERROR);
            return 1;
        }

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

        RefreshClient();
        RefreshServerView();

        for (;;)
        {
            if (!UiBackend::ProcessEvents(_ctx, 0.5))
            {
                if (!_server->IsRunning())
                    break;
                if (_closing)
                {
                    _server->Kill();
                    break;
                }
                _closing = true;
                _server->Stop();
                _model.DirtyVariable("closing");
            }

            Tick();

            if (_closing && !_server->IsRunning())
                break;

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

        _settings.Save();
        _server.reset();
        Rml::Shutdown();
        UiBackend::Shutdown();
        return 0;
    }

    bool Launcher::SetupUi()
    {
        for (char const* font : { "fonts/PTSans-Regular.ttf", "fonts/PTSans-Bold.ttf", "fonts/PTMono-Regular.ttf", "fonts/Forum-Regular.ttf" })
            if (!Rml::LoadFontFace(font))
                return false;

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(UiBackend::GetWindow(), &w, &h);
        _ctx = Rml::CreateContext("main", Rml::Vector2i(w, h));
        if (!_ctx)
            return false;
        _ctx->SetDensityIndependentPixelRatio(UiBackend::GetDisplayScale());

        Rml::DataModelConstructor c = _ctx->CreateDataModel("li");
        if (!c)
            return false;
        BindModel(c);
        _model = c.GetModelHandle();

        _doc = _ctx->LoadDocument("ui/launcher.rml");
        if (!_doc)
            return false;
        _doc->Show();

        _log = _doc->GetElementById("log");
        if (Rml::Element* input = _doc->GetElementById("cmd"))
            input->AddEventListener(Rml::EventId::Keydown, this);
        return true;
    }

    void Launcher::BindModel(Rml::DataModelConstructor& c)
    {
        if (auto chip = c.RegisterStruct<Chip>())
        {
            chip.RegisterMember("text", &Chip::text);
            chip.RegisterMember("ok", &Chip::ok);
        }
        c.RegisterArray<std::vector<Chip>>();

        if (auto row = c.RegisterStruct<EventRow>())
        {
            row.RegisterMember("time", &EventRow::time);
            row.RegisterMember("text", &EventRow::text);
        }
        c.RegisterArray<std::vector<EventRow>>();

        c.Bind("state", &_state);
        c.Bind("state_title", &_stateTitle);
        c.Bind("state_sub", &_stateSub);
        c.Bind("toggle_label", &_toggleLabel);
        c.Bind("running", &_running);
        c.Bind("auth_on", &_authOn);
        c.Bind("world_on", &_worldOn);
        c.Bind("auth_port", &_authPort);
        c.Bind("world_port", &_worldPort);
        c.Bind("uptime", &_uptime);
        c.Bind("players", &_players);
        c.Bind("diff", &_diff);
        c.Bind("memory", &_memory);
        c.Bind("client_path", &_clientPath);
        c.Bind("client_ok", &_clientOk);
        c.Bind("chips", &_chips);
        c.Bind("events", &_events);
        c.Bind("server_dir", &_serverDirText);
        c.Bind("message", &_message);
        c.Bind("play_label", &_playLabel);
        c.Bind("play_sub", &_playSub);
        c.Bind("play_enabled", &_playEnabled);
        c.Bind("tab", &_tab);
        c.Bind("closing", &_closing);

        c.BindEventCallback("toggle_server", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { ToggleServer(); });
        c.BindEventCallback("restart_server", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { RestartServer(); });
        c.BindEventCallback("play", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { Play(); });
        c.BindEventCallback("fix_realmlist", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { FixRealmlist(false); });
        c.BindEventCallback("browse_client", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { BrowseClient(); });
        c.BindEventCallback("send_command", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { SendConsoleCommand(); });
        c.BindEventCallback("run_command", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const& args)
        {
            if (!args.empty())
                RunCommand(args[0].Get<Rml::String>());
        });
        c.BindEventCallback("open_server_dir", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&)
        {
            ShellExecuteW(nullptr, L"open", _exeDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        c.BindEventCallback("open_client_dir", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&)
        {
            if (_client.valid)
                ShellExecuteW(nullptr, L"open", _client.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    }

    // Dev hook for driving the UI without a mouse: with LONELYICE_UI_SCRIPT=<file> set, each line of that file
    // ("click <id>" or "type <id> <text>") is executed and the file is deleted.
    void RunUiScript(Rml::ElementDocument* doc)
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
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(in, line))
            lines.push_back(line);
        in.close();
        std::error_code ec;
        fs::remove(path, ec);

        for (std::string const& l : lines)
        {
            std::istringstream s(l);
            std::string verb, id;
            s >> verb >> id;
            Rml::Element* el = doc->GetElementById(id);
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
                if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(el))
                    input->SetValue(text);
            }
        }
    }

    void Launcher::Tick()
    {
        RunUiScript(_doc);

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

        uint64_t tick = GetTickCount64();
        if (tick - _lastStatsTick >= 1000)
        {
            _lastStatsTick = tick;
            if (_server->IsRunning())
            {
                ServerStats st = _server->GetStats();
                _uptime = now == ServerState::Ready ? FormatUptime(st.uptime) : "—";
                _players = now == ServerState::Ready ? std::to_string(st.players) : "—";
                _diff = now == ServerState::Ready ? std::to_string(st.diff) + " мс" : "—";
                _memory = FormatBytes(_server->GetMemoryBytes());
                if (now == ServerState::Starting || now == ServerState::Loading)
                    _stateSub = "идёт загрузка, " + std::to_string((tick - _server->GetStartTick()) / 1000) + " с";
            }
            else
                _uptime = _players = _diff = _memory = "—";
            _model.DirtyVariable("uptime");
            _model.DirtyVariable("players");
            _model.DirtyVariable("diff");
            _model.DirtyVariable("memory");
            _model.DirtyVariable("state_sub");
        }

        std::wstring picked;
        {
            std::lock_guard<std::mutex> guard(_dialogLock);
            picked.swap(_pickedDir);
        }
        if (!picked.empty())
        {
            if (GameClient::IsClientDir(picked))
            {
                _settings.clientPath = picked;
                _settings.Save();
                RefreshClient();
                Message("Папка игры сохранена.");
            }
            else
                Message("В этой папке нет Wow.exe и Data\\common.MPQ.");
        }
    }

    void Launcher::OnStateChanged(ServerState prev, ServerState now)
    {
        switch (now)
        {
            case ServerState::Starting:
                AddEvent("Запуск сервера");
                break;
            case ServerState::Ready:
                AddEvent("Мир готов за " + std::to_string((GetTickCount64() - _server->GetStartTick()) / 1000) + " с, вход открыт");
                if (_pendingPlay)
                {
                    _pendingPlay = false;
                    Play();
                }
                break;
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
        RefreshServerView();
    }

    void Launcher::RefreshServerView()
    {
        ServerState s = _server->GetState();
        _running = _server->IsRunning();
        switch (s)
        {
            case ServerState::Stopped: _state = "stopped"; _stateTitle = "Остановлен"; _stateSub = "нажмите «Запустить» или «Играть»"; break;
            case ServerState::Starting: _state = "starting"; _stateTitle = "Запуск"; _stateSub = "конфиг и базы данных"; break;
            case ServerState::Loading: _state = "loading"; _stateTitle = "Загрузка"; _stateSub = "загружаем мир"; break;
            case ServerState::Ready: _state = "ready"; _stateTitle = "Работает"; _stateSub = "SQLite, вход открыт"; break;
            case ServerState::Stopping: _state = "stopping"; _stateTitle = "Остановка"; _stateSub = "сохраняем персонажей"; break;
            case ServerState::Failed: _state = "failed"; _stateTitle = "Ошибка"; _stateSub = _server->GetFailReason(); break;
        }
        _authOn = s == ServerState::Loading || s == ServerState::Ready;
        _worldOn = s == ServerState::Ready;
        _toggleLabel = _running ? "Остановить" : "Запустить";

        if (!_clientOk)
        {
            _playLabel = "ИГРАТЬ";
            _playSub = "укажите папку игры";
            _playEnabled = false;
        }
        else if (s == ServerState::Ready)
        {
            _playLabel = "ИГРАТЬ";
            _playSub = "запустить Wow.exe";
            _playEnabled = true;
        }
        else if (s == ServerState::Stopping)
        {
            _playLabel = "ИГРАТЬ";
            _playSub = "сервер останавливается";
            _playEnabled = false;
        }
        else if (_pendingPlay)
        {
            _playLabel = "ИГРАТЬ";
            _playSub = "игра откроется, когда мир загрузится";
            _playEnabled = false;
        }
        else
        {
            _playLabel = "ИГРАТЬ";
            _playSub = "запустить сервер и игру";
            _playEnabled = true;
        }
        _model.DirtyAllVariables();
    }

    void Launcher::RefreshClient()
    {
        fs::path dir = GameClient::Detect(_settings.clientPath, _exeDir);
        _client = GameClient::Inspect(dir);
        _clientOk = _client.valid;
        _chips.clear();

        if (!_client.valid)
            _clientPath = "Клиент 3.3.5a не найден";
        else
        {
            _clientPath = WideToUtf8(_client.dir.wstring());
            if (_settings.clientPath.empty())
            {
                _settings.clientPath = _client.dir.wstring();
                _settings.Save();
            }
            if (_client.version != "3.3.5.12340")
                _chips.push_back({ "Wow.exe " + (_client.version.empty() ? std::string("?") : _client.version), false });
            std::string good, bad;
            for (ClientLocale const& loc : _client.locales)
            {
                std::string& list = loc.realmlist == RealmHost ? good : bad;
                list += (list.empty() ? "" : ", ") + loc.name;
            }
            if (_client.locales.empty())
                _chips.push_back({ "локали не найдены", false });
            if (!good.empty())
                _chips.push_back({ good + ": 127.0.0.1", true });
            if (!bad.empty())
                _chips.push_back({ bad + ": другой сервер", false });
        }
        RefreshServerView();
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

    void Launcher::StartServer()
    {
        if (_server->IsRunning())
            return;
        fs::path config = _settings.serverConfig.empty() ? DefaultServerConfig(_exeDir) : fs::path(_settings.serverConfig);
        if (!fs::exists(config))
        {
            Message("Не найден конфиг сервера: " + WideToUtf8(config.wstring()));
            return;
        }
        AppendLog("-- запуск: " + WideToUtf8(config.wstring()), "me");
        EnvList env = ModuleConfigOverrides(config, _exeDir);
        if (!_server->Start(WideToUtf8(_exe.wstring()), WideToUtf8(config.wstring()), WideToUtf8(_exeDir.wstring()), env))
            Message("Не удалось запустить сервер: " + _server->GetFailReason());
        RefreshServerView();
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
        if (!_client.valid)
        {
            Message("Сначала укажите папку с клиентом 3.3.5a.");
            return;
        }

        ServerState s = _server->GetState();
        if (s != ServerState::Ready)
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

        FixRealmlist(true);

        std::string error;
        if (GameClient::Launch(_client.dir, error))
        {
            AddEvent("Запущена игра");
            Message("");
        }
        else
            Message(error);
    }

    void Launcher::FixRealmlist(bool quiet)
    {
        if (!_client.valid)
            return;

        bool needed = false;
        for (ClientLocale const& loc : _client.locales)
            if (loc.realmlist != RealmHost)
                needed = true;
        if (!needed)
        {
            if (!quiet)
                Message("realmlist уже указывает на этот компьютер.");
            return;
        }

        std::string error;
        if (!GameClient::WriteRealmlist(_client, RealmHost, error))
        {
            Message(error);
            return;
        }
        // Item and quest cache from another server shows wrong names and tooltips.
        GameClient::ClearWdb(_client.dir);
        AddEvent("realmlist: 127.0.0.1 во всех локалях, кэш клиента очищен");
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
                std::lock_guard<std::mutex> guard(me->_dialogLock);
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

    void Launcher::RunCommand(std::string const& cmd)
    {
        if (_server->GetState() != ServerState::Ready)
        {
            Message("Команды принимаются, когда мир запущен.");
            return;
        }
        AppendLog("AC> " + cmd, "me");
        if (!_server->SendCommand(cmd))
            Message("Команда не отправлена: сервер не отвечает.");
    }
}

int LauncherMain(int /*argc*/, char** /*argv*/)
{
    Launcher launcher;
    return launcher.Run();
}
