#ifndef LONELYICE_DBCRECIPES_H
#define LONELYICE_DBCRECIPES_H

#include "Plugins.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Plugin patch recipes (docs/plugin-format.md, 7): changes of DBC tables with named ids, and the SQL that goes
// with them. The same recipes build the client archives and the server rows.
namespace LonelyIce::DbcRecipes
{
    // WDBC image: header, fixed-size records of 4-byte fields, string block.
    struct Table
    {
        uint32_t fields = 0, recordSize = 0;
        std::vector<uint8_t> records;
        std::string strings;

        bool Parse(std::vector<uint8_t> const& image);
        std::vector<uint8_t> Write() const;
        uint32_t Count() const { return recordSize ? uint32_t(records.size() / recordSize) : 0; }
        uint32_t Get(uint32_t row, uint32_t field) const;
        void Set(uint32_t row, uint32_t field, uint32_t value);
        std::optional<uint32_t> Find(uint32_t keyField, uint32_t key) const;
        uint32_t MaxKey(uint32_t keyField) const;
        uint32_t AddRow(std::optional<uint32_t> copyFrom);
        uint32_t AddString(std::string const& s);
        std::string String(uint32_t offset) const;
    };

    struct NamedId
    {
        std::string plugin, name, table;
        uint32_t keyField = 0;
        std::optional<uint32_t> copy;       // stock row the new row starts from
        std::string Qualified() const { return plugin + "/" + name; }
    };

    // plugin/name -> id
    using IdMap = std::map<std::string, uint32_t>;

    class Recipe
    {
    public:
        Recipe();
        ~Recipe();
        Recipe(Recipe&&) noexcept;
        Recipe& operator=(Recipe&&) noexcept;

        // The recipe of a plugin (manifest "patches"); false with error when it cannot be read.
        bool Load(PluginManifest const& plugin, std::string& error);

        std::string const& Plugin() const { return _plugin; }
        std::string const& Version() const { return _version; }
        std::string const& Text() const { return _text; }      // file contents, for stamps
        std::filesystem::path const& Dir() const { return _dir; }

        std::vector<NamedId> Ids() const;
        std::vector<std::string> Tables() const;             // tables the patches change
        std::vector<uint32_t> FixedIds(std::string const& table) const;
        // SQL of "install" / "uninstall" for a database ("world", "characters", "auth")
        std::vector<std::string> Sql(char const* section, std::string const& database) const;
        // client files: archive path -> file in the plugin
        std::vector<std::pair<std::string, std::filesystem::path>> Files() const;

        // Applies this recipe's patches of table; touched gets the ids of the rows changed or added. Localized texts
        // go into all 16 slots for a client locale ("ruRU"), or each slot gets its own locale with "*".
        bool Patch(std::string const& table, Table& t, IdMap const& ids, std::string const& locale,
            std::set<uint32_t>* touched, std::string& error) const;

    private:
        struct Node;
        std::string _plugin, _version, _text;
        std::filesystem::path _dir;
        std::unique_ptr<Node> _root;
    };

    // Replaces {{id:name}} (a name of plugin) and {{id:other.plugin/name}} in text.
    bool Substitute(std::string const& plugin, std::string& text, IdMap const& ids, std::string& error);
}

#endif
