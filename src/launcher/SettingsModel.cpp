#include "SettingsModel.h"
#include "ConfFile.h"
#include "Lang.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
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
    // Group names and hints, labels, hints and option labels are Lang keys (settings.lang), translated when
    // shown; plugin settings bring their own text, which passes through the translation unchanged.
    _coreGroups = {
        { "realm", "set.group.realm", "set.group.realm.hint" },
        { "rates", "set.group.rates", "set.group.rates.hint" },
        { "diff", "set.group.diff", "set.group.diff.hint" },
        { "perf", "set.group.perf", "set.group.perf.hint" },
        { "launch", "set.group.launch", "set.group.launch.hint" },
    };

    _coreDefs = {
        { "realm", "set.realm.name", "realmlist.name", SetSource::Realm, {}, 't', "rst", "set.realm.name.hint", {}, "LonelyIce" },
        W("realm", "set.realm.game_type", "GameType", 's', "rst", "0", {}, { { "0", "set.opt.gametype.normal" }, { "1", "set.opt.gametype.pvp" },
            { "6", "set.opt.gametype.rp" }, { "8", "set.opt.gametype.rppvp" } }),
        W("realm", "set.realm.max_level", "MaxPlayerLevel", 'n', "rst", "80"),
        W("realm", "set.realm.start_level", "StartPlayerLevel", 'n', "rel", "1"),
        W("realm", "set.realm.start_level_dk", "StartHeroicPlayerLevel", 'n', "rel", "55"),
        W("realm", "set.realm.start_money", "StartPlayerMoney", 'n', "rel", "0"),
        W("realm", "set.realm.two_side_accounts", "AllowTwoSide.Accounts", 'b', "rel", "1"),
        W("realm", "set.realm.two_side_group", "AllowTwoSide.Interaction.Group", 'b', "rel", "0", "set.realm.two_side_group.hint"),

        Rate("set.rates.xp_kill", "Rate.XP.Kill"),
        Rate("set.rates.xp_quest", "Rate.XP.Quest"),
        Rate("set.rates.xp_explore", "Rate.XP.Explore"),
        Rate("set.rates.rest_ingame", "Rate.Rest.InGame"),
        Rate("set.rates.reputation", "Rate.Reputation.Gain"),
        Rate("set.rates.honor", "Rate.Honor"),
        Rate("set.rates.drop_money", "Rate.Drop.Money"),
        Rate("set.rates.drop_rare", "Rate.Drop.Item.Rare"),
        Rate("set.rates.drop_epic", "Rate.Drop.Item.Epic"),
        W("rates", "set.rates.skill_crafting", "SkillGain.Crafting", 'n', "rel", "1"),

        W("diff", "set.diff.normal_damage", "Rate.Creature.Normal.Damage", 'n', "rel", "1"),
        W("diff", "set.diff.normal_hp", "Rate.Creature.Normal.HP", 'n', "rel", "1"),
        W("diff", "set.diff.elite_damage", "Rate.Creature.Elite.Elite.Damage", 'n', "rel", "1"),
        W("diff", "set.diff.ignore_level", "Instance.IgnoreLevel", 'b', "rel", "0"),
        W("diff", "set.diff.ignore_raid", "Instance.IgnoreRaid", 'b', "rel", "0", "set.diff.ignore_raid.hint"),
        W("diff", "set.diff.quests_ignore_raid", "Quests.IgnoreRaid", 'b', "rel", "0"),

        W("perf", "set.perf.bind_local", "BindIP", 'b', "rst", "0.0.0.0", "set.perf.bind_local.hint"),
        W("perf", "set.perf.realm_port", "RealmServerPort", 'n', "rst", "3724"),
        W("perf", "set.perf.world_port", "WorldServerPort", 'n', "rst", "8085"),
        W("perf", "set.perf.soap", "SOAP.Enabled", 'b', "rst", "0"),
        W("perf", "set.perf.soap_port", "SOAP.Port", 'n', "rst", "7878"),
        W("perf", "set.perf.map_threads", "MapUpdate.Threads", 'n', "rst", "1", "set.perf.map_threads.hint"),
        W("perf", "set.perf.save_interval", "PlayerSaveInterval", 'n', "rel", "900000"),

        L("launch", "set.launch.ui_scale", "Launcher.UiScale", 's', "set.launch.ui_scale.hint",
            { { "100", "100 %" }, { "125", "125 %" }, { "150", "150 %" }, { "175", "175 %" }, { "200", "200 %" } }),
        L("launch", "set.launch.locale", "Launcher.Locale", 's', "set.launch.locale.hint"),
        L("launch", "set.launch.write_realmlist", "Launcher.WriteRealmlist", 'b', "set.launch.write_realmlist.hint"),
        L("launch", "set.launch.clear_wdb", "Launcher.ClearWdb", 'b', "set.launch.clear_wdb.hint"),
        L("launch", "set.launch.autostart", "Launcher.AutoStart", 'b'),
        L("launch", "set.launch.stop_with_game", "Launcher.StopWithGame", 'b'),
        L("launch", "set.launch.tray_on_close", "Launcher.TrayOnClose", 'b', "set.launch.tray_on_close.hint"),
        L("launch", "set.launch.backup_time", "Backup.Time", 't', "set.launch.backup_time.hint"),
        L("launch", "set.launch.backup_keep", "Backup.Keep", 'n'),
    };

    for (SetDef& d : _coreDefs)
    {
        if (d.key == "PlayerSaveInterval")
            d.conv = SetConv::MsToMin;
        else if (d.key == "BindIP")
            d.conv = SetConv::BindIp;
    }
}

