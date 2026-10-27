#include "ConsoleText.h"
#include "Platform.h"
#include <cassert>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv)
{
    using namespace LonelyIce;
    if (argc > 1)
    {
        std::string mode = argv[1];
        // AppenderConsole writes text + newline, then ResetColor writes ESC[0m without a newline.
        if (mode == "colored") std::printf("\x1b[36mAccount exists\n\x1b[0m@@LI deploy ok\n");
        if (mode == "plain") std::printf("Account exists\n@@LI deploy ok\n");
        if (mode == "missing") std::printf("Account exists\n");
        if (mode == "failure") std::printf("\x1b[0m@@LI state failed database\n");
        return mode == "failure" ? 1 : 0;
    }
    assert(StripConsoleColors("\x1b[0m@@LI deploy ok") == "@@LI deploy ok");
    assert(StripConsoleColors("\x1b[1;36mUTF-8: Привет\x1b[0m") == "UTF-8: Привет");
    assert(StripConsoleColors("\x1b[m@@LI state ready") == "@@LI state ready");
    assert(StripConsoleColors("literal [0m text") == "literal [0m text");
    assert(StripConsoleColors("\x1b[2J") == "\x1b[2J");
    assert(StripConsoleColors("\x1b[0") == "\x1b[0");
    for (std::string mode : {"plain", "colored", "missing", "failure"})
    {
        Platform::Child child;
        Platform::ChildOptions options;
        options.exe = Platform::ExePath();
        options.args = {mode};
        options.pipes = true;
        options.lowPriority = true;
        std::string error;
        assert(child.Start(options, error));
        child.CloseInput();
        std::string pending;
        bool deployed = false, legacyDeployed = false, databaseFailure = false;
        char buffer[3]; // Split escape sequences and the marker across pipe reads.
        auto parse = [&](std::string const& raw)
        {
            if (raw.rfind("@@LI deploy ok", 0) == 0) legacyDeployed = true;
            auto line = StripConsoleColors(raw);
            if (line.rfind("@@LI ", 0) != 0) return;
            deployed |= line == "@@LI deploy ok";
            databaseFailure |= line == "@@LI state failed database";
        };
        for (std::size_t n; (n = child.Read(buffer, sizeof(buffer))) > 0;)
        {
            pending.append(buffer, n);
            for (std::size_t end; (end = pending.find('\n')) != std::string::npos;)
            {
                parse(pending.substr(0, end));
                pending.erase(0, end + 1);
            }
        }
        if (!pending.empty()) parse(pending);
        child.Wait(-1);
        bool expected = mode == "plain" || mode == "colored";
        assert((child.ExitCode() == 0 && deployed) == expected);
        assert(databaseFailure == (mode == "failure"));
        if (mode == "colored") assert(!legacyDeployed && deployed);
        std::cout << "PASS " << mode << ": exit=" << child.ExitCode()
                  << ", old marker=" << legacyDeployed << ", fixed marker=" << deployed << '\n';
    }
}
