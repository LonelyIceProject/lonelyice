// vmap4_extractor linked into LonelyIce, in its own namespace (see MapExtractorUnit.cpp).

#include <G3D/Quat.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
// System headers vmapexport.cpp includes itself, pulled in here first so they stay at global scope: Windows.h and
// direct.h (mkdir) on Windows, sys/stat.h (mkdir, stat) everywhere.
#ifdef _WIN32
#include <Windows.h>
#include <direct.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include "Define.h"
#include "libmpq/mpq.h"

namespace VmapExtractor
{
#include "vmap4_extractor/vmapexport.cpp"
#include "vmap4_extractor/adtfile.cpp"
#include "vmap4_extractor/dbcfile.cpp"
#include "vmap4_extractor/gameobject_extract.cpp"
#include "vmap4_extractor/model.cpp"
#include "vmap4_extractor/mpq_libmpq.cpp"
#include "vmap4_extractor/wdtfile.cpp"
#include "vmap4_extractor/wmo.cpp"
}

int VmapExtractorMain(int argc, char** argv)
{
    return VmapExtractor::main(argc, argv);
}
