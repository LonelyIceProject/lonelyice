#include "Backup.h"
#include "ConfFile.h"
#include "Lang.h"
#include "Platform.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <sqlite3.h>

namespace fs = std::filesystem;
using namespace LonelyIce;
using namespace LonelyIce::Backup;

namespace
{
    // "sqlite:db/auth.sqlite;attach=..." -> db/auth.sqlite
    fs::path SqlitePath(std::string info, fs::path const& workDir)
    {
        if (info.rfind("sqlite:", 0) != 0)
            return {};
        info = info.substr(7);
        info = info.substr(0, info.find(';'));
        fs::path p = Platform::Utf8ToPath(info);
        return p.is_absolute() ? p : workDir / p;
    }

    fs::path Suffixed(fs::path p, char const* suffix)
    {
        p += suffix;
        return p;
    }

    bool Exec(sqlite3* db, char const* sql)
    {
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    std::string QueryText(sqlite3* db, char const* sql)
    {
        sqlite3_stmt* st = nullptr;
        std::string v;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            if (unsigned char const* t = sqlite3_column_text(st, 0))
                v = reinterpret_cast<char const*>(t);
        sqlite3_finalize(st);
        return v;
    }

    uint32_t Be16(uint8_t const* p) { return uint32_t(p[0]) << 8 | p[1]; }
    uint32_t Be32(uint8_t const* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

    // ---- what changed on disk: size and time of the database file and its write-ahead log

    struct FileStamp
    {
        int64_t dbSize = -1, dbTime = 0, walSize = -1, walTime = 0;
        bool SameWal(FileStamp const& o) const { return walSize == o.walSize && walTime == o.walTime; }
        bool operator==(FileStamp const& o) const { return dbSize == o.dbSize && dbTime == o.dbTime && SameWal(o); }
    };

    FileStamp StampOf(fs::path const& db)
    {
        FileStamp s;
        std::error_code ec;
        auto one = [&](fs::path const& p, int64_t& size, int64_t& time)
        {
            uint64_t const n = fs::file_size(p, ec);
            if (ec)
                return;
            fs::file_time_type const t = fs::last_write_time(p, ec);
            if (ec)
                return;
            size = int64_t(n);
            time = int64_t(t.time_since_epoch().count());
        };
        one(db, s.dbSize, s.dbTime);
        one(Suffixed(db, "-wal"), s.walSize, s.walTime);
        return s;
    }

    struct StateLine
    {
        FileStamp stamp;
        std::time_t read = 0;   // when the pages were last read
    };

    // store/state: the file stamps each database had at its newest backup
    std::map<std::string, StateLine> ReadState(fs::path const& file)
    {
        std::map<std::string, StateLine> out;
        std::ifstream f(file);
        std::string name;
        StateLine l;
        int64_t read = 0;
        while (f >> name >> l.stamp.dbSize >> l.stamp.dbTime >> l.stamp.walSize >> l.stamp.walTime >> read)
        {
            l.read = std::time_t(read);
            out[name] = l;
        }
        return out;
    }

    void WriteState(fs::path const& file, std::map<std::string, StateLine> const& state)
    {
        std::ostringstream o;
        for (auto const& [name, l] : state)
            o << name << " " << l.stamp.dbSize << " " << l.stamp.dbTime << " " << l.stamp.walSize << " " << l.stamp.walTime
              << " " << int64_t(l.read) << "\n";
        std::ofstream(file, std::ios::trunc) << o.str();
    }

    // ---- a consistent view of a database file, read page by page while the server may write
    //
    // A read transaction pins a snapshot. In WAL mode the pages of that snapshot are in the database file once the
    // log is checkpointed up to it; a checkpoint never writes pages newer than what an open reader sees, so from then
    // until the transaction ends the file holds exactly the snapshot. Without WAL the reader's shared lock keeps
    // writers out of the file. When the log never gets checkpointed up to the snapshot (a server writing without
    // pause), SQLite's backup API copies the database into a temporary file, which is read instead.
    class Source
    {
    public:
        struct SchemaRow
        {
            std::string table;      // for an index: its table
            uint32_t root = 0;
        };

        ~Source() { Close(); }

        bool Open(fs::path const& path, std::string const& name, fs::path const& tempDir, std::string& error)
        {
            if (!OpenDirect(path, name, error))
            {
                Close();
                if (!error.empty() || !CopyToTemp(path, tempDir / (name + ".tmp")) || !OpenDirect(_temp, name, error))
                {
                    if (error.empty())
                        error = Tr("backup.error.busy", name);
                    return false;
                }
            }
            _pageSize = uint32_t(std::stoul("0" + QueryText(_r, "PRAGMA page_size")));
            _pages = uint32_t(std::stoul("0" + QueryText(_r, "PRAGMA page_count")));
            _file.open(_temp.empty() ? path : _temp, std::ios::binary);
            if (!_file || _pageSize < 512)
            {
                error = name + ": " + Tr("backup.error.open");
                return false;
            }
            return true;
        }

        uint32_t PageSize() const { return _pageSize; }
        uint32_t Pages() const { return _pages; }
        std::vector<SchemaRow> const& Schema() const { return _schema; }

        bool Read(uint32_t pgno, uint8_t* out)
        {
            // the page holding the lock bytes at 1 GiB is never used and cannot be read on Windows while locked
            if (pgno == 0x40000000u / _pageSize + 1)
            {
                std::fill(out, out + _pageSize, uint8_t(0));
                return true;
            }
            _file.clear();
            return _file.seekg(std::streamoff(uint64_t(pgno - 1) * _pageSize)) && _file.read(reinterpret_cast<char*>(out), _pageSize);
        }

        void Close()
        {
            _file.close();
            if (_r && _inTxn)
                Exec(_r, "COMMIT");
            _inTxn = false;
            sqlite3_close(_c);
            sqlite3_close(_r);
            _c = _r = nullptr;
            if (!_temp.empty())
            {
                std::error_code ec;
                for (char const* suffix : { "", "-wal", "-shm", "-journal" })
                    fs::remove(Suffixed(_temp, suffix), ec);
                _temp.clear();
            }
        }

    private:
        // False with an empty error: the log did not get checkpointed up to a snapshot in time.
        bool OpenDirect(fs::path const& path, std::string const& name, std::string& error)
        {
            std::string const p = Platform::PathToUtf8(path);
            if (sqlite3_open_v2(p.c_str(), &_r, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
            {
                error = name + ": " + Tr("backup.error.open");
                return false;
            }
            sqlite3_busy_timeout(_r, 5000);
            bool ok = false;
            for (int tries = 0; tries < 20 && !ok; ++tries)
            {
                if (tries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!Exec(_r, "BEGIN") || !ReadSchema())
                {
                    Exec(_r, "ROLLBACK");
                    continue;
                }
                if (QueryText(_r, "PRAGMA journal_mode") != "wal")
                {
                    ok = true;
                    break;
                }
                if (!_c)
                {
                    // a connection checkpoints only once it has read the database and found it in WAL mode
                    if (sqlite3_open_v2(p.c_str(), &_c, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
                        break;
                    sqlite3_busy_timeout(_c, 1000);
                    QueryText(_c, "PRAGMA journal_mode");
                }
                // on SQLITE_BUSY (another checkpoint runs) both counts are -1: nothing is known
                int log = -1, done = -1;
                int const rc = sqlite3_wal_checkpoint_v2(_c, "main", SQLITE_CHECKPOINT_PASSIVE, &log, &done);
                if (rc == SQLITE_OK && log >= 0 && log == done)
                    ok = true;
                else
                    Exec(_r, "COMMIT");     // the server wrote meanwhile or still reads older pages: again
            }
            _inTxn = ok;
            return ok;
        }

        bool CopyToTemp(fs::path const& path, fs::path const& temp)
        {
            std::error_code ec;
            fs::create_directories(temp.parent_path(), ec);
            _temp = temp;
            sqlite3* src = nullptr;
            sqlite3* dst = nullptr;
            std::string const s = Platform::PathToUtf8(path), d = Platform::PathToUtf8(temp);
            bool ok = sqlite3_open_v2(s.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK
                && sqlite3_open_v2(d.c_str(), &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK;
            if (ok)
            {
                sqlite3_busy_timeout(src, 5000);
                sqlite3_backup* b = sqlite3_backup_init(dst, "main", src, "main");
                ok = b != nullptr;
                if (b)
                {
                    // all pages in one step: one read transaction, writers cannot make it start over
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
            sqlite3_close(src);
            sqlite3_close(dst);
            return ok;
        }

        bool ReadSchema()
        {
            _schema.clear();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(_r, "SELECT tbl_name, rootpage FROM sqlite_schema WHERE rootpage > 0", -1, &st, nullptr) != SQLITE_OK)
                return false;
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
                if (unsigned char const* t = sqlite3_column_text(st, 0))
                    _schema.push_back({ reinterpret_cast<char const*>(t), uint32_t(sqlite3_column_int64(st, 1)) });
            sqlite3_finalize(st);
            return rc == SQLITE_DONE;
        }

        sqlite3* _r = nullptr;      // holds the read transaction
        sqlite3* _c = nullptr;      // checkpoints
        bool _inTxn = false;
        fs::path _temp;             // the copy read instead, when one was made
        std::ifstream _file;
        uint32_t _pageSize = 0, _pages = 0;
        std::vector<SchemaRow> _schema;
    };

    // ---- which table a page belongs to: the b-trees walked from their root pages (file format, section 1.6)

    class PageMap
    {
    public:
        explicit PageMap(uint32_t pages) : _info(std::size_t(pages) + 1) { }

        void Add(uint32_t pgno, uint8_t const* d, uint32_t pageSize)
        {
            if (pgno == 1)
                _usable = pageSize - d[20];
            Info& in = _info[pgno];
            in.next = Be32(d);                      // an overflow page starts with the next one's number
            uint32_t const h = pgno == 1 ? 100 : 0;
            uint8_t const type = d[h];
            if (type != 2 && type != 5 && type != 10 && type != 13)
                return;
            bool const interior = type == 2 || type == 5;
            uint32_t const cells = Be16(d + h + 3);
            uint32_t const ptrs = h + (interior ? 12 : 8);
            in.first = uint32_t(_edges.size());
            if (interior)
                _edges.push_back(Be32(d + h + 8));
            for (uint32_t c = 0; c < cells && ptrs + 2 * c + 2 <= pageSize; ++c)
            {
                uint32_t off = Be16(d + ptrs + 2 * c);
                if (off + 4 > pageSize)
                    break;
                if (interior)
                {
                    _edges.push_back(Be32(d + off));
                    off += 4;
                    if (type == 5)
                        continue;
                }
                uint64_t payload = 0;
                off += Varint(d + off, d + pageSize, payload);
                if (type == 13)
                {
                    uint64_t rowid;
                    off += Varint(d + off, d + pageSize, rowid);
                }
                if (uint32_t const local = Local(payload, type == 13); local < payload && off + local + 4 <= pageSize)
                    _edges.push_back(Be32(d + off + local) | Overflow);
            }
            in.count = uint32_t(_edges.size()) - in.first;
        }

        // Changed pages per table, most first; pages of no table (free list, page 1) are left out.
        std::vector<TableChange> Tables(std::vector<Source::SchemaRow> const& schema, std::vector<uint32_t> const& changed) const
        {
            std::vector<int> owner(_info.size(), -1);
            std::vector<std::string> names;
            std::map<std::string, int> ids;
            std::vector<uint32_t> stack;
            for (Source::SchemaRow const& row : schema)
            {
                auto [it, fresh] = ids.emplace(row.table, int(names.size()));
                if (fresh)
                    names.push_back(row.table);
                int const t = it->second;
                stack.assign(1, row.root);
                while (!stack.empty())
                {
                    uint32_t const pg = stack.back();
                    stack.pop_back();
                    if (pg == 0 || pg >= _info.size() || owner[pg] != -1)
                        continue;
                    owner[pg] = t;
                    Info const& in = _info[pg];
                    for (uint32_t e = in.first; e < in.first + in.count; ++e)
                    {
                        if (!(_edges[e] & Overflow))
                        {
                            stack.push_back(_edges[e]);
                            continue;
                        }
                        for (uint32_t o = _edges[e] & ~Overflow, steps = 0; o && o < _info.size() && owner[o] == -1 && steps < _info.size(); ++steps)
                        {
                            owner[o] = t;
                            o = _info[o].next;
                        }
                    }
                }
            }
            std::vector<uint32_t> count(names.size(), 0);
            for (uint32_t pg : changed)
                if (pg < owner.size() && owner[pg] >= 0)
                    ++count[owner[pg]];
            std::vector<TableChange> out;
            for (std::size_t i = 0; i < names.size(); ++i)
                if (count[i] && names[i].rfind("sqlite_", 0) != 0)
                    out.push_back({ names[i], count[i] });
            std::sort(out.begin(), out.end(), [](TableChange const& a, TableChange const& b)
                { return a.pages != b.pages ? a.pages > b.pages : a.table < b.table; });
            return out;
        }

    private:
        static constexpr uint32_t Overflow = 0x80000000u;

        struct Info
        {
            uint32_t next = 0, first = 0, count = 0;
        };

        static uint32_t Varint(uint8_t const* p, uint8_t const* end, uint64_t& v)
        {
            v = 0;
            for (uint32_t i = 0; i < 9 && p + i < end; ++i)
            {
                if (i == 8)
                {
                    v = (v << 8) | p[i];
                    return 9;
                }
                v = (v << 7) | (p[i] & 0x7f);
                if (!(p[i] & 0x80))
                    return i + 1;
            }
            return 9;
        }

        // Bytes of a payload kept on the b-tree page; the rest goes to overflow pages.
        uint32_t Local(uint64_t payload, bool tableLeaf) const
        {
            uint64_t const u = _usable;
            uint64_t const x = tableLeaf ? u - 35 : (u - 12) * 64 / 255 - 23;
            if (payload <= x)
                return uint32_t(payload);
            uint64_t const m = (u - 12) * 32 / 255 - 23;
            uint64_t const k = m + (payload - m) % (u - 4);
            return uint32_t(k <= x ? k : m);
        }

        std::vector<Info> _info;
        std::vector<uint32_t> _edges;       // child pages; overflow chains with the Overflow bit
        uint32_t _usable = 4096;
    };

    // Reads a database into the store: its manifest, compared page by page with the one of the previous backup.
    bool ReadDatabase(DatabaseFile const& db, Store& store, DbManifest const* old, DbManifest& m, std::string& error)
    {
        Source src;
        if (!src.Open(db.path, db.name, store.Dir() / "tmp", error))
            return false;
        m.name = db.name;
        m.file = Platform::PathToUtf8(db.path.filename());
        m.pageSize = src.PageSize();
        m.pages = src.Pages();

        std::vector<Loc> const oldLocs = old && old->pageSize == m.pageSize ? Store::Expand(*old) : std::vector<Loc>{};
        std::vector<Loc> locs(m.pages);
        std::vector<uint32_t> changed;
        std::vector<uint8_t> page(m.pageSize);
        PageMap map(m.pages);
        for (uint32_t pg = 1; pg <= m.pages; ++pg)
        {
            if (!src.Read(pg, page.data()))
            {
                error = Tr("backup.error.read", Platform::PathToUtf8(db.path));
                return false;
            }
            Hash128 const h = HashBytes(page.data(), page.size());
            if (pg <= oldLocs.size() && store.HashAt(oldLocs[pg - 1]) == h)
                locs[pg - 1] = oldLocs[pg - 1];
            else
            {
                if (!store.Put(h, page.data(), m.pageSize, locs[pg - 1], error))
                    return false;
                changed.push_back(pg);
            }
            map.Add(pg, page.data(), m.pageSize);
        }
        m.changed = uint32_t(changed.size());
        m.runs = Store::Compress(locs);
        if (old && !changed.empty())
            m.tables = map.Tables(src.Schema(), changed);
        return true;
    }

    BackupResult Snapshot(std::vector<DatabaseFile> const& dbs, fs::path const& root, std::string const& reason,
        Retention const* retention)
    {
        BackupResult res;
        Store store(root / "store");
        if (!store.Load(res.message))
            return res;

        std::time_t const now = std::time(nullptr);
        fs::path const stateFile = store.Dir() / "state";
        std::map<std::string, StateLine> const state = ReadState(stateFile);
        std::map<std::string, StateLine> newState;
        SnapshotFile const* prev = store.Snapshots().empty() ? nullptr : &store.Snapshots().back();
        auto oldOf = [&](std::string const& name) -> DbManifest const*
        {
            if (prev)
                for (DbManifest const& m : prev->dbs)
                    if (m.name == name)
                        return &m;
            return nullptr;
        };

        SnapshotFile snap;
        snap.time = now;
        snap.reason = reason;
        bool changed = !prev;
        for (DatabaseFile const& db : dbs)
        {
            std::error_code ec;
            if (db.path.empty() || !fs::exists(db.path, ec))
                continue;
            DbManifest const* old = oldOf(db.name);
            FileStamp const before = StampOf(db.path);
            auto st = state.find(db.name);
            if (old && st != state.end() && st->second.stamp == before && now - st->second.read < 24 * 3600)
            {
                DbManifest m = *old;
                m.changed = 0;
                m.tables.clear();
                snap.dbs.push_back(std::move(m));
                newState[db.name] = st->second;
                continue;
            }

            DbManifest m;
            if (!ReadDatabase(db, store, old, m, res.message))
            {
                store.Abort();
                return res;
            }
            // Written to while being read: the stamp from before, so the next backup reads it again.
            FileStamp const after = StampOf(db.path);
            newState[db.name] = { after.SameWal(before) ? after : before, now };
            if (!old || m.changed || m.pages != old->pages || m.pageSize != old->pageSize)
            {
                changed = true;
                res.changed.push_back(db.name);
            }
            snap.dbs.push_back(std::move(m));
        }
        if (snap.dbs.empty())
        {
            res.message = Tr("backup.error.nothing");
            return res;
        }
        if (prev && prev->dbs.size() != snap.dbs.size())
            changed = true;

        if (!changed)
        {
            store.Abort();
            WriteState(stateFile, newState);
            res.ok = true;
            res.id = prev->id;
            return res;
        }

        snap.added = store.PendingBytes();
        snap.id = store.NewId(now);
        if (!store.Commit(res.message) || !store.Save(snap, res.message))
            return res;
        WriteState(stateFile, newState);
        res.ok = res.created = true;
        res.id = snap.id;
        res.added = snap.added;
        if (retention && !store.Prune(*retention, now, res.message))
            res.message = Tr("backup.error.prune", res.message);
        else if (retention && retention->budget && store.Bytes() > retention->budget)
            res.message = Tr("backup.over_budget", (store.Bytes() + (1 << 20) - 1) >> 20);
        return res;
    }

    // Writes the pages of a database of a backup into a file, then lets SQLite check it.
    bool WriteDatabase(Store& store, DbManifest const& m, fs::path const& target, std::string& error)
    {
        {
            std::ofstream f(target, std::ios::binary | std::ios::trunc);
            std::vector<uint8_t> page;
            for (Loc const& l : Store::Expand(m))
            {
                if (!store.Read(l, page, error))
                    return false;
                if (!f.write(reinterpret_cast<char const*>(page.data()), std::streamsize(page.size())))
                {
                    error = Tr("backup.error.write", Platform::PathToUtf8(target));
                    return false;
                }
            }
            if (!f.flush())
            {
                error = Tr("backup.error.write", Platform::PathToUtf8(target));
                return false;
            }
        }
        sqlite3* db = nullptr;
        std::string const p = Platform::PathToUtf8(target);
        bool ok = sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK
            && QueryText(db, "PRAGMA quick_check") == "ok";
        sqlite3_close(db);
        if (!ok)
            error = Tr("backup.error.check", m.name);
        return ok;
    }

    // One backup, restore or export at a time per store, whichever process runs it (the launcher, --backup).
    std::unique_ptr<Platform::FileLock> LockStore(fs::path const& root, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(root / "store", ec);
        auto lock = std::make_unique<Platform::FileLock>(root / "store" / "lock");
        if (!lock->Held())
        {
            error = Tr("backup.error.locked");
            return nullptr;
        }
        return lock;
    }

    // Another connection has the database open: it cannot take it exclusively.
    bool InUse(fs::path const& path)
    {
        sqlite3* db = nullptr;
        std::string const p = Platform::PathToUtf8(path);
        bool free = sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK
            && Exec(db, "PRAGMA locking_mode = EXCLUSIVE") && Exec(db, "BEGIN EXCLUSIVE") && Exec(db, "COMMIT");
        sqlite3_close(db);
        return !free;
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

BackupResult LonelyIce::BackupDatabases(std::vector<DatabaseFile> const& dbs, fs::path const& root,
    std::string const& reason, Retention const& retention)
{
    BackupResult res;
    auto const lock = LockStore(root, res.message);
    return lock ? Snapshot(dbs, root, reason, &retention) : res;
}

std::vector<SnapshotFile> LonelyIce::ListBackups(fs::path const& root)
{
    std::vector<SnapshotFile> out = ReadSnapshots(root / "store");
    std::reverse(out.begin(), out.end());
    for (SnapshotFile& s : out)
        for (DbManifest& m : s.dbs)
            m.runs.clear();
    return out;
}

std::string LonelyIce::DescribeBackup(SnapshotFile const& s)
{
    std::string out;
    for (DbManifest const& m : s.dbs)
    {
        if (!m.changed)
            continue;
        std::string part = m.name;
        if (m.changed == m.pages && m.tables.empty())
            part = Tr("backup.db_full", m.name);
        else if (!m.tables.empty())
        {
            std::string tables;
            for (std::size_t i = 0; i < m.tables.size() && i < 3; ++i)
                tables += (i ? ", " : "") + m.tables[i].table;
            if (m.tables.size() > 3)
                tables += ", " + Tr("backup.more", m.tables.size() - 3);
            part += " (" + tables + ")";
        }
        out += (out.empty() ? "" : " · ") + part;
    }
    if (out.empty())
        out = Tr("backup.no_changes");
    if (s.reason == "restore")
        out = Tr("backup.before_restore", out);
    return out;
}

uint64_t LonelyIce::BackupStoreBytes(fs::path const& root)
{
    return StoreBytes(root / "store");
}

BackupResult LonelyIce::RestoreBackup(std::vector<DatabaseFile> const& dbs, fs::path const& root, std::string const& id)
{
    BackupResult res;
    auto const lock = LockStore(root, res.message);
    if (!lock)
        return res;
    std::vector<std::pair<DbManifest, fs::path>> targets;
    {
        Store store(root / "store");
        if (!store.Load(res.message))
            return res;
        SnapshotFile const* snap = store.Find(id);
        if (!snap)
        {
            res.message = Tr("backup.error.unknown", id);
            return res;
        }
        for (DbManifest const& m : snap->dbs)
            for (DatabaseFile const& db : dbs)
                if (db.name == m.name && !db.path.empty())
                    targets.emplace_back(m, db.path);
    }
    if (targets.empty())
    {
        res.message = Tr("backup.error.nothing");
        return res;
    }
    for (auto const& [m, path] : targets)
        if (std::error_code ec; fs::exists(path, ec) && InUse(path))
        {
            res.message = Tr("backup.error.in_use", m.name);
            return res;
        }

    // The state being replaced becomes a backup itself; no retention now, it could remove the one restored.
    BackupResult before = Snapshot(dbs, root, "restore", nullptr);
    if (!before.ok)
        return before;

    Store store(root / "store");
    if (!store.Load(res.message))
        return res;
    // databases whose pages are those of the backup already stay as they are
    if (SnapshotFile const* now = store.Find(before.id))
        targets.erase(std::remove_if(targets.begin(), targets.end(), [&](auto const& t)
        {
            for (DbManifest const& m : now->dbs)
                if (m.name == t.first.name && m.pageSize == t.first.pageSize && m.pages == t.first.pages)
                {
                    std::vector<Loc> const a = Store::Expand(m), b = Store::Expand(t.first);
                    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                        [&](Loc const& x, Loc const& y) { return store.HashAt(x) == store.HashAt(y); });
                }
            return false;
        }), targets.end());
    std::error_code ec;
    for (auto const& [m, path] : targets)
        if (!WriteDatabase(store, m, Suffixed(path, ".restore"), res.message))
        {
            for (auto const& t : targets)
                fs::remove(Suffixed(t.second, ".restore"), ec);
            return res;
        }

    // Keep originals and their journals until every database has been replaced.
    std::vector<fs::path> moved;
    std::vector<fs::path> replaced;
    auto rollback = [&]
    {
        for (fs::path const& p : replaced)
        {
            fs::remove(p, ec);
            if (ec)
                res.message += "\n" + Tr("backup.error.write", Platform::PathToUtf8(p)) + ": " + ec.message();
        }
        for (auto it = moved.rbegin(); it != moved.rend(); ++it)
        {
            fs::rename(Suffixed(*it, ".old"), *it, ec);
            if (ec)
                res.message += "\n" + Tr("backup.error.write", Platform::PathToUtf8(Suffixed(*it, ".old"))) + ": " + ec.message();
        }
        for (auto const& t : targets)
            fs::remove(Suffixed(t.second, ".restore"), ec);
    };
    for (auto const& [m, path] : targets)
        for (char const* suffix : { "", "-wal", "-shm", "-journal" })
        {
            fs::path const original = Suffixed(path, suffix);
            bool const exists = fs::exists(original, ec);
            if (!ec && !exists)
                continue;
            if (!ec)
            {
                // Never overwrite an original left by an earlier failed restore.
                bool const oldExists = fs::exists(Suffixed(original, ".old"), ec);
                if (!ec && oldExists)
                    ec = std::make_error_code(std::errc::file_exists);
            }
            if (!ec)
                fs::rename(original, Suffixed(original, ".old"), ec);
            if (ec)
            {
                res.message = Tr("backup.error.write", Platform::PathToUtf8(original)) + ": " + ec.message();
                rollback();
                return res;
            }
            moved.push_back(original);
        }
    for (auto const& [m, path] : targets)
    {
        fs::rename(Suffixed(path, ".restore"), path, ec);
        if (ec)
        {
            res.message = Tr("backup.error.write", Platform::PathToUtf8(path)) + ": " + ec.message();
            rollback();
            return res;
        }
        replaced.push_back(path);
    }
    for (fs::path const& p : moved)
        fs::remove(Suffixed(p, ".old"), ec);
    for (auto const& [m, path] : targets)
        res.changed.push_back(m.name);
    fs::remove(store.Dir() / "state", ec);
    res.ok = true;
    res.id = before.id;
    res.created = before.created;
    return res;
}

BackupResult LonelyIce::ExportBackup(fs::path const& root, std::string const& id)
{
    BackupResult res;
    auto const lock = LockStore(root, res.message);
    if (!lock)
        return res;
    Store store(root / "store");
    if (!store.Load(res.message))
        return res;
    SnapshotFile const* snap = store.Find(id);
    if (!snap)
    {
        res.message = Tr("backup.error.unknown", id);
        return res;
    }
    res.dir = root / "export" / id;
    std::error_code ec;
    fs::create_directories(res.dir, ec);
    if (ec)
    {
        res.message = Tr("backup.error.mkdir", Platform::PathToUtf8(res.dir));
        return res;
    }
    for (DbManifest const& m : snap->dbs)
    {
        fs::path const target = res.dir / Platform::Utf8ToPath(m.file);
        if (!WriteDatabase(store, m, target, res.message))
            return res;
        res.added += fs::file_size(target, ec);
        res.changed.push_back(m.name);
    }
    res.ok = true;
    res.id = id;
    return res;
}
