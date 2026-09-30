#include "BackupStore.h"
#include "Lang.h"
#include "Platform.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_set>
#include <miniz.h>

namespace fs = std::filesystem;
using namespace LonelyIce;
using namespace LonelyIce::Backup;

namespace
{
    constexpr char PackMagic[8] = { 'L', 'I', 'P', 'A', 'C', 'K', '1', '\n' };
    constexpr char PackEnd[8] = { 'L', 'I', 'P', 'K', 'E', 'N', 'D', '\n' };
    constexpr uint32_t EntrySize = 32;      // hash 16, offset 8, compressed 4, size 4
    constexpr uint32_t TrailerSize = 16;    // count 4, reserved 4, PackEnd

    uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

    uint64_t Fmix(uint64_t k)
    {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ull;
        k ^= k >> 33;
        return k;
    }

    uint64_t Load64(uint8_t const* p)
    {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | p[i];
        return v;
    }

    void Put32(uint8_t* p, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            p[i] = uint8_t(v >> (8 * i));
    }

    void Put64(uint8_t* p, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            p[i] = uint8_t(v >> (8 * i));
    }

    uint32_t Get32(uint8_t const* p)
    {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    std::string Local(std::time_t t, char const* fmt)
    {
        std::tm tm = Platform::LocalTime(t);
        char buf[32];
        std::strftime(buf, sizeof(buf), fmt, &tm);
        return buf;
    }

    // Days since 1970-01-01 in local time: the day and week buckets of the retention.
    long LocalDay(std::time_t t)
    {
        std::tm tm = Platform::LocalTime(t);
        int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
        y -= m <= 2;
        long era = (y >= 0 ? y : y - 399) / 400;
        long yoe = y - era * 400;
        long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    bool WriteFileAtomic(fs::path const& path, std::string const& text)
    {
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.write(text.data(), std::streamsize(text.size())) || !f.flush())
                return false;
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        return !ec;
    }

    std::string RestOf(std::istringstream& in)
    {
        std::string s;
        std::getline(in >> std::ws, s);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
        return s;
    }

    std::string SerializeSnapshot(SnapshotFile const& s)
    {
        std::ostringstream o;
        o << "lonelyice-snapshot 1\n";
        o << "time " << int64_t(s.time) << "\n";
        o << "reason " << s.reason << "\n";
        o << "added " << s.added << "\n";
        for (DbManifest const& m : s.dbs)
        {
            o << "db " << m.name << " " << m.pageSize << " " << m.pages << " " << m.changed << " " << m.file << "\n";
            for (TableChange const& t : m.tables)
                o << "table " << t.pages << " " << t.table << "\n";
            for (Run const& r : m.runs)
                o << "run " << r.pack << " " << r.start << " " << r.count << "\n";
        }
        o << "end\n";
        return o.str();
    }

    bool ParseSnapshot(fs::path const& path, SnapshotFile& s)
    {
        std::ifstream f(path, std::ios::binary);
        std::string line;
        if (!std::getline(f, line) || line.rfind("lonelyice-snapshot 1", 0) != 0)
            return false;
        s.id = Platform::PathToUtf8(path.stem());
        bool ended = false;
        while (std::getline(f, line))
        {
            std::istringstream in(line);
            std::string key;
            in >> key;
            if (key == "time")
            {
                int64_t t = 0;
                in >> t;
                s.time = std::time_t(t);
            }
            else if (key == "reason")
                in >> s.reason;
            else if (key == "added")
                in >> s.added;
            else if (key == "db")
            {
                DbManifest m;
                in >> m.name >> m.pageSize >> m.pages >> m.changed;
                m.file = RestOf(in);
                s.dbs.push_back(std::move(m));
            }
            else if (key == "table" && !s.dbs.empty())
            {
                TableChange t;
                in >> t.pages;
                t.table = RestOf(in);
                s.dbs.back().tables.push_back(std::move(t));
            }
            else if (key == "run" && !s.dbs.empty())
            {
                Run r;
                in >> r.pack >> r.start >> r.count;
                s.dbs.back().runs.push_back(r);
            }
            else if (key == "end")
                ended = true;
        }
        return ended;   // a snapshot cut short is not used
    }
}

// MurmurHash3 x64 128 (public domain, Austin Appleby); pages are compared by it, content from a trusted source.
Hash128 Backup::HashBytes(void const* data, std::size_t size)
{
    uint8_t const* p = static_cast<uint8_t const*>(data);
    uint64_t h1 = 0, h2 = 0;
    uint64_t const c1 = 0x87c37b91114253d5ull, c2 = 0x4cf5ad432745937full;
    std::size_t const blocks = size / 16;
    for (std::size_t i = 0; i < blocks; ++i)
    {
        uint64_t k1 = Load64(p + i * 16), k2 = Load64(p + i * 16 + 8);
        k1 *= c1; k1 = Rotl(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = Rotl(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
        k2 *= c2; k2 = Rotl(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = Rotl(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }
    uint8_t const* tail = p + blocks * 16;
    uint64_t k1 = 0, k2 = 0;
    std::size_t const rest = size & 15;
    for (std::size_t i = rest; i > 8; --i)
        k2 ^= uint64_t(tail[i - 1]) << ((i - 9) * 8);
    if (rest > 8)
    {
        k2 *= c2; k2 = Rotl(k2, 33); k2 *= c1; h2 ^= k2;
    }
    for (std::size_t i = std::min<std::size_t>(rest, 8); i > 0; --i)
        k1 ^= uint64_t(tail[i - 1]) << ((i - 1) * 8);
    if (rest > 0)
    {
        k1 *= c1; k1 = Rotl(k1, 31); k1 *= c2; h1 ^= k1;
    }
    h1 ^= size;
    h2 ^= size;
    h1 += h2;
    h2 += h1;
    h1 = Fmix(h1);
    h2 = Fmix(h2);
    h1 += h2;
    h2 += h1;
    return { h1, h2 };
}

std::vector<SnapshotFile> Backup::ReadSnapshots(fs::path const& storeDir)
{
    std::vector<SnapshotFile> out;
    std::error_code ec;
    for (fs::directory_iterator it(storeDir / "snapshots", ec), end; !ec && it != end; it.increment(ec))
        if (SnapshotFile s; it->path().extension() == ".snap" && ParseSnapshot(it->path(), s))
            out.push_back(std::move(s));
    std::sort(out.begin(), out.end(), [](SnapshotFile const& a, SnapshotFile const& b)
        { return a.time != b.time ? a.time < b.time : a.id < b.id; });
    return out;
}

uint64_t Backup::StoreBytes(fs::path const& storeDir)
{
    uint64_t total = 0;
    std::error_code ec;
    for (char const* sub : { "packs", "snapshots" })
        for (fs::directory_iterator it(storeDir / sub, ec), end; !ec && it != end; it.increment(ec))
            total += it->file_size(ec);
    return total;
}

Store::Store(fs::path dir) : _dir(std::move(dir)) { }

Store::~Store()
{
    Abort();
}

fs::path Store::PackPath(uint32_t id, bool temp) const
{
    char name[32];
    std::snprintf(name, sizeof(name), temp ? "%08u.pack.tmp" : "%08u.pack", id);
    return _dir / "packs" / name;
}

bool Store::Load(std::string& error)
{
    Abort();
    _packs.clear();
    _index.clear();
    _snaps.clear();
    _readers.clear();

    std::error_code ec;
    fs::remove_all(_dir / "tmp", ec);      // database copies of an interrupted backup
    for (char const* sub : { "packs", "snapshots" })
    {
        fs::create_directories(_dir / sub, ec);
        if (ec)
        {
            error = Tr("backup.error.mkdir", Platform::PathToUtf8(_dir / sub));
            return false;
        }
        for (fs::directory_iterator it(_dir / sub, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".tmp")
                fs::remove(it->path(), ec);
    }

    for (fs::directory_iterator it(_dir / "packs", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().extension() != ".pack")
            continue;
        std::string const stem = Platform::PathToUtf8(it->path().stem());
        if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::string packError;
        LoadPack(uint32_t(std::stoul(stem)), packError);    // a damaged pack is left out; its snapshots too
    }

    for (fs::directory_iterator it(_dir / "snapshots", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().extension() != ".snap")
            continue;
        SnapshotFile s;
        if (!ParseSnapshot(it->path(), s))
            continue;
        bool valid = true;
        for (DbManifest const& m : s.dbs)
            for (Run const& r : m.runs)
            {
                auto p = _packs.find(r.pack);
                if (p == _packs.end() || uint64_t(r.start) + r.count > p->second.entries.size())
                    valid = false;
            }
        if (valid)
            _snaps.push_back(std::move(s));
    }
    std::sort(_snaps.begin(), _snaps.end(), [](SnapshotFile const& a, SnapshotFile const& b)
        { return a.time != b.time ? a.time < b.time : a.id < b.id; });
    RebuildIndex();
    return true;
}

bool Store::LoadPack(uint32_t id, std::string& error)
{
    fs::path const path = PackPath(id);
    std::ifstream f(path, std::ios::binary);
    std::error_code ec;
    uint64_t const size = fs::file_size(path, ec);
    uint8_t trailer[TrailerSize];
    if (!f || ec || size < sizeof(PackMagic) + TrailerSize || !f.seekg(std::streamoff(size - TrailerSize))
        || !f.read(reinterpret_cast<char*>(trailer), TrailerSize) || std::memcmp(trailer + 8, PackEnd, 8) != 0)
    {
        error = Tr("backup.error.damaged", Platform::PathToUtf8(path));
        return false;
    }
    uint32_t const count = Get32(trailer);
    uint64_t const table = uint64_t(count) * EntrySize;
    if (table + TrailerSize + sizeof(PackMagic) > size)
    {
        error = Tr("backup.error.damaged", Platform::PathToUtf8(path));
        return false;
    }
    uint64_t const dataEnd = size - TrailerSize - table;
    std::vector<uint8_t> raw(static_cast<std::size_t>(table));
    if (!f.seekg(std::streamoff(dataEnd)) || (table && !f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(table))))
    {
        error = Tr("backup.error.damaged", Platform::PathToUtf8(path));
        return false;
    }
    Pack pack;
    pack.bytes = size;
    pack.entries.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t const* e = raw.data() + std::size_t(i) * EntrySize;
        Entry& en = pack.entries[i];
        en.hash = { Load64(e), Load64(e + 8) };
        en.offset = Load64(e + 16);
        en.csize = Get32(e + 24);
        en.usize = Get32(e + 28);
        if (en.offset < sizeof(PackMagic) || en.offset + en.csize > dataEnd || en.csize > en.usize)
        {
            error = Tr("backup.error.damaged", Platform::PathToUtf8(path));
            return false;
        }
    }
    _packs[id] = std::move(pack);
    return true;
}

void Store::RebuildIndex()
{
    _index.clear();
    for (auto const& [id, pack] : _packs)
        for (uint32_t i = 0; i < pack.entries.size(); ++i)
            _index.emplace(pack.entries[i].hash, Loc{ id, i });
}

SnapshotFile const* Store::Find(std::string const& id) const
{
    for (SnapshotFile const& s : _snaps)
        if (s.id == id)
            return &s;
    return nullptr;
}

uint64_t Store::Bytes() const
{
    return StoreBytes(_dir);
}

bool Store::BeginPack(std::string& error)
{
    uint32_t id = _packs.empty() ? 1 : _packs.rbegin()->first + 1;
    std::error_code ec;
    while (fs::exists(PackPath(id), ec))
        ++id;
    _out.open(PackPath(id, true), std::ios::binary | std::ios::trunc);
    if (!_out.write(PackMagic, sizeof(PackMagic)))
    {
        _out.close();
        error = Tr("backup.error.write", Platform::PathToUtf8(PackPath(id, true)));
        return false;
    }
    _newId = id;
    _outPos = sizeof(PackMagic);
    _packs[id] = Pack{};
    return true;
}

bool Store::Append(uint8_t const* data, uint32_t csize, uint32_t usize, Hash128 const& h, Loc& loc, std::string& error)
{
    if (!_out.is_open() && !BeginPack(error))
        return false;
    if (!_out.write(reinterpret_cast<char const*>(data), csize))
    {
        error = Tr("backup.error.write", Platform::PathToUtf8(PackPath(_newId, true)));
        return false;
    }
    Pack& pack = _packs[_newId];
    loc = { _newId, uint32_t(pack.entries.size()) };
    pack.entries.push_back({ h, _outPos, csize, usize });
    _outPos += csize;
    _newBytes += csize;
    return true;
}

bool Store::Put(Hash128 const& h, uint8_t const* page, uint32_t size, Loc& loc, std::string& error)
{
    if (auto it = _index.find(h); it != _index.end())
    {
        loc = it->second;
        return true;
    }
    mz_ulong csize = mz_compressBound(size);
    std::vector<uint8_t> buf(csize);
    bool const packed = mz_compress2(buf.data(), &csize, page, size, 6) == MZ_OK && csize < size;
    // stored as is when compression does not help (compressed size == size tells on reading)
    if (!(packed ? Append(buf.data(), uint32_t(csize), size, h, loc, error) : Append(page, size, size, h, loc, error)))
        return false;
    _index.emplace(h, loc);
    return true;
}

bool Store::Commit(std::string& error)
{
    if (!_out.is_open())
        return true;
    Pack& pack = _packs[_newId];
    std::vector<uint8_t> table(pack.entries.size() * EntrySize + TrailerSize);
    for (std::size_t i = 0; i < pack.entries.size(); ++i)
    {
        Entry const& e = pack.entries[i];
        uint8_t* p = table.data() + i * EntrySize;
        Put64(p, e.hash.lo);
        Put64(p + 8, e.hash.hi);
        Put64(p + 16, e.offset);
        Put32(p + 24, e.csize);
        Put32(p + 28, e.usize);
    }
    uint8_t* t = table.data() + pack.entries.size() * EntrySize;
    Put32(t, uint32_t(pack.entries.size()));
    Put32(t + 4, 0);
    std::memcpy(t + 8, PackEnd, 8);
    bool const ok = bool(_out.write(reinterpret_cast<char const*>(table.data()), std::streamsize(table.size())) && _out.flush());
    _out.close();
    std::error_code ec;
    if (ok)
        fs::rename(PackPath(_newId, true), PackPath(_newId), ec);
    if (!ok || ec)
    {
        error = Tr("backup.error.write", Platform::PathToUtf8(PackPath(_newId)));
        Abort();
        return false;
    }
    pack.bytes = _outPos + table.size();
    _newId = 0;
    _newBytes = 0;
    return true;
}

void Store::Abort()
{
    if (!_newId)
        return;
    _out.close();
    std::error_code ec;
    fs::remove(PackPath(_newId, true), ec);
    if (auto it = _packs.find(_newId); it != _packs.end())
    {
        for (Entry const& e : it->second.entries)
            if (auto i = _index.find(e.hash); i != _index.end() && i->second.pack == _newId)
                _index.erase(i);
        _packs.erase(it);
    }
    _newId = 0;
    _newBytes = 0;
}

Hash128 const& Store::HashAt(Loc const& l) const
{
    return _packs.at(l.pack).entries.at(l.index).hash;
}

std::ifstream* Store::Reader(uint32_t pack)
{
    std::unique_ptr<std::ifstream>& r = _readers[pack];
    if (!r)
        r = std::make_unique<std::ifstream>(PackPath(pack), std::ios::binary);
    return *r ? r.get() : nullptr;
}

bool Store::Read(Loc const& l, std::vector<uint8_t>& page, std::string& error)
{
    auto p = _packs.find(l.pack);
    std::ifstream* f = Reader(l.pack);
    if (p == _packs.end() || l.index >= p->second.entries.size() || !f)
    {
        error = Tr("backup.error.damaged", Platform::PathToUtf8(PackPath(l.pack)));
        return false;
    }
    Entry const& e = p->second.entries[l.index];
    std::vector<uint8_t> buf(e.csize);
    f->clear();
    if (!f->seekg(std::streamoff(e.offset)) || !f->read(reinterpret_cast<char*>(buf.data()), e.csize))
    {
        error = Tr("backup.error.damaged", Platform::PathToUtf8(PackPath(l.pack)));
        return false;
    }
    page.resize(e.usize);
    if (e.csize == e.usize)
        page = std::move(buf);
    else
    {
        mz_ulong size = e.usize;
        if (mz_uncompress(page.data(), &size, buf.data(), e.csize) != MZ_OK || size != e.usize)
        {
            error = Tr("backup.error.damaged", Platform::PathToUtf8(PackPath(l.pack)));
            return false;
        }
    }
    if (HashBytes(page.data(), page.size()) != e.hash)
    {
        error = Tr("backup.error.damaged", Platform::PathToUtf8(PackPath(l.pack)));
        return false;
    }
    return true;
}

std::string Store::NewId(std::time_t t) const
{
    std::string const base = Local(t, "%Y-%m-%d_%H%M%S");
    std::string id = base;
    std::error_code ec;
    for (int n = 2; Find(id) || fs::exists(_dir / "snapshots" / (id + ".snap"), ec); ++n)
        id = base + "-" + std::to_string(n);
    return id;
}

bool Store::Save(SnapshotFile const& s, std::string& error)
{
    fs::path const path = _dir / "snapshots" / (s.id + ".snap");
    if (!WriteFileAtomic(path, SerializeSnapshot(s)))
    {
        error = Tr("backup.error.write", Platform::PathToUtf8(path));
        return false;
    }
    auto it = std::find_if(_snaps.begin(), _snaps.end(), [&](SnapshotFile const& o) { return o.id == s.id; });
    if (it != _snaps.end())
        *it = s;
    else
    {
        _snaps.push_back(s);
        std::sort(_snaps.begin(), _snaps.end(), [](SnapshotFile const& a, SnapshotFile const& b)
            { return a.time != b.time ? a.time < b.time : a.id < b.id; });
    }
    return true;
}

std::vector<Loc> Store::Expand(DbManifest const& m)
{
    std::vector<Loc> locs;
    locs.reserve(m.pages);
    for (Run const& r : m.runs)
        for (uint32_t i = 0; i < r.count; ++i)
            locs.push_back({ r.pack, r.start + i });
    return locs;
}

std::vector<Run> Store::Compress(std::vector<Loc> const& locs)
{
    std::vector<Run> runs;
    for (Loc const& l : locs)
    {
        if (!runs.empty() && runs.back().pack == l.pack && runs.back().start + runs.back().count == l.index)
            ++runs.back().count;
        else
            runs.push_back({ l.pack, l.index, 1 });
    }
    return runs;
}

std::map<uint32_t, std::vector<uint32_t>> Store::References() const
{
    std::map<uint32_t, std::vector<uint32_t>> refs;
    for (auto const& [id, pack] : _packs)
        refs[id].assign(pack.entries.size(), 0);
    for (SnapshotFile const& s : _snaps)
        for (DbManifest const& m : s.dbs)
            for (Run const& r : m.runs)
            {
                std::vector<uint32_t>& v = refs[r.pack];
                for (uint32_t i = 0; i < r.count && r.start + i < v.size(); ++i)
                    ++v[r.start + i];
            }
    return refs;
}

void Store::RemoveSnapshot(std::size_t i)
{
    std::error_code ec;
    fs::remove(_dir / "snapshots" / (_snaps[i].id + ".snap"), ec);
    _snaps.erase(_snaps.begin() + std::ptrdiff_t(i));
}

bool Store::Prune(Retention const& r, std::time_t now, std::string& error)
{
    if (_snaps.empty())
        return true;

    // The newest, everything of the last 24 hours, the newest of each day for r.days days, then of each week.
    std::vector<bool> keep(_snaps.size(), false);
    std::set<long> days, weeks;
    for (std::size_t i = _snaps.size(); i-- > 0;)
    {
        std::time_t const t = _snaps[i].time;
        long const day = LocalDay(t);
        if (i + 1 == _snaps.size() || now - t < 24 * 3600)
            keep[i] = true;
        else if (now - t < std::time_t(std::max(1, r.days)) * 24 * 3600)
            keep[i] = days.insert(day).second;
        else
            keep[i] = weeks.insert((day + 3) / 7).second;     // weeks from Monday
        days.insert(day);
        weeks.insert((day + 3) / 7);
    }
    for (std::size_t i = _snaps.size(); i-- > 0;)
        if (!keep[i])
            RemoveSnapshot(i);

    // Over the budget: the oldest snapshots go until the pages still referred to fit.
    std::map<uint32_t, std::vector<uint32_t>> refs = References();
    auto live = [&]
    {
        uint64_t total = 0;
        for (auto const& [id, pack] : _packs)
        {
            std::vector<uint32_t> const& v = refs[id];
            uint64_t bytes = 0;
            for (std::size_t i = 0; i < pack.entries.size(); ++i)
                if (v[i])
                    bytes += pack.entries[i].csize + EntrySize;
            if (bytes)
                total += bytes + sizeof(PackMagic) + TrailerSize;
        }
        return total;
    };
    if (r.budget)
    {
        uint64_t total = live();
        while (total > r.budget && _snaps.size() > 1)
        {
            for (DbManifest const& m : _snaps.front().dbs)
                for (Run const& run : m.runs)
                {
                    std::vector<uint32_t>& v = refs[run.pack];
                    for (uint32_t i = 0; i < run.count && run.start + i < v.size(); ++i)
                        --v[run.start + i];
                }
            RemoveSnapshot(0);
            total = live();
        }
    }

    // Packs nobody refers to go; packs mostly unused, and many small ones, are rewritten into one.
    std::vector<uint32_t> drop, compact, small;
    for (auto const& [id, pack] : _packs)
    {
        std::vector<uint32_t> const& v = refs[id];
        uint64_t used = 0, all = 0;
        for (std::size_t i = 0; i < pack.entries.size(); ++i)
        {
            all += pack.entries[i].csize;
            if (v[i])
                used += pack.entries[i].csize;
        }
        if (!used)
            drop.push_back(id);
        else if (used * 2 < all)
            compact.push_back(id);
        else if (pack.bytes < (4u << 20))
            small.push_back(id);
    }
    if (small.size() > 16)
        compact.insert(compact.end(), small.begin(), small.end());
    // still over the budget: every pack with pages nobody refers to
    if (r.budget && Bytes() > r.budget)
        for (auto const& [id, pack] : _packs)
            if (std::find(drop.begin(), drop.end(), id) == drop.end() && std::find(compact.begin(), compact.end(), id) == compact.end())
            {
                std::vector<uint32_t> const& v = refs[id];
                if (std::find(v.begin(), v.end(), 0u) != v.end())
                    compact.push_back(id);
            }

    std::error_code ec;
    for (uint32_t id : drop)
    {
        _readers.erase(id);
        fs::remove(PackPath(id), ec);
        _packs.erase(id);
    }
    if (!drop.empty())
        RebuildIndex();
    std::sort(compact.begin(), compact.end());
    return compact.empty() || Compact(compact, error);
}

// Copies the entries still referred to of those packs into a new pack, points the snapshots at it, deletes the packs.
// Interrupted anywhere, every snapshot still refers to packs that exist.
bool Store::Compact(std::vector<uint32_t> const& packs, std::string& error)
{
    std::map<uint32_t, std::vector<uint32_t>> const refs = References();
    std::unordered_map<uint64_t, Loc> moved;
    std::vector<uint8_t> buf;
    for (uint32_t id : packs)
    {
        std::ifstream* f = Reader(id);
        Pack const pack = _packs[id];
        std::vector<uint32_t> const& v = refs.at(id);
        for (uint32_t i = 0; i < pack.entries.size(); ++i)
        {
            if (!v[i])
                continue;
            Entry const& e = pack.entries[i];
            buf.resize(e.csize);
            if (f)
                f->clear();
            if (!f || !f->seekg(std::streamoff(e.offset)) || !f->read(reinterpret_cast<char*>(buf.data()), e.csize))
            {
                Abort();
                error = Tr("backup.error.damaged", Platform::PathToUtf8(PackPath(id)));
                return false;
            }
            Loc loc;
            if (!Append(buf.data(), e.csize, e.usize, e.hash, loc, error))
            {
                Abort();
                return false;
            }
            moved[uint64_t(id) << 32 | i] = loc;
        }
    }
    if (!Commit(error))
        return false;

    std::set<uint32_t> const old(packs.begin(), packs.end());
    for (SnapshotFile s : _snaps)
    {
        bool touched = false;
        for (DbManifest& m : s.dbs)
        {
            if (std::none_of(m.runs.begin(), m.runs.end(), [&](Run const& r) { return old.count(r.pack) != 0; }))
                continue;
            std::vector<Loc> locs = Expand(m);
            for (Loc& l : locs)
                if (old.count(l.pack))
                    l = moved.at(uint64_t(l.pack) << 32 | l.index);
            m.runs = Compress(locs);
            touched = true;
        }
        if (touched && !Save(s, error))
            return false;
    }

    std::error_code ec;
    for (uint32_t id : packs)
    {
        _readers.erase(id);
        fs::remove(PackPath(id), ec);
        _packs.erase(id);
    }
    RebuildIndex();
    return true;
}
