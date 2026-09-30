// Backups on the command line, with the launcher's settings (lonelyice.ini):
//   LonelyIce --backup [create]              a backup now; nothing is made when nothing changed
//   LonelyIce --backup list                  backups, newest first, with what changed in each
//   LonelyIce --backup restore <id>          puts a backup back; the server must be stopped
//   LonelyIce --backup export <id>           the database files of a backup into <root>/export/<id>
// Options: -c <worldserver.conf>, --root <server folder> (default: as the launcher).

#include "Backup.h"
#include "Lang.h"
#include "LauncherSettings.h"
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
    settings.file = exeDir / "lonelyice.ini";
    settings.Load();

    fs::path config, root;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        std::string const a = argv[i];
        if (a == "--backup")
            continue;
        if ((a == "-c" || a == "--config") && i + 1 < argc)
            config = Platform::Utf8ToPath(argv[++i]);
        else if (a == "--root" && i + 1 < argc)
            root = Platform::Utf8ToPath(argv[++i]);
        else
            args.push_back(a);
    }

    // The launcher's server folder and config (Launcher::Root, Launcher::ServerConfig).
    if (root.empty())
    {
        if (!config.empty() && config.parent_path().filename() == "configs")
            root = config.parent_path().parent_path();
        else
            root = settings.dataRoot.empty() ? exeDir : settings.dataRoot;
    }
    if (config.empty())
    {
        if (!settings.serverConfig.empty())
            config = settings.serverConfig;
        else if (!settings.dataRoot.empty())
            config = root / "configs" / "worldserver.conf";
        else
        {
            config = exeDir / "configs" / "worldserver.conf";
            if (std::error_code ec; fs::exists(exeDir / "configs-sqlite" / "worldserver.conf", ec))
                config = exeDir / "configs-sqlite" / "worldserver.conf";
        }
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
