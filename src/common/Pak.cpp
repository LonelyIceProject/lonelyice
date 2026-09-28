#include "Pak.h"
#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace
{
    constexpr char Magic[8] = { 'L', 'I', 'P', 'A', 'K', '0', '0', '1' };
    constexpr std::size_t Chunk = 1 << 18;

    class Deflater
    {
    public:
        explicit Deflater(std::ofstream& out) : _out(out)
        {
            deflateInit(&_z, 9);
        }
        ~Deflater() { deflateEnd(&_z); }

        bool Put(void const* data, std::size_t size, int flush = Z_NO_FLUSH)
        {
            _z.next_in = static_cast<Bytef*>(const_cast<void*>(data));
            _z.avail_in = uInt(size);
            do
            {
                _z.next_out = reinterpret_cast<Bytef*>(_buf);
                _z.avail_out = sizeof(_buf);
                int r = deflate(&_z, flush);
                if (r == Z_STREAM_ERROR)
                    return false;
                _out.write(_buf, sizeof(_buf) - _z.avail_out);
            } while (_z.avail_out == 0);
            return bool(_out);
        }

        bool Finish() { return Put(nullptr, 0, Z_FINISH); }

    private:
        std::ofstream& _out;
        z_stream _z{};
        char _buf[Chunk];
    };

    class Inflater
    {
    public:
        explicit Inflater(std::ifstream& in) : _in(in)
        {
            inflateInit(&_z);
        }
        ~Inflater() { inflateEnd(&_z); }

        uint64_t Consumed() const { return _consumed; }

        bool Get(void* data, std::size_t size)
        {
            _z.next_out = static_cast<Bytef*>(data);
            _z.avail_out = uInt(size);
            while (_z.avail_out)
            {
                if (_z.avail_in == 0)
                {
                    _in.read(_buf, sizeof(_buf));
                    std::streamsize n = _in.gcount();
                    if (n <= 0)
                        return false;
                    _consumed += uint64_t(n);
                    _z.next_in = reinterpret_cast<Bytef*>(_buf);
                    _z.avail_in = uInt(n);
                }
                int r = inflate(&_z, Z_NO_FLUSH);
                if (r == Z_STREAM_END)
                    return _z.avail_out == 0;
                if (r != Z_OK)
                    return false;
            }
            return true;
        }

    private:
        std::ifstream& _in;
        z_stream _z{};
        char _buf[Chunk];
        uint64_t _consumed = 0;
    };
}

bool LonelyIce::Pak::Write(fs::path const& pak, std::vector<Entry> const& entries, std::string& error)
{
    std::ofstream out(pak, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "cannot create " + pak.string();
        return false;
    }
    out.write(Magic, sizeof(Magic));
    auto z = std::make_unique<Deflater>(out);
    std::vector<char> buf(Chunk);
    for (Entry const& e : entries)
    {
        std::ifstream in(e.source, std::ios::binary);
        std::error_code ec;
        uint64_t size = fs::file_size(e.source, ec);
        if (!in || ec)
        {
            error = "cannot read " + e.source.string();
            return false;
        }
        uint32_t len = uint32_t(e.path.size());
        z->Put(&len, 4);
        z->Put(e.path.data(), len);
        z->Put(&size, 8);
        for (uint64_t left = size; left;)
        {
            std::size_t n = std::size_t(std::min<uint64_t>(left, buf.size()));
            in.read(buf.data(), n);
            if (std::size_t(in.gcount()) != n)
            {
                error = "short read " + e.source.string();
                return false;
            }
            z->Put(buf.data(), n);
            left -= n;
        }
    }
    uint32_t end = 0;
    z->Put(&end, 4);
    if (!z->Finish())
    {
        error = "cannot write " + pak.string();
        return false;
    }
    return true;
}

bool LonelyIce::Pak::Read(fs::path const& pak, std::function<bool(std::string const&, std::string const&)> const& fn, std::string& error,
    std::function<void(uint64_t, uint64_t)> const& progress)
{
    std::ifstream in(pak, std::ios::binary);
    char magic[8]{};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, Magic, 8) != 0)
    {
        error = "not a LonelyIce pak: " + pak.string();
        return false;
    }
    std::error_code ec;
    uint64_t total = fs::file_size(pak, ec);
    auto z = std::make_unique<Inflater>(in);
    std::string path, data;
    for (;;)
    {
        uint32_t len = 0;
        uint64_t size = 0;
        if (!z->Get(&len, 4) || len > 4096)
        {
            error = "damaged pak: " + pak.string();
            return false;
        }
        if (len == 0)
            break;
        path.resize(len);
        if (!z->Get(path.data(), len) || !z->Get(&size, 8) || size > (1ull << 32))
        {
            error = "damaged pak: " + pak.string();
            return false;
        }
        data.resize(std::size_t(size));
        if (size && !z->Get(data.data(), std::size_t(size)))
        {
            error = "damaged pak: " + pak.string();
            return false;
        }
        if (!fn(path, data))
            return true;
        if (progress)
            progress(z->Consumed(), total);
    }
    if (progress)
        progress(total, total);
    return true;
}

bool LonelyIce::Pak::Extract(fs::path const& pak, fs::path const& dir, std::string& error, std::function<void(uint64_t, uint64_t)> const& progress)
{
    bool ok = true;
    bool read = Read(pak, [&](std::string const& path, std::string const& data)
    {
        fs::path target = dir / fs::u8path(path);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        if (!out)
        {
            error = "cannot write " + target.string();
            ok = false;
        }
        return ok;
    }, error, progress);
    return read && ok;
}
