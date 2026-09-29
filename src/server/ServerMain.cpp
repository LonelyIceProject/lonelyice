// World + auth in one process. Launched by the LonelyIce UI as "LonelyIce --server -c <worldserver.conf>".
// stdout carries the log plus "@@LI ..." control lines; stdin takes console commands, one per line.

#include "ACSoap.h"
#include "AppenderDB.h"
#include "AuthService.h"
#include "Banner.h"
#include "BattlegroundMgr.h"
#include "BigNumber.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLibrary.h"
#include "DatabaseLoader.h"
#include "ClientArchives.h"
#include "ClientData.h"
#include "DbcUnpack.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "IoContext.h"
#include "MapMgr.h"
#include "Metric.h"
#include "ModuleMgr.h"
#include "ModulesScriptLoader.h"
#include "OpenSSLCrypto.h"
#include "OutdoorPvPMgr.h"
#include "Platform.h"
#include "PluginMgr.h"
#include "PluginPatches.h"
#include "ProcessPriority.h"
#include "RealmList.h"
#include "Resolver.h"
#include "ScriptLoader.h"
#include "ScriptMgr.h"
#include "SecretMgr.h"
#include "SRP6.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "SharedDefines.h"
#include "SteadyTimer.h"
#include "TC9Sidecar.h"
#include "UpdateTime.h"
#include "Util.h"
#include "World.h"
#include "WorldSessionMgr.h"
#include "WorldSocket.h"
#include "WorldSocketMgr.h"
#include <atomic>
#include <map>
#include <set>
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifndef _ACORE_CORE_CONFIG
#define _ACORE_CORE_CONFIG "worldserver.conf"
#endif

namespace fs = std::filesystem;

namespace
{
    std::mutex _outLock;

    void Control(std::string const& line)
    {
        std::lock_guard<std::mutex> guard(_outLock);
        fprintf(stdout, "@@LI %s\n", line.c_str());
        fflush(stdout);
    }

    void SetupStdio()
    {
        LonelyIce::Platform::UseParentConsole(true);

        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }

    void PrintCommandOutput(void* /*arg*/, std::string_view text)
    {
        std::lock_guard<std::mutex> guard(_outLock);
        std::string s(text);
        utf8printf(stdout, "%s", s.c_str());
    }

    void CommandFinished(void* /*arg*/, bool success)
    {
        Control(success ? "done ok" : "done fail");
    }

