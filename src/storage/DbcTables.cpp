#include "DbcTables.h"
#include "ClientArchives.h"
#include "DBCStores.h"
#include "DbcFile.h"
#include "SQLiteExtensions.h"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Built as an SQLite extension: SQLite lives in the core's database library, sqlite3ext.h routes every call through
// the routines that library hands to Init.
#include <sqlite3ext.h>

using LonelyIce::DbcFile;

namespace
{
    sqlite3_api_routines const* sqlite3_api = nullptr;

    struct TableInfo
    {
        std::string name;       // dbc_spell
        std::string file;       // Spell.dbc
        std::string format;     // the core's format string, one character per field
    };

    struct Source
    {
        std::shared_ptr<LonelyIce::ClientArchives::Reader const> reader;
        std::vector<TableInfo> tables;
        std::mutex cacheLock;
        std::map<std::string, std::weak_ptr<DbcFile const>> cache;     // files some cursor still reads
    };

    Source* _source = nullptr;

    std::shared_ptr<DbcFile const> Load(TableInfo const& table, std::string& error)
    {
        std::lock_guard guard(_source->cacheLock);
        if (std::shared_ptr<DbcFile const> cached = _source->cache[table.file].lock())
            return cached;

        std::optional<std::vector<uint8_t>> raw = _source->reader->Read("DBFilesClient\\" + table.file);
        if (!raw)
        {
            error = table.file + " is not in the client's archives";
            return nullptr;
        }

        std::shared_ptr<DbcFile const> file = DbcFile::Parse(std::move(*raw), table.file, table.format, error);
        if (file)
            _source->cache[table.file] = file;
        return file;
    }

    struct Table : sqlite3_vtab
    {
        TableInfo const* info = nullptr;
    };

    struct Cursor : sqlite3_vtab_cursor
    {
        std::shared_ptr<DbcFile const> file;
        uint32_t row = 0;
    };

    std::string Schema(std::string const& format)
    {
        std::string sql = "CREATE TABLE x(";
        for (std::size_t i = 0; i < format.size(); ++i)
        {
            if (i)
                sql += ", ";
            sql += DbcFile::ColumnName(format, i);
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
        cursor->file = Load(*table->info, error);
        cursor->row = 0;
        if (!cursor->file)
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
        return !cursor->file || cursor->row >= cursor->file->Rows();
    }

    int Column(sqlite3_vtab_cursor* base, sqlite3_context* ctx, int column)
    {
        Cursor* cursor = static_cast<Cursor*>(base);
        DbcFile const& file = *cursor->file;
        if (file.Missing(cursor->row, column))
        {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }

        switch (file.Format()[column])
        {
            case 'f':
                sqlite3_result_double(ctx, file.Float(cursor->row, column));
                break;
            case 's':
            {
                std::string_view const text = file.String(cursor->row, column);
                sqlite3_result_text(ctx, text.data() ? text.data() : "", int(text.size()), SQLITE_TRANSIENT);
                break;
            }
            default:
                sqlite3_result_int64(ctx, sqlite3_int64(file.UInt(cursor->row, column)));
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
