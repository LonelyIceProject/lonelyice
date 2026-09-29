#ifndef LONELYICE_INIFILE_H
#define LONELYICE_INIFILE_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace LonelyIce
{
    // A small INI file ([section], key=value, ; or # comments) kept line by line, so saving changes only what was set.
    // Reads UTF-8 and UTF-16 (files written by older Windows builds), writes UTF-8.
    class IniFile
    {
    public:
        bool Load(std::filesystem::path const& path);
        bool Save(std::filesystem::path const& path) const;

        std::optional<std::string> Get(std::string const& section, std::string const& key) const;
        std::string Get(std::string const& section, std::string const& key, std::string const& def) const;
        void Set(std::string const& section, std::string const& key, std::string const& value);

    private:
        std::vector<std::string> _lines;
    };
}

#endif
