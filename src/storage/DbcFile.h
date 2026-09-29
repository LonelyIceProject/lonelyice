#ifndef LONELYICE_DBCFILE_H
#define LONELYICE_DBCFILE_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LonelyIce
{
    // A DBC file read against the core's format string: one character per field (DBCFileLoader.h), "b"/"X" are
    // bytes, the rest 4 bytes. As a table: one column per format character, the index ("n", or "d") named ID, the
    // others f<position>; a file without an index has ID on its first column.
    class DbcFile
    {
    public:
        static std::shared_ptr<DbcFile const> Parse(std::vector<uint8_t> data, std::string const& name, std::string const& format,
            std::string& error);

        uint32_t Rows() const { return _records; }
        std::string const& Format() const { return _format; }

        // Unused fields may be declared wider than they are (PowerDisplay.dbc): such a value is missing.
        bool Missing(uint32_t row, std::size_t column) const;
        uint32_t UInt(uint32_t row, std::size_t column) const;         // "n", "d", "i", "x", "b", "X"
        float Float(uint32_t row, std::size_t column) const;
        std::string_view String(uint32_t row, std::size_t column) const;

        static std::size_t IndexColumn(std::string const& format);
        static std::string ColumnName(std::string const& format, std::size_t column);

    private:
        static uint32_t constexpr RowNumber = ~0u;  // offset of a "d" the file does not have: the row number is the index

        uint8_t const* Field(uint32_t row, std::size_t column) const;

        std::vector<uint8_t> _data;
        std::string _format;
        uint32_t _records = 0, _recordSize = 0, _stringSize = 0;
        std::vector<uint32_t> _offsets;     // per format character
    };
}

#endif
