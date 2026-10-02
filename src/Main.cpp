#include "Lang.h"
#include "Platform.h"
#include <cstdio>
#include <string_view>

int LauncherMain(int argc, char** argv);
int ServerMain(int argc, char** argv);
int ToolMain(int argc, char** argv);
int PackMain(int argc, char** argv);
int PkgMain(int argc, char** argv);
int BackupMain(int argc, char** argv);
int HeadlessMain(int argc, char** argv);
#ifdef LONELYICE_TUI
int TuiMain(int argc, char** argv);
#endif

// One exe: graphical launcher, terminal configuration (--tui), direct headless launch (--headless),
// or child processes: the auth + world server (--server),
// client data extractors (--tool), the plugin package manager (--pkg), backups (--backup), and at build time the
// release packer (--pack).
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a == "--headless")
            return HeadlessMain(argc, argv);
        if (a == "--tui" || a == "-nw" || a == "--nw")
        {
#ifdef LONELYICE_TUI
            return TuiMain(argc, argv);
#else
            LonelyIce::Lang::Init();
            LonelyIce::Platform::UseParentConsole(false, true);
            std::fprintf(stderr, "%s\n", LonelyIce::Tr("main.no_tui").c_str());
            return 1;
#endif
        }
        if (a == "--server")
        {
            LonelyIce::Lang::Init();
            return ServerMain(argc, argv);
        }
        if (a == "--tool")
        {
            LonelyIce::Lang::Init();
            return ToolMain(argc, argv);
        }
        if (a == "--pack")
        {
            LonelyIce::Lang::Init();
            return PackMain(argc, argv);
        }
        if (a == "--pkg")
        {
            LonelyIce::Lang::Init();
            return PkgMain(argc, argv);
        }
        if (a == "--backup")
        {
            LonelyIce::Lang::Init();
            return BackupMain(argc, argv);
        }
    }

    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h")
        {
            LonelyIce::Lang::Init();
            LonelyIce::Platform::UseParentConsole(false, true);
            std::printf("%s\n", LonelyIce::Tr("main.usage").c_str());
            return 0;
        }
#ifdef LONELYICE_GUI
    LonelyIce::Lang::Init();
    return LauncherMain(argc, argv);
#elif defined(LONELYICE_TUI)
    return TuiMain(argc, argv);
#else
    LonelyIce::Lang::Init();
    LonelyIce::Platform::UseParentConsole(false, true);
    std::fprintf(stderr, "%s\n", LonelyIce::Tr("main.usage").c_str());
    return 1;
#endif
}
