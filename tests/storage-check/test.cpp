#include "StorageCheck.h"
#include "Lang.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#endif

std::string LonelyIce::Lang::Get(std::string_view key)
{
    if (key == "storage.error.exit_details")
        return "Exit {0}: {1}";
    if (key == "storage.error.exit")
        return "Exit {0}";
    return std::string(key);
}

int main(int argc, char**)
{
    using namespace LonelyIce;
    if (argc > 1)
    {
        std::string mode = Platform::GetEnv("STORAGE_TEST_MODE").value_or("");
        if (mode == "partial")
        {
            std::puts("@@LI check db auth ok\n@@LI check db characters missing\n@@LI check db world ok\n@@LI check done");
            return 0;
        }
        if (mode != "fail")
        {
            std::puts("@@LI check db auth ok\n@@LI check db characters ok\n@@LI check db world ok\n@@LI check done");
            std::fflush(stdout);
        }
        if (mode == "ok")
            return 0;
        std::fputs("ASSERTION FAILED: storage probe context", stderr);
        std::fflush(stderr);
#ifdef _WIN32
        ExitProcess(0xC0000420);
#endif
        return 42;
    }
    for (std::string const mode : {"ok", "partial", "fail", "fail-after-done", "missing-connection"})
    {
        StorageCheck check([] {});
        Platform::Env env = {{"STORAGE_TEST_MODE", mode}};
        if (mode != "missing-connection")
            for (char const* key : { "AC_LOGIN_DATABASE_INFO", "AC_CHARACTER_DATABASE_INFO", "AC_WORLD_DATABASE_INFO" })
                env.emplace_back(key, "test-connection");
        check.Start(Platform::ExePath(), env);
        std::optional<StorageState> state;
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!(state = check.Take()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!state)
            throw std::runtime_error("Check did not finish");
        if (mode == "missing-connection")
        {
            if (state->reached || state->databases || state->error != "storage.error.connection_missing")
                throw std::runtime_error("Missing connection was not rejected before spawning the probe");
        }
        else if (mode == "ok")
        {
            if (!state->reached || !state->databases || !state->auth || !state->characters || !state->world
                || !state->error.empty())
                throw std::runtime_error("Successful probe rejected");
        }
        else if (mode == "partial")
        {
            if (!state->reached || state->databases || !state->auth || state->characters || !state->world)
                throw std::runtime_error("Partial database installation was not reported correctly");
        }
        else if (state->reached || state->databases || state->error.find("ASSERTION FAILED") == std::string::npos
            || state->error.find("@@LI") != std::string::npos)
            throw std::runtime_error("Failure context lost or crash accepted as success");
    }
    std::puts("Storage check diagnostics: OK");
}
