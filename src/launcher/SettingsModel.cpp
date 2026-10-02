#include "SettingsModel.h"
#include "ConfFile.h"
#include "Lang.h"
#include "Platform.h"
#include "ProfileConfig.h"
#include <charconv>
#include <cerrno>
#include <cctype>
#include <limits>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    using Opts = std::vector<std::pair<std::string, std::string>>;

    SetDef W(std::string g, std::string label, std::string key, char type, std::string apply, std::string def,
             std::string hint = {}, Opts opts = {})
    {
        SetDef d{g,  label, key,  SetSource::World, {}, type, apply, hint, opts, def, SetConv::None, false, {},
                 {}, {},    false};
        d.integer = type == 'n' && key.rfind("Rate.", 0) != 0;
        return d;
    }

    SetDef L(std::string g, std::string label, std::string key, char type, std::string hint = {}, Opts opts = {})
    {
        return {g,  label, key,  SetSource::Launcher, {}, type, "now", hint, opts, {}, SetConv::None, false, {},
                {}, {},    false};
    }

    SetDef Rate(std::string label, std::string key)
    {
        SetDef d = W("rates", label, key, 'n', "rel", "1");
        d.rate = true;
        return d;
    }

    ProfileConfig::Path ValuePath(SetDef const& d)
    {
        return d.source == SetSource::Module ? ProfileConfig::Path{"plugins", d.file, "settings", d.key}
                                             : ProfileConfig::Path{"server", "settings", d.key};
    }

    std::optional<fkyaml::node> TypedValue(SetValue const& value)
    {
        auto const& d = *value.def;
        if (d.conv == SetConv::BindIp)
            return fkyaml::node(value.cur == "1" ? "127.0.0.1" : "0.0.0.0");
        if (d.type == 'b')
            return fkyaml::node(value.cur == "1");
        if (d.type != 'n')
            return fkyaml::node(value.cur);
        if (d.integer || d.conv == SetConv::MsToMin)
        {
            int64_t number = 0;
            auto begin = value.cur.data();
            if (!value.cur.empty() && value.cur.front() == '+')
                ++begin;
            auto result = std::from_chars(begin, value.cur.data() + value.cur.size(), number);
            if (result.ec != std::errc{} || result.ptr != value.cur.data() + value.cur.size())
                return std::nullopt;
            if (d.conv == SetConv::MsToMin)
                number *= 60000;
            return fkyaml::node(number);
        }
        char* end = nullptr;
        double number = std::strtod(value.cur.c_str(), &end);
        if (end != value.cur.c_str() + value.cur.size() || !std::isfinite(number))
            return std::nullopt;
        return fkyaml::node(number);
    }

} // namespace

