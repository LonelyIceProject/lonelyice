#include "CommandCatalog.h"
#include <cstdlib>

using namespace LonelyIce;

std::vector<CmdGroup> const& LonelyIce::CommandCatalog()
{
    using O = std::vector<std::pair<std::string, std::string>>;
    static O const tele = { { "Dalaran", "Даларан" }, { "Stormwind", "Штормград" }, { "Orgrimmar", "Оргриммар" },
        { "Ironforge", "Стальгорн" }, { "Undercity", "Подгород" }, { "Shattrath", "Шаттрат" } };

    static std::vector<CmdGroup> const catalog = {
        { "srv", "Сервер", {
            { "Состояние сервера", "Игроки, боты, время работы, задержка", "server info", {} },
            { "Сохранить всех", "Записать всех персонажей в базу", "saveall", {} },
            { "Объявление в чат", "Сообщение всем в чат сервера", "announce {text}", { { "text", 't', "Рестарт через 5 минут" } } },
            { "Сообщение на экран", "Крупный текст по центру экрана", "notify {text}", { { "text", 't', "Привет из LonelyIce" } } },
            { "Сообщение дня", "Показывается при входе", "server set motd {text}", { { "text", 't', "Добро пожаловать" } } },
            { "Перечитать конфиг", "Применить настройки с меткой .reload", "reload config", {} },
            { "Перезапуск", "Через заданное число секунд", "server restart {sec}", { { "sec", 'n', "60" } }, true },
            { "Выключение", "Сервер сохранит всех и остановится", "server shutdown {sec}", { { "sec", 'n', "10" } }, true },
            { "Отменить выключение", "", "server shutdown cancel", {} },
        } },
        { "chr", "Персонаж", {
            { "Телепорт", "Перенести выбранного персонажа", "tele name {p} {loc}", { { "loc", 's', "Dalaran", tele } } },
            { "Уровень", "Установить уровень", "character level {p} {lvl}", { { "lvl", 'n', "80" } } },
            { "Воскресить", "Если застрял призраком", "revive {p}", {} },
            { "Вытащить из текстур", "В таверну, на кладбище или в стартовую зону", "unstuck {p} {where}",
                { { "where", 's', "inn", { { "inn", "таверна" }, { "graveyard", "кладбище" }, { "startzone", "стартовая зона" } } } } },
            { "Деньги почтой", "Сумма в золоте", "send money {p} \"LonelyIce\" \"Подарок\" {copper}", { { "gold", 'n', "100" } } },
            { "Предмет почтой", "ID предмета и количество", "send items {p} \"LonelyIce\" \"Подарок\" {item}:{cnt}", { { "item", 'n', "49426" }, { "cnt", 'n', "1" } } },
            { "Сбросить таланты", "", "reset talents {p}", {} },
            { "При следующем входе", "Смена внешности, имени, фракции или расы", "character {what} {p}",
                { { "what", 's', "customize", { { "customize", "внешность" }, { "rename", "имя" }, { "changefaction", "фракция" }, { "changerace", "раса" } } } } },
            { "Сведения", "Аккаунт, время в игре, деньги", "pinfo {p}", {} },
            { "Выгнать из игры", "", "kick {p}", {}, true },
        } },
        { "bots", "Боты", {
            { "Статистика ботов", "Сколько в мире, по уровням и занятиям", "rndbot stats", {} },
            { "Перечитать настройки ботов", "playerbots.conf без перезапуска", "rndbot reload", {} },
            { "Обновить ботов", "Внеочередной цикл обновления", "rndbot update", {} },
            { "Сбросить всех ботов", "Удаляет и пересоздаёт случайных ботов", "rndbot reset", {}, true },
        } },
        { "acc", "Аккаунты", {
            { "Создать аккаунт", "", "account create {login} {pass}", { { "login", 't', "TESTER" }, { "pass", 't', "" } } },
            { "Уровень доступа", "0 — игрок, 3 — администратор", "account set gmlevel {login} {lvl} -1", { { "login", 't', "" }, { "lvl", 'n', "3" } } },
            { "Сменить пароль", "", "account set password {login} {pass} {pass}", { { "login", 't', "" }, { "pass", 't', "" } } },
            { "Заблокировать", "Срок: 1d, 2h, -1 навсегда", "ban account {login} {time} {reason}", { { "login", 't', "" }, { "time", 't', "1d" }, { "reason", 't', "—" } }, true },
            { "Разблокировать", "", "unban account {login}", { { "login", 't', "" } } },
        } },
        { "world", "Мир", {
            { "Активные события", "Праздники и игровые события", "event activelist", {} },
            { "Запустить событие", "ID из game_event", "event start {id}", { { "id", 'n', "1" } } },
            { "Остановить событие", "", "event stop {id}", { { "id", 'n', "1" } } },
            { "Статистика подземелий", "Загруженные копии и привязки", "instance stats", {} },
            { "Перечитать таблицы мира", "После ручных правок базы мира", "reload all", {}, true },
        } },
    };
    return catalog;
}

std::string LonelyIce::BuildCommand(CmdDef const& def, std::vector<std::string> const& values, std::string const& character)
{
    auto replaceAll = [](std::string s, std::string const& what, std::string const& with)
    {
        for (std::size_t pos = 0; (pos = s.find(what, pos)) != std::string::npos; pos += with.size())
            s.replace(pos, what.size(), with);
        return s;
    };

    std::string s = replaceAll(def.tmpl, "{p}", character.empty() ? "<персонаж>" : character);
    for (std::size_t i = 0; i < def.args.size(); ++i)
    {
        std::string v = i < values.size() ? values[i] : def.args[i].def;
        if (def.args[i].name == "gold")
            s = replaceAll(s, "{copper}", std::to_string(std::atoll(v.c_str()) * 10000));
        else
            s = replaceAll(s, "{" + def.args[i].name + "}", v);
    }
    return s;
}
