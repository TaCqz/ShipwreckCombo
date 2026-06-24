#ifndef NETWORK_MULTISHIP_H
#define NETWORK_MULTISHIP_H
#ifdef ENABLE_MULTISHIP
#ifdef __cplusplus

#include <memory>

#include "soh/Network/Network.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

class MultiShip : public Network {

  private:
    // Sends an OnLoadGame packet for the currently loaded MultiShip file.
    void SendOnLoadGame();

  public:
    static MultiShip* Instance;
    virtual ~MultiShip() = default;

    // Entry point invoked by the Network menu's "Connect" button. Toggles the
    // connection (the Network base class does the actual TCP work on its thread).
    void Connect();

    // Registers the GameInteractor hooks. Called once at boot on the main thread
    // (NOT from the network thread); each hook no-ops while disconnected.
    void RegisterHooks();

    void OnIncomingJson(nlohmann::json payload) override;
    void OnConnected() override;

    // Stamps every outgoing packet with the configured user name before sending,
    // so the server can always attribute a message to the player who sent it.
    void SendJsonToRemote(nlohmann::json packet) override;
};

#endif // __cplusplus
#endif // ENABLE_MULTISHIP
#endif // NETWORK_MULTISHIP_H
