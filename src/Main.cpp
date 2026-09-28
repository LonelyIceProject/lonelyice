#include <string_view>

int LauncherMain(int argc, char** argv);
int ServerMain(int argc, char** argv);

// One exe, two roles: the launcher window, or (with --server) the auth + world server it runs as a child process.
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--server")
            return ServerMain(argc, argv);

    return LauncherMain(argc, argv);
}
