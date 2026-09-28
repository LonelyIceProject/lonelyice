#include "AuthService.h"
#include "AuthSocketMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "IoContext.h"
#include "Log.h"
#include "RealmList.h"
#include "SteadyTimer.h"
#include <memory>
#include <thread>

namespace
{
    std::shared_ptr<Acore::Asio::IoContext> _io;
    std::shared_ptr<boost::asio::steady_timer> _banTimer;
    std::thread _thread;

    void BanExpiryHandler(std::weak_ptr<boost::asio::steady_timer> timerRef, int32 interval, boost::system::error_code const& error)
    {
        if (error)
            return;

        if (std::shared_ptr<boost::asio::steady_timer> timer = timerRef.lock())
        {
            LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_DEL_EXPIRED_IP_BANS));
            LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_UPD_EXPIRED_ACCOUNT_BANS));

            timer->expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(interval));
            timer->async_wait(std::bind(&BanExpiryHandler, timerRef, interval, std::placeholders::_1));
        }
    }
}

bool LonelyIce::AuthService::Start()
{
    _io = std::make_shared<Acore::Asio::IoContext>();

    sRealmList->Initialize(*_io, sConfigMgr->GetOption<int32>("RealmsStateUpdateDelay", 20));
    if (sRealmList->GetRealms().empty())
        LOG_WARN("server.authserver", "Realm list is empty or every realm is offline; the client will see no realms until the world is up");

    int32 port = sConfigMgr->GetOption<int32>("RealmServerPort", 3724);
    if (port <= 0 || port > 0xFFFF)
    {
        LOG_ERROR("server.authserver", "RealmServerPort {} is out of range", port);
        return false;
    }

    std::string bindIp = sConfigMgr->GetOption<std::string>("BindIP", "0.0.0.0");
    if (!sAuthSocketMgr.StartNetwork(*_io, bindIp, port))
    {
        LOG_ERROR("server.authserver", "Failed to listen on {}:{}", bindIp, port);
        sRealmList->Close();
        return false;
    }

    int32 banInterval = sConfigMgr->GetOption<int32>("BanExpiryCheckInterval", 60);
    _banTimer = std::make_shared<boost::asio::steady_timer>(*_io);
    _banTimer->expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(banInterval));
    _banTimer->async_wait(std::bind(&BanExpiryHandler, std::weak_ptr<boost::asio::steady_timer>(_banTimer), banInterval, std::placeholders::_1));

    _thread = std::thread([] { _io->run(); });

    LOG_INFO("server.authserver", "Auth listening on {}:{}", bindIp, port);
    return true;
}

void LonelyIce::AuthService::Stop()
{
    if (!_io)
        return;

    sAuthSocketMgr.StopNetwork();
    if (_banTimer)
        _banTimer->cancel();
    _io->stop();
    if (_thread.joinable())
        _thread.join();

    sRealmList->Close();
    _banTimer.reset();
    _io.reset();
}
