#ifndef LONELYICE_SETTINGSMODEL_H
#define LONELYICE_SETTINGSMODEL_H

#include "LauncherSettings.h"
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace LonelyIce
{
    enum class SetSource { World, Module, Launcher, Realm };
    enum class SetConv { None, MsToMin, BindIp, BoolText };

    struct SetDef
    {
        std::string group;
        std::string label;
        std::string key;        // config key (or launcher key)
        SetSource source;
        std::string file;       // module conf file name for SetSource::Module
        char type;              // n = number, b = bool, s = select, t = text
        std::string apply;      // now, rel (.reload config), rst (restart)
        std::string hint;
        std::vector<std::pair<std::string, std::string>> options;
        std::string def;        // value when the key is missing
        SetConv conv = SetConv::None;
        bool rate = false;      // affected by the x1/x2/x5 presets
    };

    struct SetGroup
    {
        std::string id, name, hint;
    };

    struct SetValue
    {
        SetDef const* def;
        std::string orig, cur;  // UI form: numbers/text as typed, bools "1"/"0"
    };

    struct SaveResult
    {
        bool reload = false, restart = false, realmName = false;
        std::string error;
    };

    class SettingsModel
    {
    public:
        SettingsModel();

        std::vector<SetGroup> const& Groups() const { return _groups; }
        std::vector<SetValue>& Values() { return _values; }

        // client locales fill the options of Launcher.Locale
        void Load(std::filesystem::path const& worldConf, LauncherSettings const& ls, std::vector<std::string> const& locales);
        SaveResult Save(LauncherSettings& ls);
        void ApplyPreset(int rate);
        bool Changed(SetValue const& v) const { return v.orig != v.cur; }
        int ChangedCount(std::string const& group = {}) const;
        bool NeedsRestart() const;

    private:
        std::vector<SetDef> _defs;
        std::vector<SetGroup> _groups;
        std::vector<SetValue> _values;
        std::filesystem::path _worldConf;
    };
}

#endif
