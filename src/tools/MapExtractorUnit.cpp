// map_extractor linked into LonelyIce. Its sources share global names with vmap4_extractor (MPQFile, DBCFile, input_path, ...),
// so they are compiled here inside their own namespace. Every header they pull from outside src/tools/map_extractor is
// included first, so the include guards keep those at global scope.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
// System headers System.cpp includes itself (per platform), pulled in here first so they stay at global scope.
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include "Define.h"
#include "StringFormat.h"
#include "libmpq/mpq.h"

namespace MapExtractor
{
#include "map_extractor/System.cpp"
#include "map_extractor/adt.cpp"
#include "map_extractor/wdt.cpp"
#include "map_extractor/dbcfile.cpp"
#include "map_extractor/loadlib.cpp"
#include "map_extractor/mpq_libmpq.cpp"
}

int MapExtractorMain(int argc, char** argv)
{
    return MapExtractor::main(argc, argv);
}
