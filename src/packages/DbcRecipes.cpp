#include "DbcRecipes.h"
#include "Lang.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <fkYAML/node.hpp>

namespace fs = std::filesystem;
using namespace LonelyIce::DbcRecipes;

namespace
{
    std::string Str(fkyaml::node const& n, char const* key)
    {
        return n.is_mapping() && n.contains(key) && n[key].is_string() ? n[key].get_value<std::string>() : std::string();
    }

    std::optional<uint32_t> UInt(fkyaml::node const& n, char const* key)
    {
        if (n.is_mapping() && n.contains(key) && n[key].is_integer())
            return uint32_t(n[key].get_value<int64_t>());
        return std::nullopt;
    }

    // The text for a client locale: exact ("ruRU"), language ("ru"), then English.
    std::string LocaleText(fkyaml::node const& n, std::string const& locale)
    {
        std::vector<std::string> keys;
        if (!locale.empty())
            keys = { locale, locale.substr(0, 2) };
        for (char const* k : { "en", "enUS", "enGB" })
            keys.emplace_back(k);
        for (std::string const& k : keys)
            if (n.contains(k) && n[k].is_string())
                return n[k].get_value<std::string>();
        for (auto const& [k, v] : n.as_map())
            if (v.is_string())
                return v.get_value<std::string>();
        return {};
    }

    std::string Qualified(std::string const& plugin, std::string const& name)
    {
        return name.find('/') == std::string::npos ? plugin + "/" + name : name;
    }
}

// ---- Table

bool Table::Parse(std::vector<uint8_t> const& d)
{
    if (d.size() < 20 || std::memcmp(d.data(), "WDBC", 4) != 0)
        return false;
    uint32_t count, stringSize;
    std::memcpy(&count, d.data() + 4, 4);
    std::memcpy(&fields, d.data() + 8, 4);
    std::memcpy(&recordSize, d.data() + 12, 4);
    std::memcpy(&stringSize, d.data() + 16, 4);
    std::size_t const size = std::size_t(count) * recordSize;
    if (recordSize != fields * 4 || d.size() < 20 + size + stringSize)
        return false;
    records.assign(d.begin() + 20, d.begin() + 20 + size);
    strings.assign(reinterpret_cast<char const*>(d.data()) + 20 + size, stringSize);
    return true;
}

std::vector<uint8_t> Table::Write() const
{
    std::vector<uint8_t> out(20);
    std::memcpy(out.data(), "WDBC", 4);
    uint32_t const header[4] = { Count(), fields, recordSize, uint32_t(strings.size()) };
    std::memcpy(out.data() + 4, header, sizeof(header));
    out.insert(out.end(), records.begin(), records.end());
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

uint32_t Table::Get(uint32_t row, uint32_t field) const
{
    uint32_t v;
    std::memcpy(&v, records.data() + std::size_t(row) * recordSize + field * 4, 4);
    return v;
}

void Table::Set(uint32_t row, uint32_t field, uint32_t value)
{
    std::memcpy(records.data() + std::size_t(row) * recordSize + field * 4, &value, 4);
}

std::optional<uint32_t> Table::Find(uint32_t keyField, uint32_t key) const
{
    for (uint32_t i = 0; i < Count(); ++i)
        if (Get(i, keyField) == key)
            return i;
    return std::nullopt;
}

uint32_t Table::MaxKey(uint32_t keyField) const
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < Count(); ++i)
        m = std::max(m, Get(i, keyField));
    return m;
}

uint32_t Table::AddRow(std::optional<uint32_t> copyFrom)
{
    std::size_t const at = records.size();
    records.resize(at + recordSize, 0);
    if (copyFrom)
        std::memcpy(records.data() + at, records.data() + std::size_t(*copyFrom) * recordSize, recordSize);
    return Count() - 1;
}

uint32_t Table::AddString(std::string const& s)
{
    if (s.empty())
        return 0;
    if (strings.empty())
        strings.push_back('\0');
    uint32_t const offset = uint32_t(strings.size());
    strings += s;
    strings.push_back('\0');
    return offset;
}

std::string Table::String(uint32_t offset) const
{
    if (offset >= strings.size())
        return {};
    return std::string(strings.c_str() + offset);
}

// ---- Recipe

struct Recipe::Node
{
    fkyaml::node root;
};

Recipe::Recipe() = default;
Recipe::~Recipe() = default;
Recipe::Recipe(Recipe&&) noexcept = default;
Recipe& Recipe::operator=(Recipe&&) noexcept = default;

