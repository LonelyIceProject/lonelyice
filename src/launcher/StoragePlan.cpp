#include "StoragePlan.h"

namespace fs = std::filesystem;
using namespace LonelyIce;

bool LonelyIce::HasUnpackedFiles(fs::path const& dataDir)
{
    std::error_code ec;
    if (fs::is_directory(dataDir / "Cameras", ec))
        return true;
    if (fs::exists(dataDir / "maps" / "stamp.txt", ec))
        return false;
    for (fs::directory_iterator it(dataDir / "maps", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".map")
            return true;
    return false;
}

StoragePlan LonelyIce::PlanStorage(StorageState const& state, bool cache, fs::path const& dataDir, bool sqlChanged)
{
    StoragePlan plan;
    plan.newDatabases = !state.databases;
    plan.db = plan.newDatabases || sqlChanged;
    bool const files = HasUnpackedFiles(dataDir);
    if (cache)
        plan.unpack = !state.HasDbc() || !files;
    else
        plan.pack = state.dbcTables > 0 || files;
    return plan;
}
