#include "LauncherRuntime.h"
#include "Lang.h"
#include "Installer.h"
#include <cstdio>
#include <string_view>
#include <vector>

int ServerMain(int argc, char** argv);

int HeadlessMain(int argc, char** argv)
{
    using namespace LonelyIce;
    Platform::UseParentConsole(false, true);
    std::string const overrideLanguage = Platform::GetEnv("LONELYICE_LANG").value_or("");
    Lang::Init();
    LauncherSettings settings;
    settings.file = Platform::ExePath().parent_path() / "server.yaml";
    bool help = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view option(argv[i]);
        if (option == "--headless")
            continue;
        if (option == "--help" || option == "-h")
            help = true;
        else if (option == "--settings" && i + 1 < argc)
        {
            settings.file = Platform::Utf8ToPath(argv[++i]);
        }
        else
        {
            fprintf(stderr, "%s\n", Tr("runtime.headless_usage").c_str());
            return 1;
        }
    }
    std::string error;
    LaunchContext context;
    try
    {
        ResolveSettingsPaths(settings);
        if (help)
        {
            fprintf(stdout, "%s\n", Tr("runtime.headless_usage").c_str());
            return 0;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(settings.file, ec))
        {
            fprintf(stderr, "%s\n", Tr("runtime.settings_unreadable", Platform::PathToUtf8(settings.file)).c_str());
            return 1;
        }
        if (!settings.Load(&error))
        {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (overrideLanguage.empty())
            Lang::Set(settings.language);
        context = CreateLaunchContext(settings);
        std::string const sqlStamp = Installer::SqlStamp(context.exeDir / "setup");
        if (!sqlStamp.empty() && !settings.sqlStamp.empty() && sqlStamp != settings.sqlStamp)
        {
            fprintf(stderr, "%s\n", Tr("runtime.sql_update_required", Platform::PathToUtf8(settings.file)).c_str());
            return 1;
        }
        if (!PrepareLaunch(settings, context, error))
        {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::filesystem::current_path(context.root);
    }
    catch (std::filesystem::filesystem_error const& exception)
    {
        fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
    for (auto const& [name, value] : context.env)
        Platform::SetEnv(name, value);
    Platform::SetEnv("LONELYICE_SERVER_ONLY", "1");
    std::vector<std::string> arguments{ Platform::PathToUtf8(context.exe), "--server", "--no-console", "-c",
        Platform::PathToUtf8(context.config) };
    std::vector<char*> serverArguments;
    for (std::string& argument : arguments)
        serverArguments.push_back(argument.data());
    serverArguments.push_back(nullptr);
    return ServerMain(int(arguments.size()), serverArguments.data());
}
