#include "SettingsModel.h"
#include "ConfFile.h"
#include <algorithm>
#include <map>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    using Opts = std::vector<std::pair<std::string, std::string>>;

    SetDef W(std::string g, std::string label, std::string key, char type, std::string apply, std::string def, std::string hint = {}, Opts opts = {})
    {
        return { g, label, key, SetSource::World, {}, type, apply, hint, opts, def };
    }

    SetDef M(std::string g, std::string file, std::string label, std::string key, char type, std::string apply, std::string def, std::string hint = {}, Opts opts = {})
    {
        return { g, label, key, SetSource::Module, file, type, apply, hint, opts, def };
    }

    SetDef L(std::string g, std::string label, std::string key, char type, std::string hint = {}, Opts opts = {})
    {
        return { g, label, key, SetSource::Launcher, {}, type, "now", hint, opts, {} };
    }

    SetDef Rate(std::string label, std::string key)
    {
        SetDef d = W("rates", label, key, 'n', "rel", "1");
        d.rate = true;
        return d;
    }

    bool IsTrue(std::string v)
    {
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    // Writes a bool in the style the file already uses (1/0 or true/false).
    std::string BoolOut(bool on, std::optional<std::string> const& old)
    {
        std::string o = old.value_or("1");
        std::transform(o.begin(), o.end(), o.begin(), ::tolower);
        if (o == "true" || o == "false")
            return on ? "true" : "false";
        if (o == "yes" || o == "no")
            return on ? "yes" : "no";
        return on ? "1" : "0";
    }
}