    // Tab-separated rows for the launcher's Accounts tab and the character picker; names stay UTF-8.
    void ReportAccounts()
    {
        std::map<uint32, uint32> charCount;
        if (QueryResult r = CharacterDatabase.Query("SELECT account, COUNT(*) FROM characters GROUP BY account"))
        {
            do
                charCount[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint32>();
            while (r->NextRow());
        }

        std::set<uint32> humanAccounts;
        if (QueryResult r = LoginDatabase.Query(
                "SELECT a.id, a.username, COALESCE(MAX(aa.gmlevel), 0), a.last_login FROM account a "
                "LEFT JOIN account_access aa ON aa.id = a.id GROUP BY a.id, a.username, a.last_login ORDER BY a.id"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 id = f[0].Get<uint32>();
                std::string name = f[1].Get<std::string>();
                bool bot = name.rfind("RNDBOT", 0) == 0;
                if (!bot)
                    humanAccounts.insert(id);
                Control(Acore::StringFormat("acc {}\t{}\t{}\t{}\t{}\t{}", id, name, f[2].Get<uint32>(), charCount[id],
                    f[3].IsNull() ? std::string() : f[3].Get<std::string>(), bot ? 1 : 0));
            } while (r->NextRow());
        }

        if (QueryResult r = CharacterDatabase.Query("SELECT name, account, level, online FROM characters ORDER BY name"))
        {
            do
            {
                Field* f = r->Fetch();
                if (!humanAccounts.count(f[1].Get<uint32>()))
                    continue;
                Control(Acore::StringFormat("char {}\t{}\t{}\t{}", f[0].Get<std::string>(), f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>()));
            } while (r->NextRow());
        }
        Control("accend");
    }

    void SetRealmName(std::string name)
    {
        LoginDatabase.EscapeString(name);
        LoginDatabase.DirectExecute("UPDATE realmlist SET name = '{}' WHERE id = {}", name, realm.Id.Realm);
        Control("realmname ok");
    }

    std::string Env(char const* name)
    {
        return LonelyIce::Platform::GetEnv(name).value_or(std::string());
    }

    // First-run setup after the databases were created: the player's account (LONELYICE_ACCOUNT = login\tpassword\tgmlevel)
    // and the realm name (LONELYICE_REALMNAME). Runs without the world loaded, so nothing here may touch sWorld.
    bool DeploySetup()
    {
        std::string account = Env("LONELYICE_ACCOUNT");
        if (!account.empty())
        {
            std::vector<std::string_view> parts = Acore::Tokenize(account, '\t', true);
            if (parts.size() < 2)
                return false;
            std::string user(parts[0]), pass(parts[1]);
            uint32 gm = parts.size() > 2 ? Acore::StringTo<uint32>(parts[2]).value_or(0) : 0;
            Utf8ToUpperOnlyLatin(user);
            Utf8ToUpperOnlyLatin(pass);

            if (!LoginDatabase.Query("SELECT id FROM account WHERE username = '{}'", user))
            {
                // The account statements are prepared for the async connection only, so plain SQL here.
                auto [salt, verifier] = Acore::Crypto::SRP6::MakeRegistrationData(user, pass);
                LoginDatabase.DirectExecute("INSERT INTO account (username, salt, verifier, expansion, reg_mail, email, joindate) "
                    "VALUES ('{}', X'{}', X'{}', {}, '', '', CURRENT_TIMESTAMP)", user, ByteArrayToHexStr(salt), ByteArrayToHexStr(verifier),
                    uint32(EXPANSION_WRATH_OF_THE_LICH_KING));
                LoginDatabase.DirectExecute("INSERT INTO realmcharacters (realmid, acctid, numchars) SELECT realmlist.id, account.id, 0 "
                    "FROM realmlist, account LEFT JOIN realmcharacters ON acctid = account.id WHERE acctid IS NULL");
                LOG_INFO("server.worldserver", "Account {} created", user);
            }
            if (QueryResult r = LoginDatabase.Query("SELECT id FROM account WHERE username = '{}'", user))
            {
                uint32 id = r->Fetch()[0].Get<uint32>();
                LoginDatabase.DirectExecute("DELETE FROM account_access WHERE id = {}", id);
                if (gm)
                    LoginDatabase.DirectExecute("INSERT INTO account_access (id, gmlevel, RealmID) VALUES ({}, {}, -1)", id, gm);
            }
            else
                return false;
        }

        std::string realmName = Env("LONELYICE_REALMNAME");
        if (!realmName.empty())
        {
            LoginDatabase.EscapeString(realmName);
            LoginDatabase.DirectExecute("UPDATE realmlist SET name = '{}' WHERE id = {}", realmName, sConfigMgr->GetOption<uint32>("RealmID", 1));
        }
        return true;
    }

    // Patches of the installed plugins: named ids, server rows of DBC tables, recipe SQL and, when LONELYICE_CLIENT
    // names the game folder, the client archives. Runs before the world loads the DBC stores.
    bool ApplyPluginPatches()
    {
        LonelyIce::PluginPatches::Options o;
        o.pluginsDir = sConfigMgr->GetOption<std::string>("PluginsDir", "plugins");
        o.serverDbcDir = fs::path(sConfigMgr->GetOption<std::string>("DataDir", "./")) / "dbc";
        o.clientDir = fs::u8path(Env("LONELYICE_CLIENT"));
        for (PluginInfo const& plugin : sPluginMgr->GetPlugins())
            if (plugin.loaded)
                o.loaded.insert(plugin.id);

        LonelyIce::PluginPatches::Result r = LonelyIce::PluginPatches::Apply(o);
        for (std::string const& line : r.log)
            LOG_INFO("server.loading", "Plugin patches: {}", line);
        if (!r.ok)
            LOG_ERROR("server.loading", "Plugin patches failed: {}", r.error);
        return r.ok;
    }

    // LONELYICE_DATA=client: DBC data, terrain and cameras come from the game client's archives (LONELYICE_CLIENT,
    // locale LONELYICE_LOCALE or the client's first one) instead of extracted files.
    bool UseClientData()
    {
        if (Env("LONELYICE_DATA") != "client")
            return true;

        std::string error;
        fs::path const data = fs::u8path(sConfigMgr->GetOption<std::string>("DataDir", "./"));
        if (!LonelyIce::ClientData::Enable(fs::u8path(Env("LONELYICE_CLIENT")), Env("LONELYICE_LOCALE"), data, error))
        {
            LOG_ERROR("server.loading", "Client data: {}", error);
            return false;
        }

        LonelyIce::Platform::SetEnv("AC_DBC_FROM_DATABASE", "1");
        LOG_INFO("server.loading", "Client data: DBC tables, terrain and cameras read from the game client ({})", LonelyIce::ClientData::Locale());
        return true;
    }

    // --dbc fill: the client's DBC files (LONELYICE_CLIENT, LONELYICE_LOCALE) into dbc_* tables of the world database,
    // for servers that run unpacked; --dbc drop removes them again. Progress as "@@LI dbc <done> <total> <table>".
    bool UnpackDbc(std::string const& action)
    {
        if (action == "drop")
        {
            LonelyIce::DbcUnpack::Drop();
            LOG_INFO("server.loading", "DBC tables removed from the world database");
            return true;
        }

        fs::path const client = fs::u8path(Env("LONELYICE_CLIENT"));
        std::string locale = Env("LONELYICE_LOCALE");
        if (locale.empty())
        {
            std::vector<std::string> const locales = LonelyIce::ClientArchives::Locales(client);
            if (!locales.empty())
                locale = locales.front();
        }
        LonelyIce::ClientArchives::Reader archives(client, locale);
        if (locale.empty() || !archives.IsOpen())
        {
            LOG_ERROR("server.loading", "Cannot open the archives of the game client in {}", client.string());
            return false;
        }

        std::string error;
        bool const ok = LonelyIce::DbcUnpack::Fill(archives, [](std::size_t done, std::size_t total, std::string const& table)
        {
            Control(Acore::StringFormat("dbc {} {} {}", done, total, table));
        }, error);
        if (!ok)
            LOG_ERROR("server.loading", "Unpacking the DBC files failed: {}", error);
        else
            LOG_INFO("server.loading", "DBC files of the {} client unpacked into the world database", locale);
        return ok;
    }

    // Windows: raw ReadFile instead of std::cin, so the thread can be cancelled with CancelSynchronousIo without holding
    // CRT locks. POSIX: read() after poll() with a short timeout, so Stop() ends the thread by setting the flag.
    class CommandReader
    {
    public:
        void Start()
        {
            _thread = std::thread([this] { Run(); });
        }

        void Stop()
        {
            _stop = true;
            if (!_thread.joinable())
                return;
#ifdef _WIN32
            for (int i = 0; i < 50; ++i)
            {
                CancelSynchronousIo(_thread.native_handle());
                if (WaitForSingleObject(_thread.native_handle(), 20) == WAIT_OBJECT_0)
                {
                    _thread.join();
                    return;
                }
            }
            _thread.detach();
#else
            _thread.join();
#endif
        }

    private:
        enum class ReadResult
        {
            Data,
            Nothing,        // timeout or interrupted: check the flag and try again
            End             // end of input or an error
        };

#ifdef _WIN32
        static bool InputAvailable()
        {
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            return in && in != INVALID_HANDLE_VALUE;
        }

        static ReadResult ReadInput(char* buf, std::size_t size, std::size_t& read)
        {
            DWORD n = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, DWORD(size), &n, nullptr) || n == 0)
                return ReadResult::End;
            read = n;
            return ReadResult::Data;
        }
#else
        static bool InputAvailable()
        {
            return fcntl(STDIN_FILENO, F_GETFD) != -1;
        }