SettingsModel::SettingsModel()
{
    // Group names and hints, labels, hints and option labels are Lang keys (settings.lang), translated when
    // shown; plugin settings bring their own text, which passes through the translation unchanged.
    _coreGroups = {
        {"realm", "set.group.realm", "set.group.realm.hint"},    {"rates", "set.group.rates", "set.group.rates.hint"},
        {"diff", "set.group.diff", "set.group.diff.hint"},       {"perf", "set.group.perf", "set.group.perf.hint"},
        {"launch", "set.group.launch", "set.group.launch.hint"},
    };

    _coreDefs = {
        {"realm",
         "set.realm.name",
         "realmlist.name",
         SetSource::Realm,
         {},
         't',
         "rst",
         "set.realm.name.hint",
         {},
         "LonelyIce",
         SetConv::None,
         false,
         {},
         {},
         {},
         false},
        W("realm", "set.realm.game_type", "GameType", 's', "rst", "0", {},
          {{"0", "set.opt.gametype.normal"},
           {"1", "set.opt.gametype.pvp"},
           {"6", "set.opt.gametype.rp"},
           {"8", "set.opt.gametype.rppvp"}}),
        W("realm", "set.realm.max_level", "MaxPlayerLevel", 'n', "rst", "80"),
        W("realm", "set.realm.start_level", "StartPlayerLevel", 'n', "rel", "1"),
        W("realm", "set.realm.start_level_dk", "StartHeroicPlayerLevel", 'n', "rel", "55"),
        W("realm", "set.realm.start_money", "StartPlayerMoney", 'n', "rel", "0"),
        W("realm", "set.realm.two_side_accounts", "AllowTwoSide.Accounts", 'b', "rel", "1"),
        W("realm", "set.realm.two_side_group", "AllowTwoSide.Interaction.Group", 'b', "rel", "0",
          "set.realm.two_side_group.hint"),

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
        W("perf", "set.perf.realm_port", "RealmServerPort", 'n', "rst", "3724", "set.perf.realm_port.hint"),
        W("perf", "set.perf.world_port", "WorldServerPort", 'n', "rst", "8085", "set.perf.world_port.hint"),
        W("perf", "set.perf.soap", "SOAP.Enabled", 'b', "rst", "0"),
        W("perf", "set.perf.soap_port", "SOAP.Port", 'n', "rst", "7878"),
        W("perf", "set.perf.map_threads", "MapUpdate.Threads", 'n', "rst", "1", "set.perf.map_threads.hint"),
        W("perf", "set.perf.save_interval", "PlayerSaveInterval", 'n', "rel", "900000"),

        L("launch", "set.launch.ui_scale", "Launcher.UiScale", 's', "set.launch.ui_scale.hint",
          {{"100", "100 %"}, {"125", "125 %"}, {"150", "150 %"}, {"175", "175 %"}, {"200", "200 %"}}),
        L("launch", "set.launch.locale", "Launcher.Locale", 's', "set.launch.locale.hint"),
        L("launch", "set.launch.write_realmlist", "Launcher.WriteRealmlist", 'b', "set.launch.write_realmlist.hint"),
        L("launch", "set.launch.clear_wdb", "Launcher.ClearWdb", 'b', "set.launch.clear_wdb.hint"),
        L("launch", "set.launch.autostart", "Launcher.AutoStart", 'b'),
        L("launch", "set.launch.stop_with_game", "Launcher.StopWithGame", 'b'),
        L("launch", "set.launch.tray_on_close", "Launcher.TrayOnClose", 'b', "set.launch.tray_on_close.hint"),
        L("launch", "set.launch.backup_schedule", "Backup.Schedule", 's', "set.launch.backup_schedule.hint",
          {{"off", "set.opt.backup.off"},
           {"1", "set.opt.backup.1"},
           {"3", "set.opt.backup.3"},
           {"6", "set.opt.backup.6"},
           {"12", "set.opt.backup.12"},
           {"daily", "set.opt.backup.daily"}}),
        L("launch", "set.launch.backup_time", "Backup.Time", 't', "set.launch.backup_time.hint"),
        L("launch", "set.launch.backup_days", "Backup.Days", 'n', "set.launch.backup_days.hint"),
        L("launch", "set.launch.backup_budget", "Backup.Budget", 'n', "set.launch.backup_budget.hint"),
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
    (void)worldConf; // Kept for existing graphical-launcher callers; generated conf files are never user values.
    _profilePath = ls.file.empty() ? Platform::ExePath().parent_path() / "server.yaml" : ls.file;
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
        pluginGroups.push_back({"plugin:" + p.id, s.group, s.hint});
        for (PluginSetting const& f : s.fields)
        {
            static std::map<std::string, std::string> const apply = {
                {"now", "now"}, {"reload", "rel"}, {"restart", "rst"}};
            SetDef d{"plugin:" + p.id,
                     f.label,
                     f.key,
                     SetSource::Module,
                     p.id,
                     't',
                     apply.at(f.apply),
                     f.hint,
                     f.options,
                     f.def.value_or(""),
                     SetConv::None,
                     false,
                     {},
                     {},
                     {},
                     false};
            d.type = f.type == "bool" ? 'b' : f.type == "choice" ? 's' : f.type == "string" ? 't' : 'n';
            d.integer = f.type == "int";
            d.min = f.min;
            d.max = f.max;
            d.dist = f.def ? fs::path{} : p.configDist;
            _defs.push_back(std::move(d));
        }
    }
    std::sort(pluginGroups.begin(), pluginGroups.end(),
              [](SetGroup const& a, SetGroup const& b) { return a.name < b.name; });
    for (SetGroup const& g : _coreGroups)
    {
        if (g.id == "perf")
            _groups.insert(_groups.end(), pluginGroups.begin(), pluginGroups.end());
        _groups.push_back(g);
    }

    ProfileConfig profile;
    std::string profileError;
    bool loaded = profile.Load(_profilePath, profileError);
    if (!loaded)
        _errors.push_back(profileError);
    std::map<std::string, ConfFile> defaults;
    auto readDefault = [&](SetDef const& d) -> std::optional<std::string>
    {
        if (d.dist.empty())
            return std::nullopt;
        ConfFile& file = defaults[d.dist.string()];
        if (!file.IsLoaded())
            file.Load(d.dist);
        return file.Get(d.key);
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
            std::optional<std::string> cur;
            if (loaded)
            {
                try
                {
                    if (auto node = profile.Get(ValuePath(d)))
                        cur = ProfileConfig::Scalar(*node);
                }
                catch (std::exception const& error)
                {
                    _errors.push_back(d.key + ": " + error.what());
                }
            }
            if (d.def.empty())
                d.def = readDefault(d).value_or("");
            v = cur.value_or(d.def);
            if (d.type == 'b' && d.conv == SetConv::None)
            {
                std::string normalized = v;
                std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on")
                    v = "1";
                else if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off")
                    v = "0";
                else
                    _errors.push_back(Tr("tui.invalid_setting", Tr(d.label)));
            }
            else if (d.conv == SetConv::BindIp)
                v = v == "127.0.0.1" ? "1" : "0";
            else if (d.conv == SetConv::MsToMin)
            {
                int64_t milliseconds = 0;
                auto parsed = std::from_chars(v.data(), v.data() + v.size(), milliseconds);
                if (parsed.ec == std::errc{} && parsed.ptr == v.data() + v.size() && milliseconds >= 0)
                    v = std::to_string(std::max<int64_t>(1, milliseconds / 60000));
                else
                    _errors.push_back(Tr("tui.invalid_setting", Tr(d.label)));
            }
            break;
        }
        case SetSource::Realm:
            v = ls.realmName.empty() ? d.def : ls.realmName;
            break;
        case SetSource::Launcher:
            if (d.key == "Launcher.Locale")
            {
                d.options.clear();
                d.options.push_back({"", "set.opt.locale.config_wtf"});
                for (std::string const& l : locales)
                    d.options.push_back({l, l});
                v = ls.locale;
            }
            else if (d.key == "Launcher.UiScale")
                v = std::to_string(ls.uiScale);
            else if (d.key == "Launcher.WriteRealmlist")
                v = ls.writeRealmlist ? "1" : "0";
            else if (d.key == "Launcher.ClearWdb")
                v = ls.clearWdb ? "1" : "0";
            else if (d.key == "Launcher.AutoStart")
                v = ls.autoStart ? "1" : "0";
            else if (d.key == "Launcher.StopWithGame")
                v = ls.stopWithGame ? "1" : "0";
            else if (d.key == "Launcher.TrayOnClose")
                v = ls.trayOnClose ? "1" : "0";
            else if (d.key == "Backup.Schedule")
                v = ls.backupSchedule;
            else if (d.key == "Backup.Time")
                v = ls.backupTime;
            else if (d.key == "Backup.Days")
                v = std::to_string(ls.backupDays);
            else if (d.key == "Backup.Budget")
                v = std::to_string(ls.backupBudgetMb);
            break;
        }
        _values.push_back({&d, v, v});
    }
}