SettingsModel::SettingsModel()
{
    _groups = {
        { "realm", "Мир", "Реалм, уровни и старт персонажа" },
        { "rates", "Множители", "Сколько опыта, денег и добычи вы получаете" },
        { "diff", "Сложность", "Для игры с малой группой ботов" },
        { "bots", "Боты", "Населённость мира, modules\\playerbots.conf" },
        { "mods", "Модули", "Модули, у которых есть переключатель в конфиге" },
        { "perf", "Сеть и ресурсы", "Порты, потоки и сохранение" },
        { "launch", "Лаунчер", "Параметры самого LonelyIce, lonelyice.ini" },
    };

    std::string const pb = "playerbots.conf";
    _defs = {
        { "realm", "Имя мира", "realmlist.name", SetSource::Realm, {}, 't', "rst", "Хранится в базе входа, видно в списке миров", {}, "LonelyIce" },
        W("realm", "Тип мира", "GameType", 's', "rst", "0", {}, { { "0", "Обычный" }, { "1", "PvP" }, { "6", "RP" }, { "8", "RP-PvP" } }),
        W("realm", "Максимальный уровень", "MaxPlayerLevel", 'n', "rst", "80"),
        W("realm", "Стартовый уровень", "StartPlayerLevel", 'n', "rel", "1"),
        W("realm", "Стартовый уровень рыцаря смерти", "StartHeroicPlayerLevel", 'n', "rel", "55"),
        W("realm", "Стартовые деньги, медь", "StartPlayerMoney", 'n', "rel", "0"),
        W("realm", "Альянс и Орда на одном аккаунте", "AllowTwoSide.Accounts", 'b', "rel", "1"),
        W("realm", "Общие группы Альянса и Орды", "AllowTwoSide.Interaction.Group", 'b', "rel", "0", "С ботами обеих фракций в одной группе"),

        Rate("Опыт за убийства", "Rate.XP.Kill"),
        Rate("Опыт за задания", "Rate.XP.Quest"),
        Rate("Опыт за исследование", "Rate.XP.Explore"),
        Rate("Отдых в игре", "Rate.Rest.InGame"),
        Rate("Репутация", "Rate.Reputation.Gain"),
        Rate("Очки чести", "Rate.Honor"),
        Rate("Деньги с монстров", "Rate.Drop.Money"),
        Rate("Редкие вещи (синие)", "Rate.Drop.Item.Rare"),
        Rate("Эпические вещи", "Rate.Drop.Item.Epic"),
        W("rates", "Рост навыков профессий", "SkillGain.Crafting", 'n', "rel", "1"),

        W("diff", "Урон обычных монстров", "Rate.Creature.Normal.Damage", 'n', "rel", "1"),
        W("diff", "Здоровье обычных монстров", "Rate.Creature.Normal.HP", 'n', "rel", "1"),
        W("diff", "Урон элитных монстров", "Rate.Creature.Elite.Elite.Damage", 'n', "rel", "1"),
        W("diff", "Вход в подземелья без требований уровня", "Instance.IgnoreLevel", 'b', "rel", "0"),
        W("diff", "Рейды без требования рейдовой группы", "Instance.IgnoreRaid", 'b', "rel", "0", "Позволяет идти в рейд группой из 5 ботов"),
        W("diff", "Рейдовые задания в обычной группе", "Quests.IgnoreRaid", 'b', "rel", "0"),

        M("bots", pb, "Боты включены", "AiPlayerbot.Enabled", 'b', "rst", "1"),
        M("bots", pb, "Случайных ботов, минимум", "AiPlayerbot.MinRandomBots", 'n', "rst", "50", "Около 5 МБ памяти и доля процессора на каждого бота"),
        M("bots", pb, "Случайных ботов, максимум", "AiPlayerbot.MaxRandomBots", 'n', "rst", "50"),
        M("bots", pb, "Уровень ботов, от", "AiPlayerbot.RandomBotMinLevel", 'n', "rst", "1"),
        M("bots", pb, "Уровень ботов, до", "AiPlayerbot.RandomBotMaxLevel", 'n', "rst", "80"),
        M("bots", pb, "Карты для ботов", "AiPlayerbot.RandomBotMaps", 's', "rst", "0,1,530,571", {},
            { { "0,1,530,571", "Все континенты" }, { "0,1", "Только классика" }, { "0,1,530", "Классика и Запределье" }, { "571", "Только Нордскол" } }),
        M("bots", pb, "Активны вдали от игрока, %", "AiPlayerbot.BotActiveAlone", 'n', "rst", "100", "Главный рычаг нагрузки на процессор"),
        M("bots", pb, "Личных ботов на игрока", "AiPlayerbot.MaxAddedBots", 'n', "rst", "40"),
        M("bots", pb, "Боты ходят в поиск подземелий", "AiPlayerbot.RandomBotJoinLfg", 'b', "rst", "1"),
        M("bots", pb, "Боты ходят на поля боя", "AiPlayerbot.RandomBotJoinBG", 'b', "rst", "1"),
        M("bots", pb, "Гильдии ботов", "AiPlayerbot.AllowGuildBots", 'b', "rst", "1"),

        M("mods", "mod_lonelyice_tactics.conf", "Тактики ботов", "Tactics.Enable", 'b', "rst", "1"),
        M("mods", "mod_lonelyice_citizens.conf", "Жители городов", "Citizens.Enable", 'b', "rst", "1"),
        M("mods", "mod_ahbot.conf", "Аукцион: бот продаёт", "AuctionHouseBot.EnableSeller", 'b', "rst", "0"),
        M("mods", "mod_ahbot.conf", "Аукцион: бот покупает", "AuctionHouseBot.EnableBuyer", 'b', "rst", "0"),
        M("mods", "mod_aoe_loot.conf", "Сбор добычи по площади", "AOELoot.Enable", 'b', "rst", "1"),
        M("mods", "transmog.conf", "Трансмогрификация", "Transmogrification.Enable", 'b', "rst", "1"),
        M("mods", "mod_learnspells.conf", "Изучение заклинаний при повышении уровня", "LearnSpells.Enable", 'b', "rst", "1"),
        M("mods", "mod_npc_beastmaster.conf", "Мастер питомцев", "BeastMaster.Enable", 'b', "rst", "1"),
        M("mods", "instance-reset.conf", "Сброс подземелий", "instanceReset.Enable", 'b', "rst", "1"),

        W("perf", "Только этот компьютер", "BindIP", 'b', "rst", "0.0.0.0", "Выключите, чтобы подключаться из локальной сети"),
        W("perf", "Порт входа", "RealmServerPort", 'n', "rst", "3724"),
        W("perf", "Порт мира", "WorldServerPort", 'n', "rst", "8085"),
        W("perf", "SOAP для инструментов", "SOAP.Enabled", 'b', "rst", "0"),
        W("perf", "Порт SOAP", "SOAP.Port", 'n', "rst", "7878"),
        W("perf", "Потоки обновления карт", "MapUpdate.Threads", 'n', "rst", "1", "Разумно: число ядер минус 4"),
        W("perf", "Сохранение персонажей, мин", "PlayerSaveInterval", 'n', "rel", "900000"),

        L("launch", "Масштаб интерфейса", "Launcher.UiScale", 's', "Поверх масштаба Windows; также Ctrl + колесо мыши",
            { { "100", "100 %" }, { "125", "125 %" }, { "150", "150 %" }, { "175", "175 %" }, { "200", "200 %" } }),
        L("launch", "Язык клиента", "Launcher.Locale", 's', "Записывается в WTF\\Config.wtf перед запуском игры"),
        L("launch", "Проверять realmlist перед запуском", "Launcher.WriteRealmlist", 'b', "Чужой адрес заменяется на 127.0.0.1, старый файл сохраняется как realmlist.wtf.bak"),
        L("launch", "Очищать кэш клиента (Cache\\WDB)", "Launcher.ClearWdb", 'b', "Нужно после правок базы мира"),
        L("launch", "Запускать сервер вместе с лаунчером", "Launcher.AutoStart", 'b'),
        L("launch", "Выход из игры останавливает сервер", "Launcher.StopWithGame", 'b'),
        L("launch", "Закрытие окна сворачивает в трей", "Launcher.TrayOnClose", 'b', "Сервер продолжает работать, LonelyIce остаётся в области уведомлений"),
        L("launch", "Резервная копия, время", "Backup.Time", 't', "ЧЧ:ММ, пусто — без расписания"),
        L("launch", "Хранить копий", "Backup.Keep", 'n'),
    };

    for (SetDef& d : _defs)
    {
        if (d.key == "PlayerSaveInterval")
            d.conv = SetConv::MsToMin;
        else if (d.key == "BindIP")
            d.conv = SetConv::BindIp;
    }
}