        static ReadResult ReadInput(char* buf, std::size_t size, std::size_t& read)
        {
            pollfd pfd{};
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            int const ready = poll(&pfd, 1, 100);
            if (ready == 0 || (ready < 0 && errno == EINTR))
                return ReadResult::Nothing;
            if (ready < 0)
                return ReadResult::End;
            // POLLHUP / POLLERR without data: read() returns 0 or fails, which is the end of input
            ssize_t const n = ::read(STDIN_FILENO, buf, size);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                return ReadResult::Nothing;
            if (n <= 0)
                return ReadResult::End;
            read = std::size_t(n);
            return ReadResult::Data;
        }
#endif

        void Run()
        {
            if (!InputAvailable())
                return;

            std::string pending;
            char buf[4096];
            while (!_stop)
            {
                std::size_t read = 0;
                ReadResult const r = ReadInput(buf, sizeof(buf), read);
                if (r == ReadResult::Nothing)
                    continue;
                if (r == ReadResult::End)
                {
                    // Launcher went away: save everyone and stop.
                    if (!_stop && !World::IsStopped())
                        World::StopNow(SHUTDOWN_EXIT_CODE);
                    return;
                }

                pending.append(buf, read);
                std::size_t eol;
                while ((eol = pending.find('\n')) != std::string::npos)
                {
                    std::string line = pending.substr(0, eol);
                    pending.erase(0, eol + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    if (line.empty())
                        continue;

                    if (line == "@@quit")
                    {
                        World::StopNow(SHUTDOWN_EXIT_CODE);
                        continue;
                    }
                    if (line == "@@accounts")
                    {
                        ReportAccounts();
                        continue;
                    }
                    if (line.rfind("@@realmname ", 0) == 0)
                    {
                        SetRealmName(line.substr(12));
                        continue;
                    }

                    sWorld->QueueCliCommand(new CliCommandHolder(nullptr, line.c_str(), &PrintCommandOutput, &CommandFinished));
                }
            }
        }

        std::thread _thread;
        std::atomic<bool> _stop{ false };
    };