void SettingsModel::Load(fs::path const& worldConf, std::vector<PluginManifest> const& plugins,
    LauncherSettings const& ls, std::vector<std::string> const& locales)
{
    _worldConf = worldConf;
    _errors.clear();

    // Core groups, then one group per plugin (by group name) before the network and launcher groups.
    _defs = _coreDefs;
    _groups.clear();
    std::vector<SetGroup> pluginGroups;
    for (PluginManifest const& p : plugins)
    {
        PluginSettings s = ReadPluginSettings(p);
        if (!s.error.empty())
            _errors.push_back(s.error);
        if (s.fields.empty() || p.configDist.empty())
            continue;
        pluginGroups.push_back({ "plugin:" + p.id, s.group, s.hint });
        for (PluginSetting const& f : s.fields)
        {
            static std::map<std::string, std::string> const apply = { { "now", "now" }, { "reload", "rel" }, { "restart", "rst" } };
            SetDef d{ "plugin:" + p.id, f.label, f.key, SetSource::Module, ConfigFileName(p), 't', apply.at(f.apply), f.hint, f.options, f.def.value_or("") };
            d.type = f.type == "bool" ? 'b' : f.type == "choice" ? 's' : f.type == "string" ? 't' : 'n';
            d.integer = f.type == "int";
            d.quoted = f.type == "string" || f.type == "choice";
            d.min = f.min;
            d.max = f.max;
            d.dist = p.configDist;
            _defs.push_back(std::move(d));
        }
    }
    std::sort(pluginGroups.begin(), pluginGroups.end(), [](SetGroup const& a, SetGroup const& b) { return a.name < b.name; });
    for (SetGroup const& g : _coreGroups)
    {
        if (g.id == "perf")
            _groups.insert(_groups.end(), pluginGroups.begin(), pluginGroups.end());
        _groups.push_back(g);
    }

    std::map<std::string, ConfFile> files;
    auto load = [&](fs::path const& p) -> ConfFile&
    {
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
                ConfFile& f = load(d.source == SetSource::World ? worldConf : worldConf.parent_path() / "modules" / d.file);
                std::optional<std::string> cur = f.IsLoaded() ? f.Get(d.key) : std::nullopt;
                // A key missing from the plugin's config (or the whole config) has the .dist's value.
                if (!cur && !d.dist.empty())
                    cur = load(d.dist).Get(d.key);
                if (!cur && d.source == SetSource::Module && d.dist.empty() && !f.IsLoaded())
                    continue;
                v = cur.value_or(d.def);
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
                    d.options.push_back({ "", "set.opt.locale.config_wtf" });
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
                if (!f.IsLoaded())
                {
                    // a plugin without its own config yet starts from its .dist
                    std::error_code ec;
                    if (!d.dist.empty() && !fs::exists(p, ec))
                    {
                        fs::create_directories(p.parent_path(), ec);
                        fs::copy_file(d.dist, p, ec);
                    }
                    if (!f.Load(p))
                    {
                        res.error = Tr("set.error.open", p.string());
                        return res;
                    }
                }
                std::string out = v.cur;
                if (d.conv == SetConv::BindIp)
                    out = v.cur == "1" ? "127.0.0.1" : "0.0.0.0";
                else if (d.conv == SetConv::MsToMin)
                    out = std::to_string(std::max(1, std::atoi(v.cur.c_str())) * 60000);
                else if (d.type == 'b')
                    out = BoolOut(v.cur == "1", f.Get(d.key));
                else if (d.type == 'n' && (d.integer || d.min || d.max))
                {
                    char* end = nullptr;
                    double n = std::strtod(v.cur.c_str(), &end);
                    if (end == v.cur.c_str())
                    {
                        res.error = Tr("set.error.number", Tr(d.label));
                        return res;
                    }
                    if (d.min)
                        n = std::max(n, *d.min);
                    if (d.max)
                        n = std::min(n, *d.max);
                    out = d.integer ? std::to_string(std::llround(n)) : out;
                    if (!d.integer && (d.min || d.max))
                    {
                        out = std::to_string(n);
                        out.erase(out.find_last_not_of('0') + 1);
                        if (out.back() == '.')
                            out.pop_back();
                    }
                    v.cur = out;
                }
                f.Set(d.key, out, d.quoted);
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
            res.error = Tr("set.error.write", path);
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