bool Recipe::Load(PluginManifest const& plugin, std::string& error)
{
    _plugin = plugin.id;
    _version = plugin.version;
    _dir = plugin.dir;
    std::ifstream in(plugin.patches, std::ios::binary);
    if (!in)
    {
        error = Tr("patch.recipe.no_file", plugin.id, plugin.patches.filename().string());
        return false;
    }
    _text.assign(std::istreambuf_iterator<char>(in), {});
    try
    {
        _root = std::make_unique<Node>();
        _root->root = fkyaml::node::deserialize(_text);
        if (!_root->root.is_mapping())
            throw std::runtime_error("not an object");
    }
    catch (std::exception const& e)
    {
        error = plugin.id + ": " + plugin.patches.filename().string() + ": " + e.what();
        return false;
    }
    return true;
}

std::vector<NamedId> Recipe::Ids() const
{
    std::vector<NamedId> out;
    fkyaml::node const& r = _root->root;
    if (!r.contains("ids") || !r["ids"].is_mapping())
        return out;
    for (auto const& [key, value] : r["ids"].as_map())
    {
        NamedId n;
        n.plugin = _plugin;
        n.name = key.get_value<std::string>();
        n.table = Str(value, "table");
        n.keyField = UInt(value, "key").value_or(0);
        n.copy = UInt(value, "copy");
        out.push_back(std::move(n));
    }
    return out;
}

std::vector<std::string> Recipe::Tables() const
{
    std::vector<std::string> out;
    auto add = [&](std::string const& t)
    {
        if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end())
            out.push_back(t);
    };
    for (NamedId const& n : Ids())
        add(n.table);
    fkyaml::node const& r = _root->root;
    if (r.contains("patches") && r["patches"].is_sequence())
        for (fkyaml::node const& entry : r["patches"].as_seq())
            add(Str(entry, "table"));
    return out;
}

std::vector<uint32_t> Recipe::FixedIds(std::string const& table) const
{
    std::vector<uint32_t> out;
    fkyaml::node const& r = _root->root;
    if (r.contains("patches") && r["patches"].is_sequence())
        for (fkyaml::node const& entry : r["patches"].as_seq())
            if (Str(entry, "table") == table && entry.contains("rows"))
                for (fkyaml::node const& row : entry["rows"].as_seq())
                    if (auto id = UInt(row, "id"))
                        out.push_back(*id);
    return out;
}

std::vector<std::string> Recipe::Sql(char const* section, std::string const& database) const
{
    std::vector<std::string> out;
    fkyaml::node const& r = _root->root;
    if (r.contains(section) && r[section].is_mapping() && r[section].contains(database) && r[section][database].is_sequence())
        for (fkyaml::node const& s : r[section][database].as_seq())
            if (s.is_string())
                out.push_back(s.get_value<std::string>());
    return out;
}

std::vector<std::pair<std::string, fs::path>> Recipe::Files() const
{
    std::vector<std::pair<std::string, fs::path>> out;
    fkyaml::node const& r = _root->root;
    if (r.contains("files") && r["files"].is_sequence())
        for (fkyaml::node const& f : r["files"].as_seq())
        {
            std::string to = Str(f, "to");
            std::replace(to.begin(), to.end(), '/', '\\');
            out.emplace_back(to, _dir / fs::u8path(Str(f, "from")));
        }
    return out;
}

