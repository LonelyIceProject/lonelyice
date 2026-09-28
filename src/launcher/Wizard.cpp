#include "Wizard.h"
#include "TextUtil.h"
#include "UiBackend.h"
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <fstream>
#include <thread>
#include <Windows.h>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    char const* const StepNames[] = { "Клиент", "Размещение", "Компоненты", "Мир", "Игра", "Установка", "Готово" };
    constexpr int StepCount = 7;

    std::string Utf8(fs::path const& p)
    {
        return WideToUtf8(p.wstring());
    }

    uint64_t FreeBytes(fs::path p)
    {
        std::error_code ec;
        while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p.parent_path() != p)
            p = p.parent_path();
        ULARGE_INTEGER free{};
        if (!GetDiskFreeSpaceExW(p.c_str(), &free, nullptr, nullptr))
            return 0;
        return free.QuadPart;
    }

    std::string Gb(uint64_t bytes)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", double(bytes) / double(1ull << 30));
        std::string s = buf;
        std::replace(s.begin(), s.end(), '.', ',');
        return s + " ГБ";
    }

    uint32_t CountFiles(fs::path const& dir, std::wstring const& ext = {})
    {
        std::error_code ec;
        uint32_t n = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (ext.empty() || it->path().extension() == ext)
                ++n;
        return n;
    }

    bool Writable(fs::path const& dir)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path probe = dir / L".lonelyice-write-test";
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
    _coresNote = "из " + std::to_string(cores) + " потоков процессора";
}

Wizard::~Wizard() = default;

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
    BuildPlaces();
    BuildComponents();
    Go(0);
}

void Wizard::Inspect(fs::path const& client)
{
    _client = client.empty() ? ClientInfo{} : GameClient::Inspect(client);
    _checks.clear();
    _clientFound = _client.valid;
    if (!_client.valid)
    {
        _clientTitle = "Клиент 3.3.5a не найден";
        _clientPath = client.empty() ? "Укажите папку, где лежит Wow.exe" : Utf8(client);
        _checks.push_back({ "bad", "Wow.exe и Data\\common.MPQ", "не найдены" });
        return;
    }
    std::error_code ec;
    bool nextToExe = fs::equivalent(_client.dir, _host.exeDir, ec) || fs::equivalent(_client.dir, _host.exeDir.parent_path(), ec);
    _clientTitle = nextToExe ? "Клиент найден рядом с LonelyIce.exe" : "Клиент найден";
    _clientPath = Utf8(_client.dir);

    bool build = _client.version == "3.3.5.12340";
    _checks.push_back({ build ? "ok" : "bad", "Wow.exe, версия 3.3.5a", _client.version.empty() ? "версия не прочитана" : "сборка " + _client.version.substr(_client.version.rfind('.') + 1) });
    uint32_t mpq = CountFiles(_client.dir / "Data", L".MPQ") + CountFiles(_client.dir / "Data", L".mpq");
    _checks.push_back({ mpq >= 4 ? "ok" : "bad", "Архивы Data\\*.MPQ", std::to_string(mpq) + " шт." });
    for (ClientLocale const& l : _client.locales)
        _checks.push_back({ "ok", "Локаль " + l.name, "Data\\" + l.name });
    if (_client.locales.empty())
        _checks.push_back({ "bad", "Локаль", "в Data\\ нет ни одной" });
    bool writable = Writable(_client.dir / "WTF");
    _checks.push_back({ writable ? "ok" : "bad", "Запись в папку клиента", writable ? "разрешена" : "нет прав, запустите от администратора или перенесите игру" });
    bool running = GameClient::IsRunning(_client.dir);
    _checks.push_back({ running ? "warn" : "ok", running ? "Игра запущена" : "Игра не запущена", running ? "закройте Wow.exe перед установкой" : "" });

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
    return _customPlace;
}

void Wizard::BuildPlaces()
{
    _places.clear();
    auto add = [&](char const* id, char const* title, fs::path const& p, char const* detail)
    {
        _places.push_back({ id, title, p.empty() ? "папка не выбрана" : Utf8(p), p.empty() ? "" : Gb(FreeBytes(p)) + " свободно", detail });
    };
    add("client", "Внутри папки клиента", _client.valid ? _client.dir / "LonelyIce" : fs::path(), "Всё в одном месте, удобно переносить.");
    add("exe", "Рядом с LonelyIce.exe", _host.exeDir, "Если exe лежит отдельно от клиента.");
    add("custom", "Другая папка…", _customPlace, "Например, на быстром SSD.");
}

