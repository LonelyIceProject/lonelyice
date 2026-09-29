#include "CommandCatalog.h"
#include "Lang.h"
#include <cstdlib>

using namespace LonelyIce;

// Group names, titles, descriptions, option labels and text argument defaults are Lang keys (commands.lang):
// the launcher translates them when shown, BuildCommand translates the defaults it falls back to.
std::vector<CmdGroup> const& LonelyIce::CommandCatalog()
{
    using O = std::vector<std::pair<std::string, std::string>>;
    static O const tele = { { "Dalaran", "cmd.loc.dalaran" }, { "Stormwind", "cmd.loc.stormwind" }, { "Orgrimmar", "cmd.loc.orgrimmar" },
        { "Ironforge", "cmd.loc.ironforge" }, { "Undercity", "cmd.loc.undercity" }, { "Shattrath", "cmd.loc.shattrath" } };

    static std::vector<CmdGroup> const catalog = {
        { "srv", "cmd.group.srv", {
            { "cmd.srv.info", "cmd.srv.info.desc", "server info", {} },
            { "cmd.srv.saveall", "cmd.srv.saveall.desc", "saveall", {} },
            { "cmd.srv.announce", "cmd.srv.announce.desc", "announce {text}", { { "text", 't', "cmd.srv.announce.text" } } },
            { "cmd.srv.notify", "cmd.srv.notify.desc", "notify {text}", { { "text", 't', "cmd.srv.notify.text" } } },
            { "cmd.srv.motd", "cmd.srv.motd.desc", "server set motd {text}", { { "text", 't', "cmd.srv.motd.text" } } },
            { "cmd.srv.reload_config", "cmd.srv.reload_config.desc", "reload config", {} },
            { "cmd.srv.restart", "cmd.srv.restart.desc", "server restart {sec}", { { "sec", 'n', "60" } }, true },
            { "cmd.srv.shutdown", "cmd.srv.shutdown.desc", "server shutdown {sec}", { { "sec", 'n', "10" } }, true },
            { "cmd.srv.shutdown_cancel", "", "server shutdown cancel", {} },
        } },
        { "chr", "cmd.group.chr", {
            { "cmd.chr.teleport", "cmd.chr.teleport.desc", "tele name {p} {loc}", { { "loc", 's', "Dalaran", tele } } },
            { "cmd.chr.level", "cmd.chr.level.desc", "character level {p} {lvl}", { { "lvl", 'n', "80" } } },
            { "cmd.chr.revive", "cmd.chr.revive.desc", "revive {p}", {} },
            { "cmd.chr.unstuck", "cmd.chr.unstuck.desc", "unstuck {p} {where}",
                { { "where", 's', "inn", { { "inn", "cmd.opt.unstuck.inn" }, { "graveyard", "cmd.opt.unstuck.graveyard" }, { "startzone", "cmd.opt.unstuck.startzone" } } } } },
            { "cmd.chr.send_money", "cmd.chr.send_money.desc", "send money {p} \"LonelyIce\" \"{subject}\" {copper}", { { "gold", 'n', "100" } } },
            { "cmd.chr.send_items", "cmd.chr.send_items.desc", "send items {p} \"LonelyIce\" \"{subject}\" {item}:{cnt}", { { "item", 'n', "49426" }, { "cnt", 'n', "1" } } },
            { "cmd.chr.reset_talents", "", "reset talents {p}", {} },
            { "cmd.chr.next_login", "cmd.chr.next_login.desc", "character {what} {p}",
                { { "what", 's', "customize", { { "customize", "cmd.opt.next_login.customize" }, { "rename", "cmd.opt.next_login.rename" },
                    { "changefaction", "cmd.opt.next_login.changefaction" }, { "changerace", "cmd.opt.next_login.changerace" } } } } },
            { "cmd.chr.pinfo", "cmd.chr.pinfo.desc", "pinfo {p}", {} },
            { "cmd.chr.kick", "", "kick {p}", {}, true },
        } },
        { "bots", "cmd.group.bots", {
            { "cmd.bots.stats", "cmd.bots.stats.desc", "rndbot stats", {} },
            { "cmd.bots.reload", "cmd.bots.reload.desc", "rndbot reload", {} },
            { "cmd.bots.update", "cmd.bots.update.desc", "rndbot update", {} },
            { "cmd.bots.reset", "cmd.bots.reset.desc", "rndbot reset", {}, true },
        } },
        { "acc", "cmd.group.acc", {
            { "cmd.acc.create", "", "account create {login} {pass}", { { "login", 't', "TESTER" }, { "pass", 't', "" } } },
            { "cmd.acc.gmlevel", "cmd.acc.gmlevel.desc", "account set gmlevel {login} {lvl} -1", { { "login", 't', "" }, { "lvl", 'n', "3" } } },
            { "cmd.acc.password", "", "account set password {login} {pass} {pass}", { { "login", 't', "" }, { "pass", 't', "" } } },
            { "cmd.acc.ban", "cmd.acc.ban.desc", "ban account {login} {time} {reason}", { { "login", 't', "" }, { "time", 't', "1d" }, { "reason", 't', "cmd.acc.ban.reason" } }, true },
            { "cmd.acc.unban", "", "unban account {login}", { { "login", 't', "" } } },
        } },
        { "world", "cmd.group.world", {
            { "cmd.world.events", "cmd.world.events.desc", "event activelist", {} },
            { "cmd.world.event_start", "cmd.world.event_start.desc", "event start {id}", { { "id", 'n', "1" } } },
            { "cmd.world.event_stop", "", "event stop {id}", { { "id", 'n', "1" } } },
            { "cmd.world.instances", "cmd.world.instances.desc", "instance stats", {} },
            { "cmd.world.reload_all", "cmd.world.reload_all.desc", "reload all", {}, true },
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

    std::string s = replaceAll(def.tmpl, "{p}", character.empty() ? Tr("cmd.no_character") : character);
    s = replaceAll(s, "{subject}", Tr("cmd.mail.subject"));
    for (std::size_t i = 0; i < def.args.size(); ++i)
    {
        std::string v = i < values.size() ? values[i] : Tr(def.args[i].def);
        if (def.args[i].name == "gold")
            s = replaceAll(s, "{copper}", std::to_string(std::atoll(v.c_str()) * 10000));
        else
            s = replaceAll(s, "{" + def.args[i].name + "}", v);
    }
    return s;
}