    class StatusReporter
    {
    public:
        explicit StatusReporter(Acore::Asio::IoContext& io) : _timer(io) { }

        static void Start(std::shared_ptr<StatusReporter> const& self)
        {
            Arm(self);
        }

        void Cancel() { _timer.cancel(); }

    private:
        static void Arm(std::shared_ptr<StatusReporter> const& self)
        {
            self->_timer.expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(2));
            self->_timer.async_wait([ref = std::weak_ptr<StatusReporter>(self)](boost::system::error_code const& error)
            {
                if (error)
                    return;
                if (std::shared_ptr<StatusReporter> me = ref.lock())
                {
                    // Bots have no client socket: active sessions are real players, player count includes bots.
                    Control(Acore::StringFormat("stat players={} chars={} uptime={} diff={}",
                        sWorldSessionMgr->GetActiveSessionCount(), sWorldSessionMgr->GetPlayerCount(),
                        GameTime::GetUptime().count(), sWorldUpdateTime.GetAverageUpdateTime()));
                    Arm(me);
                }
            });
        }

        boost::asio::steady_timer _timer;
    };

    bool StartDB()
    {
        DatabaseLibrary::Init();

        DatabaseLoader loader("server.worldserver", DatabaseLoader::DATABASE_MASK_ALL, AC_MODULES_LIST);
        loader
            .AddDatabase(LoginDatabase, "Login")
            .AddDatabase(CharacterDatabase, "Character")
            .AddDatabase(WorldDatabase, "World");

        if (!loader.Load())
            return false;

        if (!sScriptMgr->OnModuleDatabasesLoading())
            return false;

        realm.Id.Realm = sConfigMgr->GetOption<uint32>("RealmID", 1);
        if (!realm.Id.Realm || realm.Id.Realm > 255)
        {
            LOG_ERROR("server.worldserver", "RealmID must be in 1..255");
            return false;
        }

        LOG_INFO("server.loading", "Loading World Information...");
        LOG_INFO("server.loading", "> RealmID:              {}", realm.Id.Realm);

        LoginDatabase.DirectExecute("UPDATE account SET online = 0 WHERE online = {}", realm.Id.Realm);
        CharacterDatabase.DirectExecute("UPDATE characters SET online = 0 WHERE online <> 0");

        WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_UPD_VERSION);
        stmt->SetData(0, GitRevision::GetFullVersion());
        stmt->SetData(1, GitRevision::GetHash());
        WorldDatabase.Execute(stmt);

        sWorld->LoadDBVersion();
        LOG_INFO("server.loading", "> Version DB world:     {}", sWorld->GetDBVersion());

        sScriptMgr->OnAfterDatabasesLoaded(loader.GetUpdateFlags());
        return true;
    }

    void StopDB()
    {
        CharacterDatabase.Close();
        WorldDatabase.Close();
        LoginDatabase.Close();

        sScriptMgr->OnModuleDatabasesClosing();

        DatabaseLibrary::End();
    }

    bool LoadRealmInfo(Acore::Asio::IoContext& ioContext)
    {
        QueryResult result = LoginDatabase.Query("SELECT id, name, address, localAddress, localSubnetMask, port, icon, flag, timezone, allowedSecurityLevel, population, gamebuild FROM realmlist WHERE id = {}", realm.Id.Realm);
        if (!result)
            return false;

        Acore::Asio::Resolver resolver(ioContext);
        Field* fields = result->Fetch();
        realm.Name = fields[1].Get<std::string>();

        auto resolve = [&](std::string const& host) -> std::unique_ptr<boost::asio::ip::address>
        {
            Optional<boost::asio::ip::tcp::endpoint> ep = resolver.Resolve(boost::asio::ip::tcp::v4(), host, "");
            if (!ep)
            {
                LOG_ERROR("server.worldserver", "Could not resolve address {}", host);
                return nullptr;
            }
            return std::make_unique<boost::asio::ip::address>(ep->address());
        };

        realm.ExternalAddress = resolve(fields[2].Get<std::string>());
        realm.LocalAddress = resolve(fields[3].Get<std::string>());
        realm.LocalSubnetMask = resolve(fields[4].Get<std::string>());
        if (!realm.ExternalAddress || !realm.LocalAddress || !realm.LocalSubnetMask)
            return false;

        realm.Port = fields[5].Get<uint16>();
        realm.Type = fields[6].Get<uint8>();
        realm.Flags = RealmFlags(fields[7].Get<uint8>());
        realm.Timezone = fields[8].Get<uint8>();
        realm.AllowedSecurityLevel = AccountTypes(fields[9].Get<uint8>());
        realm.PopulationLevel = fields[10].Get<float>();
        realm.Build = fields[11].Get<uint32>();
        return true;
    }

    void WorldUpdateLoop()
    {
        uint32 minUpdateDiff = uint32(sConfigMgr->GetOption<int32>("MinWorldUpdateTime", 1));
        uint32 realPrevTime = getMSTime();

        LoginDatabase.WarnAboutSyncQueries(true);
        CharacterDatabase.WarnAboutSyncQueries(true);
        WorldDatabase.WarnAboutSyncQueries(true);
        sScriptMgr->OnDatabaseWarnAboutSyncQueries(true);

        while (!World::IsStopped())
        {
            ++World::m_worldLoopCounter;
            uint32 realCurrTime = getMSTime();

            uint32 diff = getMSTimeDiff(realPrevTime, realCurrTime);
            if (diff < minUpdateDiff)
            {
                std::this_thread::sleep_for(Milliseconds(minUpdateDiff - diff));
                continue;
            }

            sWorld->Update(diff);
            realPrevTime = realCurrTime;
        }

        sScriptMgr->OnDatabaseWarnAboutSyncQueries(false);
        LoginDatabase.WarnAboutSyncQueries(false);
        CharacterDatabase.WarnAboutSyncQueries(false);
        WorldDatabase.WarnAboutSyncQueries(false);
    }

    fs::path ParseConfigPath(int argc, char** argv)
    {
        fs::path configFile = fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_CORE_CONFIG));
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string_view(argv[i]) == "-c" || std::string_view(argv[i]) == "--config")
                configFile = argv[i + 1];
        return configFile;
    }