SaveResult SettingsModel::Save(LauncherSettings& ls)
{
    SaveResult res;
    ProfileConfig profile;
    std::string error;
    fs::path path = ls.file.empty() ? _profilePath : ls.file;
    if (!profile.Load(path, error))
    {
        res.error = error;
        return res;
    }
    LauncherSettings staged = ls;
    try
    {
        for (SetValue const& v : _values)
        {
            if (!Changed(v))
                continue;
            if (auto invalid = ValidateValue(v); !invalid.empty())
            {
                res.error = invalid;
                return res;
            }
            SetDef const& d = *v.def;
            res.reload |= d.apply == "rel";
            res.restart |= d.apply == "rst";
            switch (d.source)
            {
            case SetSource::World:
            case SetSource::Module:
            {
                auto node = TypedValue(v);
                if (!node)
                {
                    res.error = Tr("tui.invalid_setting", Tr(d.label));
                    return res;
                }
                profile.SetEffective(ValuePath(d), std::move(*node));
                break;
            }
            case SetSource::Realm:
                staged.realmName = v.cur;
                staged.pendingRealmName = v.cur;
                res.realmName = true;
                break;
            case SetSource::Launcher:
                if (d.key == "Launcher.Locale")
                    staged.locale = v.cur;
                else if (d.key == "Launcher.UiScale")
                    staged.uiScale = std::stoi(v.cur);
                else if (d.key == "Launcher.WriteRealmlist")
                    staged.writeRealmlist = v.cur == "1";
                else if (d.key == "Launcher.ClearWdb")
                    staged.clearWdb = v.cur == "1";
                else if (d.key == "Launcher.AutoStart")
                    staged.autoStart = v.cur == "1";
                else if (d.key == "Launcher.StopWithGame")
                    staged.stopWithGame = v.cur == "1";
                else if (d.key == "Launcher.TrayOnClose")
                    staged.trayOnClose = v.cur == "1";
                else if (d.key == "Backup.Schedule")
                    staged.backupSchedule = v.cur;
                else if (d.key == "Backup.Time")
                    staged.backupTime = v.cur;
                else if (d.key == "Backup.Days")
                    staged.backupDays = std::stoi(v.cur);
                else if (d.key == "Backup.Budget")
                    staged.backupBudgetMb = std::stoi(v.cur);
                break;
            }
        }
        staged.ApplyToProfile(profile);
        if (!profile.Save(error))
        {
            res.error = error;
            return res;
        }
    }
    catch (std::exception const& exception)
    {
        res.error = exception.what();
        return res;
    }
    // The profile and local override writes succeeded. Failed writes leave the model and launcher draft intact.
    ls = std::move(staged);
    for (SetValue& value : _values)
        value.orig = value.cur;
    return res;
}

