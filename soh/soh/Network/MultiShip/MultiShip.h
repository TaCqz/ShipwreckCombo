#ifndef NETWORK_MULTISHIP_H
#define NETWORK_MULTISHIP_H
#ifdef ENABLE_MULTISHIP
#ifdef __cplusplus

#include <memory>

#include "soh/Network/Network.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

class MultiShip : public Network {

  private:
    void RegisterHooks();

  public:
    static MultiShip* Instance;
    virtual ~MultiShip() = default;

    // Entry point invoked by the Network menu's "Connect" button.
    // For now this is a stub that does nothing real (see MultiShip.cpp).
    void Connect();

    void OnIncomingJson(nlohmann::json payload) override;
    void OnConnected() override;
    void OnDisconnected() override;
};

#endif // __cplusplus
#endif // ENABLE_MULTISHIP
#endif // NETWORK_MULTISHIP_H
