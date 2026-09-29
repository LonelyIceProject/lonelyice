#ifndef LONELYICE_CONFFILE_H
#define LONELYICE_CONFFILE_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace LonelyIce
{
    // AzerothCore .conf reader/writer that keeps comments, order and the quoting style of each value.
    class ConfFile
    {
    public:
        bool Load(std::filesystem::path const& file);
        bool Save() const;
        bool IsLoaded() const { return _loaded; }
        std::filesystem::path const& GetPath() const { return _path; }

        std::optional<std::string> Get(std::string const& key) const;
        // Replaces the value in place, keeping its quotes; appends "Key = value" at the end if the key is
        // missing, quoted when quoteNew is set.
        void Set(std::string const& key, std::string const& value, bool quoteNew = false);

    private:
        int Find(std::string const& key) const;

        std::filesystem::path _path;
        std::vector<std::string> _lines;
        std::vector<bool> _cr;
        bool _loaded = false;
        bool _crlf = false;
    };
}

#endif
