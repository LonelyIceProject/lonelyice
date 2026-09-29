#include "DbcUnpack.h"
#include "ClientArchives.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DbcFile.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include <cmath>

namespace
{
    std::size_t constexpr RowsPerInsert = 250;

    std::string Quote(std::string_view name)
    {
        return "`" + std::string(name) + "`";
    }

    std::string CreateTable(std::string const& table, std::string const& format)
    {
        std::string sql = "CREATE TABLE " + Quote(table) + " (";
        for (std::size_t i = 0; i < format.size(); ++i)
        {
            sql += Quote(LonelyIce::DbcFile::ColumnName(format, i));
            switch (format[i])
            {
                case 'f': sql += " FLOAT"; break;
                case 's': sql += " TEXT"; break;
                case 'b':
                case 'X': sql += " TINYINT UNSIGNED"; break;
                default:  sql += " INT UNSIGNED"; break;
            }
            sql += i == LonelyIce::DbcFile::IndexColumn(format) ? " NOT NULL, " : " NULL, ";
        }
        // No primary key: an index may repeat (WorldMapArea.dbc by area, 0 for every continent); the core keeps one row.
        sql.resize(sql.size() - 2);
        return sql + ")";
    }

    std::string Value(LonelyIce::DbcFile const& file, uint32_t row, std::size_t column)
    {
        if (file.Missing(row, column))
            return "NULL";

        switch (file.Format()[column])
        {
            case 'f':
            {
                float const value = file.Float(row, column);
                return std::isfinite(value) ? Acore::StringFormat("{}", value) : std::string("0");
            }
            case 's':
            {
                std::string text(file.String(row, column));
                WorldDatabase.EscapeString(text);
                return "'" + text + "'";
            }
            default:
                return std::to_string(file.UInt(row, column));
        }
    }

    bool FillTable(LonelyIce::DbcFile const& file, std::string const& table, std::string& error)
    {
        WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
        trans->Append("DROP TABLE IF EXISTS " + Quote(table));
        trans->Append(CreateTable(table, file.Format()));

        std::string const insert = "INSERT INTO " + Quote(table) + " VALUES ";
        std::string sql;
        for (uint32_t row = 0; row < file.Rows(); ++row)
        {
            sql += sql.empty() ? insert + "(" : ",(";
            for (std::size_t column = 0; column < file.Format().size(); ++column)
            {
                if (column)
                    sql += ',';
                sql += Value(file, row, column);
            }
            sql += ')';
            if ((row + 1) % RowsPerInsert == 0 || row + 1 == file.Rows())
            {
                trans->Append(sql);
                sql.clear();
            }
        }
        WorldDatabase.DirectCommitTransaction(trans);

        QueryResult count = WorldDatabase.Query("SELECT COUNT(*) FROM " + Quote(table));
        if (!count || count->Fetch()[0].Get<uint64>() != file.Rows())
        {
            error = "could not write " + table;
            return false;
        }
        return true;
    }
}

namespace LonelyIce::DbcUnpack
{
    bool Fill(ClientArchives::Reader const& archives, Progress const& progress, std::string& error)
    {
        std::vector<DBCFileInfo> const& files = GetDBCFiles();
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            std::string const table = GetDBCTableName(files[i].file);
            if (progress)
                progress(i, files.size(), table);

            std::optional<std::vector<uint8_t>> raw = archives.Read(std::string("DBFilesClient\\") + files[i].file);
            if (!raw)
            {
                error = std::string(files[i].file) + " is not in the client's archives";
                return false;
            }
            std::shared_ptr<DbcFile const> file = DbcFile::Parse(std::move(*raw), files[i].file, files[i].format, error);
            if (!file || !FillTable(*file, table, error))
                return false;
        }
        if (progress)
            progress(files.size(), files.size(), {});
        return true;
    }

    void Drop()
    {
        for (DBCFileInfo const& f : GetDBCFiles())
            WorldDatabase.DirectExecute("DROP TABLE IF EXISTS " + Quote(GetDBCTableName(f.file)));
    }
}
