#include "Lang.h"
#include <string_view>

int LauncherMain(int argc, char** argv);
int ServerMain(int argc, char** argv);
int ToolMain(int argc, char** argv);
int PackMain(int argc, char** argv);
int PkgMain(int argc, char** argv);

// One exe, several roles: the launcher window, or child processes it starts: the auth + world server (--server),
// client data extractors (--tool), the plugin package manager (--pkg), and at build time the release packer (--pack).
int main(int argc, char** argv)
{
    LonelyIce::Lang::Init();
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a == "--server")
            return ServerMain(argc, argv);
        if (a == "--tool")
            return ToolMain(argc, argv);
        if (a == "--pack")
            return PackMain(argc, argv);
        if (a == "--pkg")
            return PkgMain(argc, argv);
    }

    return LauncherMain(argc, argv);
}
