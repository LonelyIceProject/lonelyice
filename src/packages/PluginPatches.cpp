#include "PluginPatches.h"
#include "ClientPatch.h"
#include "CryptoHash.h"
#include "DBCfmt.h"
#include "DatabaseEnv.h"
#include "DbcRecipes.h"
#include "Field.h"
#include "Lang.h"
#include "Plugins.h"
#include "QueryResult.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using namespace LonelyIce;
using DbcRecipes::Recipe;

namespace
{
    // Patched DBC tables the server reads rows for from the world database (they override the DBC file).
    struct SqlTable
    {
        char const* dbc;
        char const* table;
        char const* fmt;
        std::vector<std::pair<uint32, uint32>> texts;   // string fields the format skips (x): first, count
    };

    SqlTable const SqlTables[] = {
        { "Spell.dbc", "spell_dbc", SpellEntryfmt, { { 170, 16 }, { 187, 16 } } },   // Description, ToolTip
    };

    SqlTable const* FindSqlTable(std::string const& dbc)
    {
        for (SqlTable const& t : SqlTables)
            if (dbc == t.dbc)
                return &t;
        return nullptr;
    }

    // Bumped when the rows written for the same recipe change; installed recipes are installed again.
    constexpr int InstallerVersion = 1;

    char const Sep = '\x1e', DbSep = '\x1f';   // uninstall statements: db DbSep sql, joined by Sep

    void Exec(std::string const& db, std::string const& sql)
    {
        if (db == "world")
            WorldDatabase.DirectExecute(sql);
        else if (db == "characters")
            CharacterDatabase.DirectExecute(sql);
        else if (db == "auth")
            LoginDatabase.DirectExecute(sql);
    }

    std::string Escaped(std::string s)
    {
        WorldDatabase.EscapeString(s);
        return s;
    }

    std::string Hex(std::string const& s)
    {
        Acore::Crypto::SHA256 sha;
        sha.UpdateData(s);
        sha.Finalize();
        std::string out;
        for (uint8 b : sha.GetDigest())
        {
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", b);
            out += hex;
        }
        return out;
    }

    std::vector<uint8_t> ReadFile(fs::path const& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }

    // One row of a patched table as SQL values, in field order (the *_dbc tables have one column per field).
    std::string RowValues(DbcRecipes::Table const& t, uint32 row, SqlTable const& st)
    {
        std::string out;
        for (uint32 f = 0; f < t.fields; ++f)
        {
            uint32 const v = t.Get(row, f);
            if (f)
                out += ", ";
            bool const text = st.fmt[f] == 's' || std::any_of(st.texts.begin(), st.texts.end(), [f](auto const& r) { return f >= r.first && f < r.first + r.second; });
            if (text)
                out += "'" + Escaped(t.String(v)) + "'";
            else if (st.fmt[f] == 'f')
            {
                float x;
                std::memcpy(&x, &v, 4);
                char buf[32];
                snprintf(buf, sizeof(buf), "%.9g", x);
                out += buf;
            }
            else
                // negative numbers are stored as their two's complement
                out += v >= 0xFFFF0000u ? std::to_string(int32(v)) : std::to_string(v);
        }
        return out;
    }

    struct Installed
    {
        std::string hash;
        std::string uninstall;
    };
}