void SettingsModel::Load(fs::path const& worldConf, LauncherSettings const& ls, std::vector<std::string> const& locales)
{
    _worldConf = worldConf;
    std::map<std::string, ConfFile> files;
    auto conf = [&](SetDef const& d) -> ConfFile&
    {
        fs::path p = d.source == SetSource::World ? worldConf : worldConf.parent_path() / "modules" / d.file;
        ConfFile& f = files[p.string()];
        if (!f.IsLoaded())
            f.Load(p);
        return f;
    };

    _values.clear();
    for (SetDef& d : _defs)
    {
        std::string v;
        switch (d.source)
        {
            case SetSource::World:
            case SetSource::Module:
            {
                // Settings of a module that is not installed (no config file) are not shown.
                if (d.source == SetSource::Module && !conf(d).IsLoaded())
                    continue;
                v = conf(d).Get(d.key).value_or(d.def);
                if (d.type == 'b' && d.conv == SetConv::None)
                    v = IsTrue(v) ? "1" : "0";
                else if (d.conv == SetConv::BindIp)
                    v = v == "127.0.0.1" ? "1" : "0";
                else if (d.conv == SetConv::MsToMin)
                    v = std::to_string(std::max(1, std::atoi(v.c_str()) / 60000));
                break;
            }
            case SetSource::Realm:
                v = ls.pendingRealmName.empty() ? (ls.realmName.empty() ? d.def : ls.realmName) : ls.pendingRealmName;
                break;
            case SetSource::Launcher:
                if (d.key == "Launcher.Locale")
                {
                    d.options.clear();
                    d.options.push_back({ "", "Как в Config.wtf" });
                    for (std::string const& l : locales)
                        d.options.push_back({ l, l });
                    v = ls.locale;
                }
                else if (d.key == "Launcher.UiScale") v = std::to_string(ls.uiScale);
                else if (d.key == "Launcher.WriteRealmlist") v = ls.writeRealmlist ? "1" : "0";
                else if (d.key == "Launcher.ClearWdb") v = ls.clearWdb ? "1" : "0";
                else if (d.key == "Launcher.AutoStart") v = ls.autoStart ? "1" : "0";
                else if (d.key == "Launcher.StopWithGame") v = ls.stopWithGame ? "1" : "0";
                else if (d.key == "Launcher.TrayOnClose") v = ls.trayOnClose ? "1" : "0";
                else if (d.key == "Backup.Time") v = ls.backupTime;
                else if (d.key == "Backup.Keep") v = std::to_string(ls.backupKeep);
                break;
        }
        _values.push_back({ &d, v, v });
    }
}

