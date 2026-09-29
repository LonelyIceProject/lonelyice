#ifndef LONELYICE_DBCUNPACK_H
#define LONELYICE_DBCUNPACK_H

#include <cstddef>
#include <functional>
#include <string>

namespace LonelyIce::ClientArchives
{
    class Reader;
}

// The client's DBC files unpacked into real dbc_* tables of the world database (the same tables DbcTables offers
// virtually), for servers that do not read the client: DBC.FromDatabase loads them. Written through the core's
// database interfaces, so any backend works. The world database must be open.
namespace LonelyIce::DbcUnpack
{
    using Progress = std::function<void(std::size_t done, std::size_t total, std::string const& table)>;

    bool Fill(ClientArchives::Reader const& archives, Progress const& progress, std::string& error);
    void Drop();
}

#endif
