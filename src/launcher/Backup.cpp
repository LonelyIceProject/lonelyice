#include "Backup.h"
#include "ConfFile.h"
#include "TextUtil.h"
#include <algorithm>
#include <ctime>
#include <sqlite3.h>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    // "sqlite:db/auth.sqlite;attach=..." -> db/auth.sqlite
    fs::path SqlitePath(std::string info, fs::path const& workDir)
    {
        if (info.rfind("sqlite:", 0) != 0)
            return {};
        info = info.substr(7);
        info = info.substr(0, info.find(';'));
        fs::path p = fs::u8path(info);
        return p.is_absolute() ? p : workDir / p;
    }

    std::string Stamp()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &tm);
        return buf;
    }
}

std::vector<DatabaseFile> LonelyIce::FindDatabases(fs::path const& worldConf, fs::path const& workDir)
{
    std::vector<DatabaseFile> out;
    ConfFile world;
    world.Load(worldConf);
    for (auto [name, key] : { std::pair{ "auth", "LoginDatabaseInfo" }, { "characters", "CharacterDatabaseInfo" }, { "world", "WorldDatabaseInfo" } })
        out.push_back({ name, SqlitePath(world.Get(key).value_or(""), workDir) });

    ConfFile bots;
    if (bots.Load(worldConf.parent_path() / "modules" / "playerbots.conf"))
        out.push_back({ "playerbots", SqlitePath(bots.Get("PlayerbotsDatabaseInfo").value_or(""), workDir) });
    return out;
}

BackupResult LonelyIce::BackupDatabases(std::vector<DatabaseFile> const& dbs, fs::path const& root, int keep)
{
    BackupResult res;
    res.dir = root / Stamp();
    std::error_code ec;
    fs::create_directories(res.dir, ec);
    if (ec)
    {
        res.message = "Не удалось создать папку " + WideToUtf8(res.dir.wstring());
        return res;
    }

    int copied = 0;
    for (DatabaseFile const& db : dbs)
    {
        if (db.name == "world" || db.path.empty() || !fs::exists(db.path))
            continue;

        fs::path target = res.dir / db.path.filename();
        sqlite3* src = nullptr;
        sqlite3* dst = nullptr;
        std::string srcPath = WideToUtf8(db.path.wstring()), dstPath = WideToUtf8(target.wstring());
        bool ok = sqlite3_open_v2(srcPath.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
            sqlite3_open_v2(dstPath.c_str(), &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK;
        if (ok)
        {
            sqlite3_busy_timeout(src, 5000);
            sqlite3_backup* b = sqlite3_backup_init(dst, "main", src, "main");
            ok = b != nullptr;
            if (b)
            {
                // All pages in one step: one read transaction, so a server writing meanwhile cannot force restarts.
                int rc;
                for (int tries = 0; (rc = sqlite3_backup_step(b, -1)) == SQLITE_BUSY || rc == SQLITE_LOCKED; ++tries)
                {
                    if (tries > 100)
                        break;
                    sqlite3_sleep(50);
                }
                ok = rc == SQLITE_DONE;
                sqlite3_backup_finish(b);
            }
        }
        if (!ok)
            res.message = db.name + ": " + (dst ? sqlite3_errmsg(dst) : src ? sqlite3_errmsg(src) : "не открывается");
        sqlite3_close(src);
        sqlite3_close(dst);
        if (!ok)
            return res;

        res.bytes += fs::file_size(target, ec);
        ++copied;
    }

    if (!copied)
    {
        fs::remove(res.dir, ec);
        res.message = "Нет файлов баз данных для копирования";
        return res;
    }

    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory())
            dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end());
    while (int(dirs.size()) > std::max(1, keep))
    {
        fs::remove_all(dirs.front(), ec);
        dirs.erase(dirs.begin());
    }

    res.ok = true;
    return res;
}