SaveResult SettingsModel::Save(LauncherSettings& ls)
{
    SaveResult res;
    std::map<std::string, ConfFile> files;

    for (SetValue& v : _values)
    {
        if (!Changed(v))
            continue;
        SetDef const& d = *v.def;
        if (d.apply == "rel")
            res.reload = true;
        else if (d.apply == "rst")
            res.restart = true;

        switch (d.source)
        {
            case SetSource::World:
            case SetSource::Module:
            {
                fs::path p = d.source == SetSource::World ? _worldConf : _worldConf.parent_path() / "modules" / d.file;
                ConfFile& f = files[p.string()];
                if (!f.IsLoaded() && !f.Load(p))
                {
                    res.error = "Не удалось открыть " + p.string();
                    return res;
                }
                std::string out = v.cur;
                if (d.conv == SetConv::BindIp)
                    out = v.cur == "1" ? "127.0.0.1" : "0.0.0.0";
                else if (d.conv == SetConv::MsToMin)
                    out = std::to_string(std::max(1, std::atoi(v.cur.c_str())) * 60000);
                else if (d.type == 'b')
                    out = BoolOut(v.cur == "1", f.Get(d.key));
                f.Set(d.key, out);
                break;
            }
            case SetSource::Realm:
                ls.pendingRealmName = v.cur;
                res.realmName = true;
                break;
            case SetSource::Launcher:
                if (d.key == "Launcher.Locale") ls.locale = v.cur;
                else if (d.key == "Launcher.UiScale") ls.uiScale = std::clamp(std::atoi(v.cur.c_str()), 50, 300);
                else if (d.key == "Launcher.WriteRealmlist") ls.writeRealmlist = v.cur == "1";
                else if (d.key == "Launcher.ClearWdb") ls.clearWdb = v.cur == "1";
                else if (d.key == "Launcher.AutoStart") ls.autoStart = v.cur == "1";
                else if (d.key == "Launcher.StopWithGame") ls.stopWithGame = v.cur == "1";
                else if (d.key == "Launcher.TrayOnClose") ls.trayOnClose = v.cur == "1";
                else if (d.key == "Backup.Time") ls.backupTime = v.cur;
                else if (d.key == "Backup.Keep") ls.backupKeep = std::max(1, std::atoi(v.cur.c_str()));
                break;
        }
    }

    for (auto& [path, f] : files)
    {
        if (!f.Save())
        {
            res.error = "Не удалось записать " + path;
            return res;
        }
    }
    ls.Save();

    for (SetValue& v : _values)
        v.orig = v.cur;
    return res;
}

void SettingsModel::ApplyPreset(int rate)
{
    for (SetValue& v : _values)
        if (v.def->rate)
            v.cur = std::to_string(rate);
}

int SettingsModel::ChangedCount(std::string const& group) const
{
    int n = 0;
    for (SetValue const& v : _values)
        if (Changed(v) && (group.empty() || v.def->group == group))
            ++n;
    return n;
}

bool SettingsModel::NeedsRestart() const
{
    for (SetValue const& v : _values)
        if (Changed(v) && v.def->apply == "rst")
            return true;
    return false;
}