#ifdef _WIN32
    // Highest timer resolution the system allows; restored on scope exit.
    struct TimerResolution
    {
        TimerResolution()
        {
            TIMECAPS caps;
            if (timeGetDevCaps(&caps, sizeof(caps)) == TIMERR_NOERROR)
            {
                _period = std::min(std::max(caps.wPeriodMin, 1u), caps.wPeriodMax);
                timeBeginPeriod(_period);
            }
        }
        ~TimerResolution()
        {
            if (_period)
                timeEndPeriod(_period);
        }
        UINT _period = 0;
    };
#else
    // POSIX sleeps are precise enough already.
    struct TimerResolution
    {
    };
#endif

    void SetEnvironment(char const* name, char const* value)
    {
#ifdef _WIN32
        // _putenv_s updates the CRT copy that getenv reads as well as the process environment.
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
}

int ServerMain(int argc, char** argv)
{
    SetupStdio();
    Control("state starting");

    // Nobody can answer "create the database?" on this process's stdin.
    SetEnvironment("AC_DISABLE_INTERACTIVE", "1");
    bool deploy = false, applyOnly = false;
    std::string dbcAction;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--deploy")
            deploy = true;
        if (std::string_view(argv[i]) == "--apply")
            applyOnly = true;
        if (std::string_view(argv[i]) == "--dbc" && i + 1 < argc)
            dbcAction = argv[i + 1];
    }

    Acore::Impl::CurrentServerProcessHolder::_type = SERVER_PROCESS_WORLDSERVER;
    signal(SIGABRT, &Acore::AbortHandler);
