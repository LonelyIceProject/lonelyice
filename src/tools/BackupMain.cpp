// Backups on the command line, with the launcher's settings (server.yaml):
//   LonelyIce --backup [create]              a backup now; nothing is made when nothing changed
//   LonelyIce --backup list                  backups, newest first, with what changed in each
//   LonelyIce --backup restore <id>          puts a backup back; the server must be stopped
//   LonelyIce --backup export <id>           the database files of a backup into <root>/export/<id>
// Options: -c <worldserver.conf>, --root <server folder> (default: as the launcher).

#include "Backup.h"
#include "Lang.h"
#include "LauncherSettings.h"
#include "LauncherRuntime.h"
#include "PackageManager.h"
#include "Platform.h"
#include <cstdio>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    void Print(std::string const& s)
    {
        fputs((s + "\n").c_str(), stdout);
    }

    int Fail(std::string const& s)
    {
        Print(Tr("backup.cli.error", s));
        return 1;
    }

    std::string Megabytes(uint64_t b)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", double(b) / double(1 << 20));
        return Tr("backup.cli.mb", buf);
    }

    std::string Local(std::time_t t)
    {
        std::tm tm = Platform::LocalTime(t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        return buf;
    }
}

int BackupMain(int argc, char** argv)
{
    Platform::UseParentConsole(false, true);

    fs::path const exeDir = Platform::ExePath().parent_path();
    LauncherSettings settings;
    settings.file = exeDir / "server.yaml";

    fs::path config, root;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        std::string const a = argv[i];
        if (a == "--backup")
            continue;
        if ((a == "-c" || a == "--config") && i + 1 < argc)
            config = Platform::Utf8ToPath(argv[++i]);
        else if (a == "--settings" && i + 1 < argc)
            settings.file = Platform::Utf8ToPath(argv[++i]);
        else if (a == "--root" && i + 1 < argc)
            root = Platform::Utf8ToPath(argv[++i]);
        else
            args.push_back(a);
    }

    std::string error;
    if (!settings.Load(&error))
        return Fail(error);
    ResolveSettingsPaths(settings);
    LaunchContext context = CreateLaunchContext(settings);
    if (root.empty())
        root = context.root;
    if (config.empty())
    {
        settings.dataRoot = root;
        config = root / ".runtime" / "configs" / "worldserver.conf";
        Packages::Manager packages(context.plugins);
        // A running server owns the plugin lock; its existing adapter names the live databases.
        if (!packages.ServerRunning() && !GenerateRuntimeConfig(settings, error))
            return Fail(error);
        if (!fs::is_regular_file(config))
            return Fail("The configured server has no generated database configuration.");
    }

    std::string const cmd = args.empty() ? "create" : args[0];
    fs::path const backups = root / "backups";
    std::vector<DatabaseFile> const dbs = FindDatabases(config, root);
    Backup::Retention const retention{ settings.backupDays, uint64_t(settings.backupBudgetMb) << 20 };

    if (cmd == "create")
    {
        BackupResult const r = BackupDatabases(dbs, backups, "manual", retention);
        if (!r.ok)
            return Fail(r.message);
        if (!r.created)
            Print(Tr("backup.cli.unchanged", r.id));
        else
            Print(Tr("backup.cli.created", r.id, Megabytes(r.added)));
        if (!r.message.empty())
            Print(r.message);
        return 0;
    }
    if (cmd == "list")
    {
        for (Backup::SnapshotFile const& s : ListBackups(backups))
            Print(s.id + "  " + Local(s.time) + "  +" + Megabytes(s.added) + "  " + DescribeBackup(s));
        Print(Tr("backup.cli.total", Megabytes(BackupStoreBytes(backups))));
        return 0;
    }
    if ((cmd == "restore" || cmd == "export") && args.size() < 2)
        return Fail(Tr("backup.cli.need_id"));
    if (cmd == "restore")
    {
        BackupResult const r = RestoreBackup(dbs, backups, args[1]);
        if (!r.ok)
            return Fail(r.message);
        Print(Tr("backup.cli.restored", args[1], r.id));
        return 0;
    }
    if (cmd == "export")
    {
        BackupResult const r = ExportBackup(backups, args[1]);
        if (!r.ok)
            return Fail(r.message);
        Print(Tr("backup.cli.exported", Platform::PathToUtf8(r.dir)));
        return 0;
    }
    Print(Tr("backup.cli.usage"));
    return 1;
}
