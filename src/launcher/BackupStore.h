#ifndef LONELYICE_BACKUPSTORE_H
#define LONELYICE_BACKUPSTORE_H

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The backup store: database pages kept once however many snapshots contain them.
//
//   store/packs/00000001.pack    pages compressed one by one, a table of their hashes at the end
//   store/snapshots/<id>.snap    per database: its pages as runs of pack entries (text)
//
// A backup reads the pages of every database, stores the ones the store does not have yet in a new pack and
// writes a snapshot. Removing snapshots leaves pages nobody refers to; packs of those are deleted or compacted.
namespace LonelyIce::Backup
{
    struct Hash128
    {
        uint64_t lo = 0, hi = 0;
        bool operator==(Hash128 const& o) const { return lo == o.lo && hi == o.hi; }
        bool operator!=(Hash128 const& o) const { return !(*this == o); }
    };

    Hash128 HashBytes(void const* data, std::size_t size);

    struct Loc
    {
        uint32_t pack = 0, index = 0;
    };

    struct Run
    {
        uint32_t pack = 0, start = 0, count = 0;
    };

    struct TableChange
    {
        std::string table;
        uint32_t pages = 0;
    };

    struct DbManifest
    {
        std::string name;                   // auth, characters, world, playerbots
        std::string file;                   // file name of the database
        uint32_t pageSize = 0;
        uint32_t pages = 0;
        uint32_t changed = 0;               // pages that differ from the previous snapshot
        std::vector<TableChange> tables;    // tables of the changed pages, most pages first
        std::vector<Run> runs;
    };

    struct SnapshotFile
    {
        std::string id;                     // 2026-09-30_203712, the file name
        std::time_t time = 0;
        std::string reason;                 // manual, scheduled, restore
        uint64_t added = 0;                 // compressed bytes of the pages this snapshot brought
        std::vector<DbManifest> dbs;
    };

    // The snapshot files of a store, oldest first, as written (not checked against the packs); cheap.
    std::vector<SnapshotFile> ReadSnapshots(std::filesystem::path const& storeDir);
    // Bytes the files of a store take.
    uint64_t StoreBytes(std::filesystem::path const& storeDir);

    struct Retention
    {
        int days = 14;                      // one snapshot per day for that many days, then one per week
        uint64_t budget = 0;                // bytes the store may take, 0: no limit; the newest snapshot always stays
    };

    class Store
    {
    public:
        explicit Store(std::filesystem::path dir);
        ~Store();
        Store(Store const&) = delete;
        Store& operator=(Store const&) = delete;

        // Reads the pack tables and the snapshots; removes what an interrupted run left.
        bool Load(std::string& error);

        std::filesystem::path const& Dir() const { return _dir; }
        std::vector<SnapshotFile> const& Snapshots() const { return _snaps; }   // oldest first
        SnapshotFile const* Find(std::string const& id) const;
        uint64_t Bytes() const;

        // A page for the pack being written: an entry with the same hash when the store has one.
        bool Put(Hash128 const& h, uint8_t const* page, uint32_t size, Loc& loc, std::string& error);
        uint64_t PendingBytes() const { return _newBytes; }
        // Finishes the pack being written (nothing when no page was new).
        bool Commit(std::string& error);
        void Abort();

        Hash128 const& HashAt(Loc const& l) const;
        bool Read(Loc const& l, std::vector<uint8_t>& page, std::string& error);

        // A new snapshot id for that time (unique within the store).
        std::string NewId(std::time_t t) const;
        bool Save(SnapshotFile const& s, std::string& error);

        // Removes the snapshots the retention does not keep, then pages nobody refers to.
        bool Prune(Retention const& r, std::time_t now, std::string& error);

        // Page locations of a database of a snapshot, page 1 first.
        static std::vector<Loc> Expand(DbManifest const& m);
        static std::vector<Run> Compress(std::vector<Loc> const& locs);

    private:
        struct Entry
        {
            Hash128 hash;
            uint64_t offset = 0;
            uint32_t csize = 0, usize = 0;
        };
        struct Pack
        {
            std::vector<Entry> entries;
            uint64_t bytes = 0;             // file size
        };
        struct HashKey
        {
            std::size_t operator()(Hash128 const& h) const { return std::size_t(h.lo ^ (h.hi * 31)); }
        };

        std::filesystem::path PackPath(uint32_t id, bool temp = false) const;
        bool LoadPack(uint32_t id, std::string& error);
        bool BeginPack(std::string& error);
        bool Append(uint8_t const* data, uint32_t csize, uint32_t usize, Hash128 const& h, Loc& loc, std::string& error);
        bool Compact(std::vector<uint32_t> const& packs, std::string& error);
        std::map<uint32_t, std::vector<uint32_t>> References() const;
        void RebuildIndex();
        void RemoveSnapshot(std::size_t i);
        std::ifstream* Reader(uint32_t pack);

        std::filesystem::path _dir;
        std::map<uint32_t, Pack> _packs;
        std::unordered_map<Hash128, Loc, HashKey> _index;
        std::vector<SnapshotFile> _snaps;
        std::map<uint32_t, std::unique_ptr<std::ifstream>> _readers;

        // the pack being written: in _packs already, its file is <id>.pack.tmp until Commit
        uint32_t _newId = 0;
        std::ofstream _out;
        uint64_t _outPos = 0;
        uint64_t _newBytes = 0;
    };
}

#endif
