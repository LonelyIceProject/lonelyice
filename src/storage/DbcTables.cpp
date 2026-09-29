#include "DbcTables.h"
#include "ClientArchives.h"
#include "DBCStores.h"
#include "SQLiteExtensions.h"
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Built as an SQLite extension: SQLite lives in the core's database library, sqlite3ext.h routes every call through
// the routines that library hands to Init.
#include <sqlite3ext.h>

namespace
{
    sqlite3_api_routines const* sqlite3_api = nullptr;

    struct TableInfo
    {
        std::string name;       // dbc_spell
        std::string file;       // Spell.dbc
        std::string format;     // the core's format string, one character per field
    };

    // A DBC file as read from the archives.
    uint32_t constexpr RowNumber = ~0u;     // offset of a field the file does not have: the row number

    struct DbcImage
    {
        std::vector<uint8_t> data;
        uint32_t records = 0, fields = 0, recordSize = 0, stringSize = 0;
        std::vector<uint32_t> offsets;          // per format character

        uint8_t const* Record(uint32_t row) const { return data.data() + 20 + std::size_t(row) * recordSize; }
        uint8_t const* Strings() const { return data.data() + 20 + std::size_t(records) * recordSize; }
    };

    struct Source
    {
        std::shared_ptr<LonelyIce::ClientArchives::Reader const> reader;
        std::vector<TableInfo> tables;
        std::mutex cacheLock;
        std::map<std::string, std::weak_ptr<DbcImage const>> cache;
    };

    Source* _source = nullptr;