#ifndef _WIN32
    // Log lines written after the launcher closed our stdout must not kill the process while it saves.
    signal(SIGPIPE, SIG_IGN);
#endif

    TimerResolution timerResolution;
    (void)timerResolution;

    fs::path configFile = ParseConfigPath(argc, argv);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        if (std::string_view(argv[i]) != "--server")
            args.emplace_back(argv[i]);

    sConfigMgr->Configure(configFile.generic_string(), args, CONFIG_FILE_LIST);
    if (!sConfigMgr->LoadAppConfigs())
    {
        Control("state failed config");
        return 1;
    }

    std::shared_ptr<Acore::Asio::IoContext> ioContext = std::make_shared<Acore::Asio::IoContext>();

    sLog->RegisterAppender<AppenderDB>();
    sLog->Initialize(sConfigMgr->GetOption<bool>("Log.Async.Enable", false) ? ioContext.get() : nullptr);

    Acore::Banner::Show("LonelyIce",
        [](std::string_view text) { LOG_INFO("server.worldserver", text); },
        []()
        {
            LOG_INFO("server.worldserver", "> Using configuration file       {}", sConfigMgr->GetFilename());
            LOG_INFO("server.worldserver", "> Using SSL version:             {} (library: {})", OPENSSL_VERSION_TEXT, OpenSSL_version(OPENSSL_VERSION));
            LOG_INFO("server.worldserver", "> Using Boost version:           {}.{}.{}", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        });

    if (!sToCloud9Sidecar->CheckLibsidecarAbi())
        return 1;

    OpenSSLCrypto::threadsSetup();
    std::shared_ptr<void> opensslHandle(nullptr, [](void*) { OpenSSLCrypto::threadsCleanup(); });

    BigNumber seed;
    seed.SetRand(16 * 8);

    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
#ifdef _WIN32
    signals.add(SIGBREAK);
#else
    signals.add(SIGHUP);
#endif
    signals.async_wait([](boost::system::error_code const& error, int)
    {
        if (!error)
            World::StopNow(SHUTDOWN_EXIT_CODE);
    });

    int numThreads = std::max(1, sConfigMgr->GetOption<int32>("ThreadPool", 2));
    std::shared_ptr<std::vector<std::thread>> threadPool(new std::vector<std::thread>(), [ioContext](std::vector<std::thread>* del)
    {
        ioContext->stop();
        for (std::thread& thr : *del)
            thr.join();
        delete del;
    });

    for (int i = 0; i < numThreads; ++i)
        threadPool->push_back(std::thread([ioContext]() { ioContext->run(); }));

    SetProcessPriority("server.worldserver", sConfigMgr->GetOption<int32>(CONFIG_PROCESSOR_AFFINITY, 0), sConfigMgr->GetOption<bool>(CONFIG_HIGH_PRIORITY, true));

    // Plugins register their configs and SQL folders, so they load before module configs and databases.
    sPluginMgr->Load(sConfigMgr->GetOption<std::string>("PluginsDir", "plugins"));
    sConfigMgr->LoadModulesConfigs();

    sScriptMgr->SetScriptLoader(AddScripts);
    sScriptMgr->SetModulesLoader([]()
    {
        AddModulesScripts();
        sPluginMgr->AddScripts();
    });

    std::shared_ptr<void> sScriptMgrHandle(nullptr, [](void*) { sScriptMgr->Unload(); });

    LOG_INFO("server.loading", "Initializing Scripts...");
    sScriptMgr->Initialize();

    if (!UseClientData())
    {
        Control("state failed client");
        return 1;
    }

    if (!StartDB())
    {
        Control("state failed database");
        return 1;
    }

    std::shared_ptr<void> dbHandle(nullptr, [](void*) { StopDB(); });

    if (!dbcAction.empty())
    {
        bool const unpacked = UnpackDbc(dbcAction);
        Control(unpacked ? "dbc ok" : "dbc failed");
        return unpacked ? 0 : 1;
    }

    bool const patched = ApplyPluginPatches();
    if (applyOnly)
    {
        Control(patched ? "apply ok" : "apply failed");
        return patched ? 0 : 1;
    }

    if (deploy)
    {
        bool ok = DeploySetup() && patched;
        Control(ok ? "deploy ok" : "deploy failed account");
        return ok ? 0 : 1;
    }

    LoginDatabase.DirectExecute("UPDATE realmlist SET flag = (flag & ~{}) | {} WHERE id = '{}'", REALM_FLAG_OFFLINE, REALM_FLAG_VERSION_MISMATCH, realm.Id.Realm);

    if (!LoadRealmInfo(*ioContext))
    {
        LOG_ERROR("server.worldserver", "Realm {} is missing from auth.realmlist", realm.Id.Realm);
        Control("state failed realm");
        return 1;
    }

    sMetric->Initialize(realm.Name, *ioContext, []() { METRIC_VALUE("online_players", sWorldSessionMgr->GetPlayerCount()); });
    std::shared_ptr<void> sMetricHandle(nullptr, [](void*) { sMetric->Unload(); });

    static std::string enabledModules = []
    {
        std::string list = AC_MODULES_LIST;
        for (PluginInfo const& plugin : sPluginMgr->GetPlugins())
            if (plugin.loaded)
                list += (list.empty() ? "" : ",") + plugin.id;
        return list;
    }();
    Acore::Module::SetEnableModulesList(enabledModules);

    sSecretMgr->Initialize();

    if (!LonelyIce::AuthService::Start())
    {
        Control("state failed auth");
        return 1;
    }

    std::shared_ptr<void> authHandle(nullptr, [](void*) { LonelyIce::AuthService::Stop(); });

    Control("state loading");
    sWorld->SetInitialWorldSettings();

    std::shared_ptr<void> mapManagementHandle(nullptr, [](void*)
    {
        sBattlegroundMgr->DeleteAllBattlegrounds();
        sOutdoorPvPMgr->Die();
        sMapMgr->UnloadAll();
        sScriptMgr->OnAfterUnloadAllMaps();
    });

    std::shared_ptr<std::thread> soapThread;
    if (sConfigMgr->GetOption<bool>("SOAP.Enabled", false))
    {
        soapThread.reset(new std::thread(ACSoapThread, sConfigMgr->GetOption<std::string>("SOAP.IP", "127.0.0.1"), uint16(sConfigMgr->GetOption<int32>("SOAP.Port", 7878))),
            [](std::thread* thr)
            {
                thr->join();
                delete thr;
            });
    }

    uint16 worldPort = uint16(sWorld->getIntConfig(CONFIG_PORT_WORLD));
    std::string worldListener = sConfigMgr->GetOption<std::string>("BindIP", "0.0.0.0");
    int networkThreads = std::max(1, sConfigMgr->GetOption<int32>("Network.Threads", 1));

    if (!sWorldSocketMgr.StartWorldNetwork(*ioContext, worldListener, worldPort, networkThreads))
    {
        LOG_ERROR("server.worldserver", "Failed to initialize network");
        Control("state failed network");
        World::StopNow(ERROR_EXIT_CODE);
        return 1;
    }

    std::shared_ptr<void> sWorldSocketMgrHandle(nullptr, [](void*)
    {
        sWorldSessionMgr->KickAll();
        sWorldSessionMgr->UpdateSessions(1);
        sWorldSocketMgr.StopNetwork();

        LoginDatabase.DirectExecute("UPDATE account SET online = 0 WHERE online = {}", realm.Id.Realm);
        CharacterDatabase.DirectExecute("UPDATE characters SET online = 0 WHERE online <> 0");
    });

    LoginDatabase.DirectExecute("UPDATE realmlist SET flag = flag & ~{}, population = 0 WHERE id = '{}'", REALM_FLAG_VERSION_MISMATCH, realm.Id.Realm);
    realm.PopulationLevel = 0.0f;
    realm.Flags = RealmFlags(realm.Flags & ~uint32(REALM_FLAG_VERSION_MISMATCH));

    LOG_INFO("server.worldserver", "{} (LonelyIce) ready...", GitRevision::GetFullVersion());

    sScriptMgr->OnStartup();

    static CommandReader commands;
    commands.Start();

    std::shared_ptr<StatusReporter> status = std::make_shared<StatusReporter>(*ioContext);
    StatusReporter::Start(status);

    Control(Acore::StringFormat("state ready port={} realm={}", worldPort, realm.Name));

    WorldUpdateLoop();

    Control("state stopping");
    status->Cancel();
    status.reset();
    commands.Stop();

    threadPool.reset();

    sLog->SetSynchronous();
    sScriptMgr->OnShutdown();

    LoginDatabase.DirectExecute("UPDATE realmlist SET flag = flag | {} WHERE id = '{}'", REALM_FLAG_OFFLINE, realm.Id.Realm);

    LOG_INFO("server.worldserver", "Halting process...");
    return World::GetExitCode();
}