std::string SettingsModel::ValidateValue(SetValue const& value)
{
    auto const& d = *value.def;
    auto invalid = [&] { return Tr("tui.invalid_setting", Tr(d.label)); };
    auto const& input = value.cur;
    if (input.find_first_of("\r\n") != std::string::npos || input.find('\0') != std::string::npos)
        return invalid();
    if (d.type == 'b' && input != "0" && input != "1")
        return invalid();
    if (d.type == 's' &&
        std::none_of(d.options.begin(), d.options.end(), [&](auto const& option) { return option.first == input; }))
        return invalid();
    if (d.type == 'n')
    {
        if (input.empty() || std::isspace(static_cast<unsigned char>(input.front())))
            return invalid();
        char* end = nullptr;
        errno = 0;
        double number = std::strtod(input.c_str(), &end);
        if (end != input.c_str() + input.size() || errno == ERANGE || !std::isfinite(number) ||
            (d.min && number < *d.min) || (d.max && number > *d.max))
            return invalid();
        if (d.integer || d.source == SetSource::Launcher || d.conv == SetConv::MsToMin)
        {
            int64_t integer = 0;
            auto begin = input.data();
            if (input.front() == '+')
                ++begin;
            auto parsed = std::from_chars(begin, input.data() + input.size(), integer);
            if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
                return invalid();
            if (d.source == SetSource::Launcher && (integer < 0 || integer > std::numeric_limits<int>::max()))
                return invalid();
            if (d.conv == SetConv::MsToMin && (integer < 1 || integer > std::numeric_limits<int64_t>::max() / 60000))
                return invalid();
            if (d.key == "Backup.Days" && integer < 1)
                return invalid();
        }
    }
    if (d.key == "Backup.Time")
    {
        if (input.size() != 5 || input[2] != ':' || !std::isdigit(static_cast<unsigned char>(input[0])) ||
            !std::isdigit(static_cast<unsigned char>(input[1])) ||
            !std::isdigit(static_cast<unsigned char>(input[3])) ||
            !std::isdigit(static_cast<unsigned char>(input[4])) || input.substr(0, 2) > "23" ||
            input.substr(3, 2) > "59")
            return invalid();
    }
    return {};
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
        if (Changed(v) && (v.def->apply == "rst" || v.def->apply == "rel"))
            return true;
    return false;
}