    uint32_t U32(uint8_t const* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    bool IsByte(char c) { return c == 'b' || c == 'X'; }

    std::shared_ptr<DbcImage const> Load(TableInfo const& table, std::string& error)
    {
        std::lock_guard guard(_source->cacheLock);
        if (std::shared_ptr<DbcImage const> cached = _source->cache[table.file].lock())
            return cached;

        std::optional<std::vector<uint8_t>> raw = _source->reader->Read("DBFilesClient\\" + table.file);
        if (!raw)
        {
            error = table.file + " is not in the client's archives";
            return nullptr;
        }

        auto image = std::make_shared<DbcImage>();
        image->data = std::move(*raw);
        std::vector<uint8_t> const& d = image->data;
        if (d.size() < 20 || std::memcmp(d.data(), "WDBC", 4) != 0)
        {
            error = table.file + " is not a DBC file";
            return nullptr;
        }

        image->records = U32(&d[4]);
        image->fields = U32(&d[8]);
        image->recordSize = U32(&d[12]);
        image->stringSize = U32(&d[16]);

        // A "d" field may be missing from the file (the gt* tables): the row number is the index then.
        bool const rowIndex = image->fields + 1 == table.format.size() && table.format.find('d') != std::string::npos;
        if (image->fields != table.format.size() && !rowIndex)
        {
            error = table.file + " has " + std::to_string(image->fields) + " fields, the server expects " +
                std::to_string(table.format.size()) + " (a client of another version?)";
            return nullptr;
        }

        uint32_t offset = 0;
        for (char c : table.format)
        {
            if (rowIndex && c == 'd')
            {
                image->offsets.push_back(RowNumber);
                continue;
            }
            image->offsets.push_back(offset);
            offset += IsByte(c) ? 1 : 4;
        }
        // Unused fields ("x") may be declared wider than they are (PowerDisplay.dbc), so only the size is checked here.
        if (20 + uint64_t(image->records) * image->recordSize + image->stringSize > d.size())
        {
            error = table.file + " is damaged";
            return nullptr;
        }

        _source->cache[table.file] = image;
        return image;
    }

    struct Table : sqlite3_vtab
    {
        TableInfo const* info = nullptr;
    };

    struct Cursor : sqlite3_vtab_cursor
    {
        std::shared_ptr<DbcImage const> image;
        uint32_t row = 0;
    };

    // Columns follow the format string; the index field ("n", or "d" kept out of the record) is named ID, like in
    // the core's override tables. A file without one gets ID on its first column.
    std::string Schema(std::string const& format)
    {
        std::size_t index = format.find_first_of("nd");
        if (index == std::string::npos)
            index = 0;

        std::string sql = "CREATE TABLE x(";
        for (std::size_t i = 0; i < format.size(); ++i)
        {
            if (i)
                sql += ", ";
            sql += i == index ? std::string("ID") : "f" + std::to_string(i);
            switch (format[i])
            {
                case 'f': sql += " REAL"; break;
                case 's': sql += " TEXT"; break;
                default:  sql += " INTEGER"; break;
            }
        }
        return sql + ")";
    }

    int Connect(sqlite3* db, void* aux, int /*argc*/, char const* const* /*argv*/, sqlite3_vtab** out, char** /*error*/)
    {
        TableInfo const* info = static_cast<TableInfo const*>(aux);
        int const rc = sqlite3_declare_vtab(db, Schema(info->format).c_str());
        if (rc != SQLITE_OK)
            return rc;

        Table* table = new Table();
        table->info = info;
        *out = table;
        return SQLITE_OK;
    }

    int Disconnect(sqlite3_vtab* vtab)
    {
        delete static_cast<Table*>(vtab);
        return SQLITE_OK;
    }

    int BestIndex(sqlite3_vtab* /*vtab*/, sqlite3_index_info* info)
    {
        info->estimatedCost = 100000.0;
        info->estimatedRows = 100000;
        return SQLITE_OK;
    }

    int Open(sqlite3_vtab* /*vtab*/, sqlite3_vtab_cursor** out)
    {
        *out = new Cursor();
        return SQLITE_OK;
    }

    int Close(sqlite3_vtab_cursor* cursor)
    {
        delete static_cast<Cursor*>(cursor);
        return SQLITE_OK;
    }

    int Filter(sqlite3_vtab_cursor* base, int /*indexNum*/, char const* /*indexStr*/, int /*argc*/, sqlite3_value** /*argv*/)
    {
        Cursor* cursor = static_cast<Cursor*>(base);
        Table* table = static_cast<Table*>(base->pVtab);
        std::string error;
        cursor->image = Load(*table->info, error);
        cursor->row = 0;
        if (!cursor->image)
        {
            sqlite3_free(table->zErrMsg);
            table->zErrMsg = sqlite3_mprintf("%s", error.c_str());
            return SQLITE_ERROR;
        }
        return SQLITE_OK;
    }

    int Next(sqlite3_vtab_cursor* base)
    {
        ++static_cast<Cursor*>(base)->row;
        return SQLITE_OK;
    }

    int Eof(sqlite3_vtab_cursor* base)
    {
        Cursor* cursor = static_cast<Cursor*>(base);
        return !cursor->image || cursor->row >= cursor->image->records;
    }

    int Column(sqlite3_vtab_cursor* base, sqlite3_context* ctx, int column)
    {
        Cursor* cursor = static_cast<Cursor*>(base);
        DbcImage const& image = *cursor->image;
        char const type = static_cast<Table*>(base->pVtab)->info->format[column];
        if (image.offsets[column] == RowNumber)
        {
            sqlite3_result_int64(ctx, cursor->row);
            return SQLITE_OK;
        }

        if (image.offsets[column] + (IsByte(type) ? 1u : 4u) > image.recordSize)
        {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }

        uint8_t const* field = image.Record(cursor->row) + image.offsets[column];

        switch (type)
        {
            case 'b':
            case 'X':
                sqlite3_result_int(ctx, *field);
                break;
            case 'f':
            {
                float value;
                std::memcpy(&value, field, 4);
                sqlite3_result_double(ctx, value);
                break;
            }
            case 's':
            {
                uint32_t const offset = U32(field);
                if (offset >= image.stringSize)
                    sqlite3_result_text(ctx, "", 0, SQLITE_STATIC);
                else
                {
                    char const* text = reinterpret_cast<char const*>(image.Strings() + offset);
                    sqlite3_result_text(ctx, text, int(strnlen(text, image.stringSize - offset)), SQLITE_TRANSIENT);
                }
                break;
            }
            default:
                sqlite3_result_int64(ctx, sqlite3_int64(U32(field)));
                break;
        }
        return SQLITE_OK;
    }

    int Rowid(sqlite3_vtab_cursor* base, sqlite3_int64* rowid)
    {
        *rowid = static_cast<Cursor*>(base)->row;
        return SQLITE_OK;
    }

    sqlite3_module const Module = []
    {
        sqlite3_module m{};
        m.iVersion = 0;
        m.xCreate = nullptr;            // eponymous only: the table exists on every connection without CREATE
        m.xConnect = &Connect;
        m.xBestIndex = &BestIndex;
        m.xDisconnect = &Disconnect;
        m.xDestroy = &Disconnect;
        m.xOpen = &Open;
        m.xClose = &Close;
        m.xFilter = &Filter;
        m.xNext = &Next;
        m.xEof = &Eof;
        m.xColumn = &Column;
        m.xRowid = &Rowid;
        return m;
    }();

    int Init(sqlite3* db, char** error, sqlite3_api_routines const* api)
    {
        sqlite3_api = api;
        for (TableInfo const& table : _source->tables)
        {
            int const rc = sqlite3_create_module_v2(db, table.name.c_str(), &Module, const_cast<TableInfo*>(&table), nullptr);
            if (rc != SQLITE_OK)
            {
                *error = sqlite3_mprintf("cannot register %s", table.name.c_str());
                return rc;
            }
        }
        return SQLITE_OK;
    }
}

namespace LonelyIce::DbcTables
{
    bool Enable(std::shared_ptr<ClientArchives::Reader const> archives, std::string& error)
    {
        if (_source)
            return true;

        auto source = std::make_unique<Source>();
        source->reader = std::move(archives);
        for (DBCFileInfo const& f : GetDBCFiles())
            source->tables.push_back({ GetDBCTableName(f.file), f.file, f.format });

        _source = source.release();         // lives as long as the process: every connection's tables point into it
        if (!AddSQLiteExtension(&Init))
        {
            error = "cannot register the DBC tables with SQLite";
            return false;
        }
        return true;
    }
}