void Wizard::BuildComponents()
{
    fs::path root = PlacePath();
    fs::path data = root / "data";
    std::error_code ec;
    bool setup = fs::exists(_host.exeDir / "setup" / "sql.pak", ec) && fs::exists(_host.exeDir / "setup" / "configs.pak", ec);
    bool haveDb = !root.empty() && fs::exists(root / "db" / "world.sqlite", ec);
    uint32_t maps = root.empty() ? 0 : CountFiles(data / "maps");
    uint32_t vmaps = root.empty() ? 0 : CountFiles(data / "vmaps", L".vmtree");
    uint32_t mmaps = root.empty() ? 0 : CountFiles(data / "mmaps");

    auto have = [](bool yes, std::string const& what) { return yes ? "уже есть: " + what : std::string(); };
    bool newSql = setup && haveDb && _host.sqlStamp && _host.sqlStamp() != Installer::SqlStamp(_host.exeDir / "setup");
    std::vector<CompRow> old = std::move(_comps);
    _comps = {
        { "db", "Базы данных", "Вход, мир, персонажи и боты из SQL в папке setup", "0,4 ГБ · ≈ 1 мин",
            !setup ? "нет setup\\sql.pak рядом с exe" : newSql ? "в setup\\sql.pak новые обновления, базы будут обновлены" : have(haveDb, "db\\world.sqlite, будут только обновлены"),
            setup && (!haveDb || newSql), !setup },
        { "maps", "DBC и карты", "Таблицы клиента, карты высот, камеры", "0,7 ГБ · ≈ 2 мин", have(maps > 0, std::to_string(maps) + " файлов карт"), maps == 0, false },
        { "vmaps", "Модели зданий (vmaps)", "Линия видимости, пещеры, помещения", "0,6 ГБ · ≈ 5 мин", have(vmaps > 0, std::to_string(vmaps) + " карт"), vmaps == 0, false },
        { "mmaps", "Навигация (mmaps)", "Пути для монстров и ботов. Без неё боты ходят сквозь стены", "6 ГБ · 20–60 мин",
            mmaps ? "уже есть " + std::to_string(mmaps) + " файлов; включите, чтобы достроить недостающие" : "", mmaps == 0, false },
        { "client", "Подготовка клиента", "realmlist, кэш, логин в окне входа, ярлык", "< 1 мин", "", true, false },
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
    char buf[96];
    snprintf(buf, sizeof(buf), "Выбрано: %d из %zu · ≈ %.1f ГБ на диске", n, _comps.size(), need);
    _total = buf;
    std::replace(_total.begin(), _total.end(), '.', ',');
}

void Wizard::RefreshFooter()
{
    _steps.clear();
    for (int i = 0; i < StepCount; ++i)
        _steps.push_back({ StepNames[i], i < _step ? "done" : i == _step ? "cur" : "" });
    _counter = "шаг " + std::to_string(_step + 1) + " из " + std::to_string(StepCount);
    _backVisible = _step > 0 && _step < 5;
    bool installing = _installer.IsRunning();
    _cancelLabel = _step == 5 && installing ? "Прервать" : _step == 6 ? "Закрыть" : "Позже";
    _nextOk = true;
    if (_step == 4)
        _nextLabel = "Установить";
    else if (_step == 5)
    {
        _nextLabel = _installer.Finished() && !_installer.Succeeded() ? "Повторить" : "Готово";
        _nextOk = _installer.Finished();
    }
    else if (_step == 6)
        _nextLabel = "Играть";
    else
        _nextLabel = "Далее";
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
                return Error("Укажите папку с клиентом 3.3.5a.");
            if (_client.version != "3.3.5.12340")
                return Error("Нужен клиент 3.3.5a (сборка 12340).");
            return Go(1);
        case 1:
        {
            fs::path root = PlacePath();
            if (root.empty())
                return Error("Выберите папку.");
            if (!Writable(root))
                return Error("В эту папку нельзя записывать: " + Utf8(root));
            BuildComponents();
            return Go(2);
        }
        case 2:
        {
            RefreshTotal();
            if (std::none_of(_comps.begin(), _comps.end(), [](CompRow const& c) { return c.on; }))
                return Error("Выберите хотя бы один компонент.");
            if (_comps[0].on && _comps[0].locked)
                return Error("Нет файлов setup\\sql.pak и configs.pak рядом с LonelyIce.exe.");
            int t = std::atoi(_threads.c_str());
            if (t < 1)
                return Error("Число потоков должно быть больше нуля.");
            Go(3);
            if (FreeBytes(PlacePath()) < (9ull << 30) && _comps[3].on)
                Error("Внимание: свободно меньше 9 ГБ, навигации может не хватить места.");
            return;
        }
        case 3:
            if (_comps[0].on && (!ValidLogin(_login) || _pass.empty()))
                return Error("Логин: латинские буквы и цифры, до 16 символов; пароль не пустой.");
            if (_realm.empty())
                return Error("Имя мира не может быть пустым.");
            return Go(4);
        case 4:
            if (_host.serverRunning && _host.serverRunning())
                return Error("Остановите сервер перед установкой.");
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
    o.shortcut = _lnk;
    _options = o;
    _log.clear();
    _installer.Start(o);
    Tick();
}

void Wizard::Tick()
{
    std::wstring picked;
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
            Inspect(picked);
            BuildPlaces();
        }
        else
        {
            _customPlace = picked;
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
        rows.push_back({ s.title, note, st, int(s.progress * 100.f + 0.5f) });
    }
    bool changed = rows.size() != _inst.size();
    for (std::size_t i = 0; !changed && i < rows.size(); ++i)
        changed = rows[i].note != _inst[i].note || rows[i].state != _inst[i].state || rows[i].pct != _inst[i].pct;
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
            _done.clear();
            fs::path root = _options.root;
            if (_options.db)
                _done.push_back({ "ok", "Базы собраны", Utf8(root / "db") });
            if (_options.maps || _options.vmaps || _options.mmaps)
                _done.push_back({ "ok", "Данные клиента", Utf8(root / "data") });
            if (_options.db && !_options.login.empty())
                _done.push_back({ "ok", "Аккаунт " + _options.login + (_options.gmLevel ? ", GM " + std::to_string(_options.gmLevel) : ""), "" });
            if (_options.client_prep && _options.realmlist)
                _done.push_back({ "ok", "realmlist.wtf → 127.0.0.1", _rlNote });
            if (_host.installed)
                _host.installed(_options);
        }
        RefreshFooter();
        Dirty();
    }
    if (_installer.IsRunning())
        _reported = false;
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
            me->_picked = Utf8ToWide(list[0]);
        }
        if (me->_host.wake)
            me->_host.wake();
    }, this, UiBackend::GetWindow(), start.c_str(), false);
}
