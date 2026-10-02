#include "SettingsModel.h"
#include "ProfileConfig.h"
#include "Platform.h"
#include "Lang.h"
#include <chrono>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace LonelyIce::Platform
{
    std::filesystem::path ExePath()
    {
        return std::filesystem::absolute("LonelyIce.exe");
    }
    std::string PathToUtf8(std::filesystem::path const& path)
    {
        auto encoded = path.generic_u8string();
        return std::string(encoded.begin(), encoded.end());
    }
    std::filesystem::path Utf8ToPath(std::string const& path)
    {
        return std::filesystem::path(reinterpret_cast<char8_t const*>(path.data()),
                                     reinterpret_cast<char8_t const*>(path.data() + path.size()));
    }
} // namespace LonelyIce::Platform
namespace LonelyIce::Lang
{
    std::string Get(std::string_view key)
    {
        return std::string(key);
    }
    std::string Pick(std::map<std::string, std::string> const& texts)
    {
        return texts.empty() ? "" : texts.begin()->second;
    }
} // namespace LonelyIce::Lang
using namespace LonelyIce;
namespace fs = std::filesystem;
void Check(bool ok, char const* assertion)
{
    if (!ok)
        throw std::runtime_error(assertion);
}
void Write(fs::path const& path, std::string const& text)
{
    std::ofstream file(path);
    file << text;
}
SetValue& Find(SettingsModel& model, std::string const& key)
{
    for (auto& value : model.Values())
        if (value.def->key == key)
            return value;
    throw std::runtime_error("Missing test key: " + key);
}
int main()
{
    try
    {
        fs::path root =
            fs::temp_directory_path() /
            ("lonelyice-yaml-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);

        Write(root / "server.yaml", R"(server:
  settings:
    MaxPlayerLevel: 30
    Unlisted: retain
plugins:
  demo:
    version: "1.0"
    enabled: true
    settings:
      Flag: false
      Count: 2
      Fraction: 1.25
      Big: 1
      UnlistedPlugin: keep
unknown:
  nested: portable
  sequence: [7, "x # y", true]
)");
        Write(root / "local.yaml", R"(server:
  settings:
    MaxPlayerLevel: 40
plugins:
  demo:
    settings:
      Count: 3
      Fraction: 2.5
unknown:
  local: retain
)");
        Write(root / "ignored.conf", "MaxPlayerLevel = 999\n");
        Write(root / "demo.conf.dist",
              "Flag = 0\nCount = 2\nFraction = 1.25\nBig = 1\nText = template-text\nMode = a\n");
        Write(root / "settings.json", R"({"fields":[
{"key":"Flag","type":"bool","apply":"reload"},
{"key":"Count","type":"int","min":1,"max":10},
{"key":"Fraction","type":"float","min":0,"max":20},
{"key":"Big","type":"int"},
{"key":"Text","type":"string","default":""},
{"key":"Mode","type":"choice","options":[{"value":"a"},{"value":"b"}]}
]})");
        LauncherSettings settings;
        settings.file = root / "server.yaml";
        std::string error;
        Check(settings.Load(&error), error.c_str());
        PluginManifest plugin;
        plugin.id = "demo";
        plugin.name = "Demo";
        plugin.configDist = root / "demo.conf.dist";
        plugin.settings = root / "settings.json";
        SettingsModel model;
        model.Load(root / "ignored.conf", {plugin}, settings, {});
        Check(model.Errors().empty(), "schema/profile errors");
        Check(Find(model, "MaxPlayerLevel").cur == "40", "local override not loaded");
        Check(Find(model, "Fraction").cur == "2.5", "float scalar not loaded");
        Check(Find(model, "Text").cur.empty(), "empty explicit schema default overridden by template");
        Check(!model.NeedsRestart(), "clean settings show a restart warning");
        Find(model, "Flag").cur = "1";
        Check(model.NeedsRestart(), "reload setting fails to warn about GUI restart");
        Find(model, "Flag").cur = "0";
        Check(!model.NeedsRestart(), "reverted reload setting leaves restart warning");
        Find(model, "Count").cur = "11";
        Check(!model.Save(settings).error.empty(), "range not enforced");
        Check(model.ChangedCount() == 1, "range rejection erased dirty draft");
        Find(model, "Count").cur = "2x";
        Check(!model.Save(settings).error.empty(), "trailing numeric junk accepted");
        Find(model, "Count").cur = "3";
        Find(model, "Fraction").cur = "nan";
        Check(!model.Save(settings).error.empty(), "nonfinite float accepted");
        Find(model, "Fraction").cur = "2.5";
        Find(model, "MaxPlayerLevel").cur = "55";
        Find(model, "realmlist.name").cur = "Renamed realm";
        Find(model, "Flag").cur = "1";
        Find(model, "Count").cur = "8";
        Find(model, "Fraction").cur = "3.75";
        Find(model, "Big").cur = "9223372036854775807";
        Find(model, "Text").cur = "text ; # = preserved";
        Find(model, "Mode").cur = "b";
        Find(model, "Rate.Creature.Normal.Damage").cur = "0.75";
        auto result = model.Save(settings);
        Check(result.error.empty(), result.error.c_str());
        Check(model.ChangedCount() == 0, "successful save left dirty values");
        Check(settings.realmName == "Renamed realm" && settings.pendingRealmName == "Renamed realm",
              "portable realm name and pending GUI application diverged");
        ProfileConfig saved;
        Check(saved.Load(settings.file, error), error.c_str());
        using Layer = ProfileConfig::Layer;
        Check(saved.Get({"server", "settings", "MaxPlayerLevel"}, Layer::Profile)->get_value<int64_t>() == 30,
              "portable key overwritten by local edit");
        Check(saved.Get({"server", "settings", "MaxPlayerLevel"}, Layer::Local)->get_value<int64_t>() == 55,
              "local edit not routed locally");
        Check(saved.Get({"plugins", "demo", "settings", "Count"}, Layer::Profile)->get_value<int64_t>() == 2,
              "portable plugin key overwritten");
        Check(saved.Get({"plugins", "demo", "settings", "Count"}, Layer::Local)->get_value<int64_t>() == 8,
              "local plugin key not updated");
        Check(saved.Get({"plugins", "demo", "settings", "Flag"})->is_boolean(), "bool saved as string");
        Check(saved.Get({"plugins", "demo", "settings", "Fraction"})->is_float_number(), "float saved as string");
        Check(saved.Get({"plugins", "demo", "settings", "Big"})->get_value<int64_t>() == INT64_MAX,
              "integer precision lost");
        Check(saved.Get({"server", "settings", "Rate.Creature.Normal.Damage"})->get_value<double>() == 0.75,
              "fractional core rate rejected");
        Check(saved.String({"plugins", "demo", "settings", "Text"}) == "text ; # = preserved",
              "string lost during serialization");
        Check(saved.String({"plugins", "demo", "settings", "Mode"}) == "b", "choice not preserved as string");
        Check(saved.String({"server", "settings", "Unlisted"}) == "retain", "unknown core key lost");
        Check(saved.String({"plugins", "demo", "settings", "UnlistedPlugin"}) == "keep", "unknown plugin key lost");
        Check(saved.String({"plugins", "demo", "version"}) == "1.0", "package version lost");
        Check(saved.Boolean({"plugins", "demo", "enabled"}), "package enable metadata lost");
        Check(saved.String({"server", "realmName"}) == "Renamed realm", "portable realm name not saved");
        Check(saved.String({"unknown", "nested"}) == "portable" && saved.String({"unknown", "local"}) == "retain",
              "unknown mappings lost");
        Check(saved.Get({"unknown", "sequence"})->as_seq().at(1).get_value<std::string>() == "x # y",
              "unknown sequence strings corrupted");
        Find(model, "Count").cur = "9";
#ifdef _WIN32
        Check(SetFileAttributesW(settings.file.c_str(), FILE_ATTRIBUTE_READONLY), "could not inject write error");
        result = model.Save(settings);
        SetFileAttributesW(settings.file.c_str(), FILE_ATTRIBUTE_NORMAL);
        Check(!result.error.empty(), "read-only write error not surfaced");
        Check(model.ChangedCount() == 1, "write error erased dirty draft");
#else
        if (geteuid() != 0)
        {
            auto original = fs::status(root).permissions();
            fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
            result = model.Save(settings);
            fs::permissions(root, original, fs::perm_options::replace);
            Check(!result.error.empty(), "read-only directory write error not surfaced");
            Check(model.ChangedCount() == 1, "write error erased dirty draft");
        }
        else
            std::cout << "Permission-error injection skipped when running as root.\n";
#endif
        std::ifstream unchanged(root / "ignored.conf");
        std::string text;
        std::getline(unchanged, text);
        Check(text == "MaxPlayerLevel = 999", "generated conf treated as editable source");
        std::cout << "Fixture: " << root << '\n';
        std::cout << "PASS: layered typed values, schema defaults, unknown keys, strict validation, write failure and "
                     "dirty-state retention\n";
        return 0;
    }
    catch (std::exception const& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
