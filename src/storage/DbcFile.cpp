#include "DbcFile.h"
#include <cstring>

namespace
{
    uint32_t U32(uint8_t const* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    bool IsByte(char c) { return c == 'b' || c == 'X'; }
}

namespace LonelyIce
{
    std::shared_ptr<DbcFile const> DbcFile::Parse(std::vector<uint8_t> data, std::string const& name, std::string const& format,
        std::string& error)
    {
        auto file = std::make_shared<DbcFile>();
        file->_data = std::move(data);
        file->_format = format;
        std::vector<uint8_t> const& d = file->_data;
        if (d.size() < 20 || std::memcmp(d.data(), "WDBC", 4) != 0)
        {
            error = name + " is not a DBC file";
            return nullptr;
        }

        file->_records = U32(&d[4]);
        uint32_t const fields = U32(&d[8]);
        file->_recordSize = U32(&d[12]);
        file->_stringSize = U32(&d[16]);

        // A "d" field may be missing from the file (the gt* tables): the row number is the index then.
        bool const rowIndex = fields + 1 == format.size() && format.find('d') != std::string::npos;
        if (fields != format.size() && !rowIndex)
        {
            error = name + " has " + std::to_string(fields) + " fields, the server expects " + std::to_string(format.size()) +
                " (a client of another version?)";
            return nullptr;
        }

        uint32_t offset = 0;
        for (char c : format)
        {
            if (rowIndex && c == 'd')
            {
                file->_offsets.push_back(RowNumber);
                continue;
            }
            file->_offsets.push_back(offset);
            offset += IsByte(c) ? 1 : 4;
        }

        if (20 + uint64_t(file->_records) * file->_recordSize + file->_stringSize > d.size())
        {
            error = name + " is damaged";
            return nullptr;
        }
        return file;
    }

    uint8_t const* DbcFile::Field(uint32_t row, std::size_t column) const
    {
        return _data.data() + 20 + std::size_t(row) * _recordSize + _offsets[column];
    }

    bool DbcFile::Missing(uint32_t /*row*/, std::size_t column) const
    {
        return _offsets[column] != RowNumber && _offsets[column] + (IsByte(_format[column]) ? 1u : 4u) > _recordSize;
    }

    uint32_t DbcFile::UInt(uint32_t row, std::size_t column) const
    {
        if (_offsets[column] == RowNumber)
            return row;
        if (Missing(row, column))
            return 0;
        return IsByte(_format[column]) ? *Field(row, column) : U32(Field(row, column));
    }

    float DbcFile::Float(uint32_t row, std::size_t column) const
    {
        if (Missing(row, column))
            return 0.f;
        float value;
        std::memcpy(&value, Field(row, column), 4);
        return value;
    }

    std::string_view DbcFile::String(uint32_t row, std::size_t column) const
    {
        if (Missing(row, column))
            return {};
        uint32_t const offset = U32(Field(row, column));
        if (offset >= _stringSize)
            return {};
        char const* text = reinterpret_cast<char const*>(_data.data() + 20 + std::size_t(_records) * _recordSize + offset);
        return std::string_view(text, strnlen(text, _stringSize - offset));
    }

    std::size_t DbcFile::IndexColumn(std::string const& format)
    {
        std::size_t const index = format.find_first_of("nd");
        return index == std::string::npos ? 0 : index;
    }

    std::string DbcFile::ColumnName(std::string const& format, std::size_t column)
    {
        return column == IndexColumn(format) ? std::string("ID") : "f" + std::to_string(column);
    }
}
