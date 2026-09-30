// Plugin package manager on the command line:
//   LonelyIce --pkg list                           installed plugins
//   LonelyIce --pkg available [--locale <lang>]    packages of the index (with texts in that language)
//   LonelyIce --pkg install <id>[@<range>]...      with their dependencies
//   LonelyIce --pkg update [<id>...]               everything when no id is given
//   LonelyIce --pkg remove <id>
//   LonelyIce --pkg enable <id> | disable <id>
//   LonelyIce --pkg apply -c <worldserver.conf> [--client <game folder>]
//                                                      install / remove the plugins' patches in the databases
//                                                      (and the client) now instead of on the next server start
//   LonelyIce --pkg pack <plugin folder> [<out dir>]   <id>-<version>.zip and its index entry
// Options: --plugins <dir> (default: plugins next to the exe), --index <urls> (default: lonelyice.ini).

#include "Lang.h"
#include "LauncherSettings.h"
#include "PackageManager.h"
#include "Platform.h"
#include "TextUtil.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace LonelyIce;

int ServerMain(int argc, char** argv);

namespace
{
    void Print(std::string const& s)
    {
        fputs((s + "\n").c_str(), stdout);
    }

    int Fail(std::string const& s)
    {
        Print(Tr("pkg.cli.error", s));
        return 1;
    }

    fs::path ExeDir()
    {
        return Platform::ExePath().parent_path();
    }

    // Also seen by getenv (the server reads some settings that way).
    void SetEnvironment(char const* name, std::string const& value)
    {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    void PrintPlan(Packages::Plan const& plan)
    {
        for (Packages::Step const& s : plan.steps)
            Print("  " + s.package.id + " " + (s.from.empty() ? "" : s.from + " -> ") + s.package.version);
    }
}

int PkgMain(int argc, char** argv)
{
    Platform::UseParentConsole(false, true);

    fs::path const exeDir = ExeDir();
    LauncherSettings settings;
    settings.file = exeDir / "lonelyice.ini";
    settings.Load();

    fs::path pluginsDir = exeDir / "plugins";
    std::string index = settings.packageIndex, config, client, locale;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        std::string const a = argv[i];
        if (a == "--pkg")
            continue;
        if (a == "--plugins" && i + 1 < argc)
            pluginsDir = fs::u8path(argv[++i]);
        else if (a == "--index" && i + 1 < argc)
            index = argv[++i];
        else if ((a == "-c" || a == "--config") && i + 1 < argc)
            config = argv[++i];
        else if (a == "--client" && i + 1 < argc)
            client = argv[++i];
        else if (a == "--locale" && i + 1 < argc)
            locale = argv[++i];
        else
            args.push_back(a);
    }
    if (args.empty())
    {
        Print(Tr("pkg.cli.usage"));
        return 1;
    }

    std::string const cmd = args[0];
    std::vector<std::string> const rest(args.begin() + 1, args.end());
    Packages::Manager pm(pluginsDir);
    std::string error;

    if (cmd == "list")
    {
        for (Packages::Local const& l : pm.Installed())
            Print(l.enabled ? l.manifest.id + " " + l.manifest.version + "  " + l.manifest.name
                : Tr("pkg.cli.list_disabled", l.manifest.id, l.manifest.version, l.manifest.name));
        return 0;
    }

    if (cmd == "pack")
    {
        if (rest.empty())
            return Fail(Tr("pkg.cli.need_folder"));
        std::string entry;
        if (!Packages::Manager::Pack(fs::u8path(rest[0]), rest.size() > 1 ? fs::u8path(rest[1]) : fs::current_path(), entry, error))
            return Fail(error);
        Print(entry);
        return 0;
    }

    if (cmd == "apply")
    {
        if (config.empty())
            return Fail(Tr("pkg.cli.need_config"));
        if (!client.empty())
            SetEnvironment("LONELYICE_CLIENT", client);
        SetEnvironment("AC_PLUGINS_DIR", pluginsDir.string());
        std::vector<std::string> sargs = { argv[0], "--apply", "-c", config };
        std::vector<char*> ptrs;
        for (std::string& s : sargs)
            ptrs.push_back(s.data());
        return ServerMain(int(ptrs.size()), ptrs.data());
    }

    if (cmd == "remove" || cmd == "enable" || cmd == "disable")
    {
        if (rest.empty())
            return Fail(Tr("pkg.cli.need_id"));
        std::string const& id = rest[0];
        if (cmd != "enable")
        {
            std::vector<std::string> const deps = pm.Dependents(id);
            if (!deps.empty())
            {
                std::string list;
                for (std::string const& d : deps)
                    list += (list.empty() ? "" : ", ") + d;
                return Fail(Tr("pkg.cli.needed_by", id, list));
            }
        }
        bool const ok = cmd == "remove" ? pm.Remove(id, error) : pm.SetEnabled(id, cmd == "enable", error);
        if (!ok)
            return Fail(error);
        Print(Tr(cmd == "remove" ? "pkg.cli.removed" : cmd == "enable" ? "pkg.cli.enabled" : "pkg.cli.disabled", id));
        return 0;
    }

    if (!pm.LoadIndex(index, error))
        return Fail(Tr("pkg.cli.index_error", error));
    for (Packages::Source const& s : pm.Sources())
        if (!s.ok)
            Print(Tr("pkg.cli.index_skipped", s.location, s.error));

    if (cmd == "available")
    {
        std::map<std::string, std::string> installed;
        for (Packages::Local const& l : pm.Installed())
            installed[l.manifest.id] = l.manifest.version;
        for (Packages::Package const& p : pm.Available())
            if (locale.empty() || HasLocale(p.locales, locale))
                Print(installed.count(p.id) ? Tr("pkg.cli.available_installed", p.id, p.version, installed[p.id], p.name)
                : p.id + " " + p.version + "  " + p.name);
        return 0;
    }

    Packages::Plan plan;
    if (cmd == "install")
    {
        if (rest.empty())
            return Fail(Tr("pkg.cli.need_id"));
        std::map<std::string, std::string> requests;
        for (std::string const& r : rest)
        {
            std::size_t const at = r.find('@');
            requests[r.substr(0, at)] = at == std::string::npos ? "*" : r.substr(at + 1);
        }
        plan = pm.Resolve(requests, false);
    }
    else if (cmd == "update")
    {
        if (rest.empty())
            plan = pm.ResolveUpdates();
        else
        {
            std::map<std::string, std::string> requests;
            for (std::string const& id : rest)
                requests[id] = "*";
            plan = pm.Resolve(requests, true);
        }
    }
    else
        return Fail(Tr("pkg.cli.unknown_command", cmd));

    if (!plan.error.empty())
        return Fail(plan.error);
    if (plan.steps.empty())
    {
        Print(Tr("pkg.cli.nothing_to_install"));
        return 0;
    }
    Print(Tr("pkg.cli.will_install"));
    PrintPlan(plan);
    if (!pm.Install(plan, error, [](std::string const& line) { Print(line); }))
        return Fail(error);
    Print(Tr("pkg.cli.done"));
    return 0;
}