PluginPatches::Result PluginPatches::Apply(Options const& o)
{
    Result res;
    auto fail = [&](std::string error) { res.ok = false; res.error = std::move(error); return res; };

    WorldDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS `plugin_ids` (`plugin` VARCHAR(64) NOT NULL, `name` VARCHAR(64) NOT NULL, "
        "`dbc` VARCHAR(64) NOT NULL, `id` INT UNSIGNED NOT NULL, PRIMARY KEY (`plugin`, `name`))");
    WorldDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS `plugin_patches` (`plugin` VARCHAR(64) NOT NULL, `hash` VARCHAR(64) NOT NULL, "
        "`uninstall` TEXT NOT NULL, PRIMARY KEY (`plugin`))");

    // What the databases have now.
    std::map<std::string, Installed> installed;
    if (QueryResult r = WorldDatabase.Query("SELECT `plugin`, `hash`, `uninstall` FROM `plugin_patches`"))
        do
            installed[r->Fetch()[0].Get<std::string>()] = { r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<std::string>() };
        while (r->NextRow());

    DbcRecipes::IdMap ids;
    std::map<std::string, std::string> idTable;     // plugin/name -> dbc
    if (QueryResult r = WorldDatabase.Query("SELECT `plugin`, `name`, `dbc`, `id` FROM `plugin_ids`"))
        do
        {
            Field* f = r->Fetch();
            std::string const q = f[0].Get<std::string>() + "/" + f[1].Get<std::string>();
            ids[q] = f[3].Get<uint32>();
            idTable[q] = f[2].Get<std::string>();
        } while (r->NextRow());

    // The installed plugins and their recipes, in load order. A plugin with server code counts once it loaded.
    std::vector<PluginManifest> const plugins = ReadPlugins(o.pluginsDir);
    std::set<std::string> present;
    std::vector<Recipe> recipes;
    for (PluginManifest const* p : OrderByDependencies(plugins))
    {
        present.insert(p->id);
        if (p->patches.empty() || (p->serverLibrary && !o.loaded.count(p->id)))
            continue;
        Recipe r;
        std::string error;
        if (!r.Load(*p, error))
            return fail(error);
        recipes.push_back(std::move(r));
    }

    auto uninstall = [&](std::string const& plugin, bool dropIds)
    {
        std::string const& text = installed[plugin].uninstall;
        for (std::size_t pos = 0; pos < text.size();)
        {
            std::size_t end = text.find(Sep, pos);
            if (end == std::string::npos)
                end = text.size();
            std::string const entry = text.substr(pos, end - pos);
            std::size_t const split = entry.find(DbSep);
            if (split != std::string::npos)
                Exec(entry.substr(0, split), entry.substr(split + 1));
            pos = end + 1;
        }
        WorldDatabase.DirectExecute("DELETE FROM `plugin_patches` WHERE `plugin` = '" + Escaped(plugin) + "'");
        if (dropIds)
        {
            WorldDatabase.DirectExecute("DELETE FROM `plugin_ids` WHERE `plugin` = '" + Escaped(plugin) + "'");
            std::erase_if(ids, [&](auto const& e) { return e.first.rfind(plugin + "/", 0) == 0; });
        }
        installed.erase(plugin);
        res.changed = true;
    };

    // Plugins that are gone: their rows, SQL and ids go.
    std::vector<std::string> gone;
    for (auto const& [plugin, state] : installed)
        if (!present.count(plugin))
            gone.push_back(plugin);
    for (std::string const& plugin : gone)
    {
        uninstall(plugin, true);
        res.log.push_back(Tr("patch.plugin.removed", plugin));
    }

    // Stock tables of the server, read once.
    std::map<std::string, DbcRecipes::Table> stock;
    auto stockTable = [&](std::string const& table) -> DbcRecipes::Table const*
    {
        auto it = stock.find(table);
        if (it == stock.end())
        {
            DbcRecipes::Table t;
            std::vector<uint8_t> image = ReadFile(o.serverDbcDir / table);
            if (image.empty() && !o.clientDir.empty())
                image = ClientPatch::ReadStockTable(o.clientDir, table);
            if (!t.Parse(image))
                return nullptr;
            it = stock.emplace(table, std::move(t)).first;
        }
        return &it->second;
    };

    for (Recipe const& r : recipes)
    {
        std::string const hash = Hex(std::to_string(InstallerVersion) + "\n" + r.Version() + "\n" + r.Text());
        auto was = installed.find(r.Plugin());
        if (was != installed.end() && was->second.hash == hash)
            continue;
        if (was != installed.end())
            uninstall(r.Plugin(), false);

        // Named ids: new ones get the next free id of their table, ones no longer declared are released.
        std::vector<DbcRecipes::NamedId> const declared = r.Ids();
        for (auto it = ids.begin(); it != ids.end();)
        {
            bool const own = it->first.rfind(r.Plugin() + "/", 0) == 0;
            bool const kept = std::any_of(declared.begin(), declared.end(), [&](auto const& n) { return n.Qualified() == it->first; });
            if (own && !kept)
            {
                WorldDatabase.DirectExecute("DELETE FROM `plugin_ids` WHERE `plugin` = '" + Escaped(r.Plugin()) + "' AND `name` = '"
                    + Escaped(it->first.substr(r.Plugin().size() + 1)) + "'");
                it = ids.erase(it);
            }
            else
                ++it;
        }
        for (DbcRecipes::NamedId const& n : declared)
        {
            if (ids.count(n.Qualified()))
                continue;
            DbcRecipes::Table const* t = stockTable(n.table);
            if (!t)
                return fail(Tr("patch.plugin.no_id_table", r.Plugin(), n.table));
            uint32 next = t->MaxKey(n.keyField);
            for (auto const& [q, id] : ids)
                if (idTable[q] == n.table)
                    next = std::max(next, id);
            for (Recipe const& other : recipes)
                for (uint32 fixed : other.FixedIds(n.table))
                    next = std::max(next, fixed);
            if (SqlTable const* st = FindSqlTable(n.table))
                if (QueryResult m = WorldDatabase.Query(std::string("SELECT MAX(`ID`) FROM `") + st->table + "`"))
                    if (!m->Fetch()[0].IsNull())
                        next = std::max(next, m->Fetch()[0].Get<uint32>());
            ids[n.Qualified()] = ++next;
            idTable[n.Qualified()] = n.table;
            WorldDatabase.DirectExecute("INSERT INTO `plugin_ids` (`plugin`, `name`, `dbc`, `id`) VALUES ('" + Escaped(r.Plugin()) + "', '"
                + Escaped(n.name) + "', '" + Escaped(n.table) + "', " + std::to_string(next) + ")");
            res.log.push_back(r.Plugin() + ": " + n.name + " = " + n.table + " " + std::to_string(next));
        }

        // Server rows of the patched tables, and how to take them back.
        std::string undo;
        auto addUndo = [&](std::string const& db, std::string const& sql)
        {
            if (!undo.empty())
                undo += Sep;
            undo += db + DbSep + sql;
        };
        std::string error;
        for (std::string const& db : { "world", "characters", "auth" })
            for (std::string sql : r.Sql("uninstall", db))
            {
                if (!DbcRecipes::Substitute(r.Plugin(), sql, ids, error))
                    return fail(error);
                addUndo(db, sql);
            }

        for (std::string const& table : r.Tables())
        {
            SqlTable const* st = FindSqlTable(table);
            if (!st)
                continue;       // client-only table
            DbcRecipes::Table const* base = stockTable(table);
            if (!base)
                return fail(Tr("patch.plugin.no_server_table", r.Plugin(), table));
            if (std::strlen(st->fmt) != base->fields)
                return fail(Tr("patch.plugin.format_mismatch", table));
            DbcRecipes::Table t = *base;
            std::set<uint32_t> touched;
            if (!r.Patch(table, t, ids, "*", &touched, error))
                return fail(error);
            for (uint32 id : touched)
            {
                std::string const del = std::string("DELETE FROM `") + st->table + "` WHERE `ID` = " + std::to_string(id);
                WorldDatabase.DirectExecute(del);
                WorldDatabase.DirectExecute(std::string("INSERT INTO `") + st->table + "` VALUES (" + RowValues(t, *t.Find(0, id), *st) + ")");
                addUndo("world", del);
            }
        }

        for (std::string const& db : { "world", "characters", "auth" })
            for (std::string sql : r.Sql("install", db))
            {
                if (!DbcRecipes::Substitute(r.Plugin(), sql, ids, error))
                    return fail(error);
                Exec(db, sql);
            }

        WorldDatabase.DirectExecute("INSERT INTO `plugin_patches` (`plugin`, `hash`, `uninstall`) VALUES ('" + Escaped(r.Plugin()) + "', '"
            + hash + "', '" + Escaped(undo) + "')");
        installed[r.Plugin()] = { hash, undo };
        res.changed = true;
        res.log.push_back(Tr("patch.plugin.installed", r.Plugin()));
    }

    if (!o.clientDir.empty())
    {
        ClientPatch::Result client = ClientPatch::Apply(o.clientDir, recipes, ids);
        res.log.insert(res.log.end(), client.log.begin(), client.log.end());
        if (!client.ok)
            return fail(Tr("patch.plugin.client_error", client.error));
    }
    return res;
}
