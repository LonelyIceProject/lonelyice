#ifndef LONELYICE_DBCTABLES_H
#define LONELYICE_DBCTABLES_H

#include <memory>
#include <string>

namespace LonelyIce::ClientArchives
{
    class Reader;
}

// The client's DBC files as read-only tables of every SQLite connection the server opens: dbc_spell, dbc_map, ...
// (one per file the core loads, see GetDBCFiles()), read straight from the client's archives. With DBC.FromDatabase
// the core loads its DBC stores from them. A real table of the same name in the database takes precedence.
namespace LonelyIce::DbcTables
{
    // Call before the databases are opened.
    bool Enable(std::shared_ptr<ClientArchives::Reader const> archives, std::string& error);
}

#endif
