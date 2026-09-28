#ifndef LONELYICE_AUTHSERVICE_H
#define LONELYICE_AUTHSERVICE_H

namespace LonelyIce::AuthService
{
    // Needs LoginDatabase, sConfigMgr and sSecretMgr already initialized by the world side.
    bool Start();
    void Stop();
}

#endif