bool Recipe::Patch(std::string const& table, Table& t, IdMap const& ids, std::string const& locale,
    std::set<uint32_t>* touched, std::string& error) const
{
    fkyaml::node const& r = _root->root;
    if (!r.contains("patches") || !r["patches"].is_sequence())
        return true;

    std::vector<NamedId> const named = Ids();
    auto lookup = [&](std::string const& ref) -> uint32_t
    {
        auto it = ids.find(Qualified(_plugin, ref));
        return it == ids.end() ? 0 : it->second;
    };

    for (fkyaml::node const& entry : r["patches"].as_seq())
    {
        if (Str(entry, "table") != table || !entry.contains("rows"))
            continue;
        std::string const where = _plugin + ", " + table;
        uint32_t const keyField = UInt(entry, "key").value_or(0);

        for (fkyaml::node const& row : entry["rows"].as_seq())
        {
            // A fixed id, or "@name" for a named id (the row is added when missing).
            uint32_t id = 0;
            bool byName = false;
            NamedId const* declared = nullptr;
            if (auto fixed = UInt(row, "id"))
                id = *fixed;
            else if (std::string ref = Str(row, "id"); ref.size() > 1 && ref[0] == '@')
            {
                byName = true;
                std::string const q = Qualified(_plugin, ref.substr(1));
                for (NamedId const& n : named)
                    if (n.Qualified() == q)
                        declared = &n;
                if (!(id = lookup(ref.substr(1))))
                {
                    error = Tr("patch.recipe.no_id", where, ref);
                    return false;
                }
            }
            else
            {
                error = Tr("patch.recipe.row_no_id", where);
                return false;
            }

            std::string mode = Str(row, "mode");
            if (mode.empty())
                mode = byName ? "upsert" : "update";

            std::optional<uint32_t> at = t.Find(keyField, id);
            if (at && mode == "insert")
            {
                error = Tr("patch.recipe.row_exists", where, id);
                return false;
            }
            if (!at)
            {
                if (mode == "update")
                {
                    error = Tr("patch.recipe.row_missing", where, id);
                    return false;
                }
                std::optional<uint32_t> copy;
                std::optional<uint32_t> copyId = UInt(row, "copy");
                if (!copyId && declared)
                    copyId = declared->copy;
                if (copyId && !(copy = t.Find(keyField, *copyId)))
                {
                    error = Tr("patch.recipe.copy_missing", where, *copyId);
                    return false;
                }
                at = t.AddRow(copy);
                t.Set(*at, keyField, id);
            }
            if (touched)
                touched->insert(id);

            if (!row.contains("set"))
                continue;
            for (auto const& [key, value] : row["set"].as_map())
            {
                uint32_t field = 0;
                try
                {
                    field = uint32_t(std::stoul(key.get_value<std::string>()));
                }
                catch (std::exception const&)
                {
                    error = Tr("patch.recipe.field_not_number", where, key.get_value<std::string>());
                    return false;
                }
                bool const isRef = value.is_mapping() && value.contains("ref");
                bool const localized = value.is_mapping() && !isRef;
                if (field == keyField || field + (localized ? 16 : 0) >= t.fields)
                {
                    error = Tr("patch.recipe.field_forbidden", where, field);
                    return false;
                }

                if (value.is_integer())
                    t.Set(*at, field, uint32_t(value.get_value<int64_t>()));
                else if (value.is_float_number())
                {
                    float const f = float(value.get_value<double>());
                    uint32_t bits;
                    std::memcpy(&bits, &f, 4);
                    t.Set(*at, field, bits);
                }
                else if (value.is_boolean())
                    t.Set(*at, field, value.get_value<bool>() ? 1 : 0);
                else if (value.is_string())
                    t.Set(*at, field, t.AddString(value.get_value<std::string>()));
                else if (isRef)
                {
                    uint32_t const refId = lookup(Str(value, "ref"));
                    if (!refId)
                    {
                        error = Tr("patch.recipe.no_id", where, Str(value, "ref"));
                        return false;
                    }
                    t.Set(*at, field, refId);
                }
                else if (localized)
                {
                    // A client table gets its locale's text in every slot, so the client finds it whatever its slot
                    // order; the server's rows (locale "*") get each slot's own locale.
                    static char const* const slots[16] = { "enUS", "koKR", "frFR", "deDE", "zhCN", "zhTW", "esES", "esMX", "ruRU" };
                    std::map<std::string, uint32_t> offsets;
                    for (uint32_t slot = 0; slot < 16; ++slot)
                    {
                        std::string const l = locale != "*" ? locale : slots[slot] ? slots[slot] : "";
                        std::string const text = LocaleText(value, l);
                        auto it = offsets.find(text);
                        if (it == offsets.end())
                            it = offsets.emplace(text, t.AddString(text)).first;
                        t.Set(*at, field + slot, it->second);
                    }
                }
            }
        }
    }
    return true;
}

bool LonelyIce::DbcRecipes::Substitute(std::string const& plugin, std::string& text, IdMap const& ids, std::string& error)
{
    static std::string const open = "{{id:";
    std::string out;
    std::size_t last = 0;
    for (std::size_t pos = text.find(open); pos != std::string::npos; pos = text.find(open, last))
    {
        std::size_t const end = text.find("}}", pos);
        if (end == std::string::npos)
            break;
        std::string const name = text.substr(pos + open.size(), end - pos - open.size());
        auto it = ids.find(Qualified(plugin, name));
        if (it == ids.end())
        {
            error = Tr("patch.recipe.no_id_placeholder", plugin, name);
            return false;
        }
        out.append(text, last, pos - last);
        out += std::to_string(it->second);
        last = end + 2;
    }
    if (last)
    {
        out.append(text, last, std::string::npos);
        text = std::move(out);
    }
    return true;
}
