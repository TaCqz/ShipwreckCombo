#ifndef NETWORK_MULTISHIP_H
#define NETWORK_MULTISHIP_H
#ifdef ENABLE_MULTISHIP
#ifdef __cplusplus

#include <atomic>
#include <memory>

#include "soh/Network/Network.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

class MultiShip : public Network {

  private:
    // Sends an OnLoadGame packet for the currently loaded MultiShip file.
    void SendOnLoadGame();

    // Set on (re)connect (network thread); consumed on the main thread in the
    // OnGameFrameUpdate hook to re-report every already-collected check, so the
    // server catches up on anything collected while we were disconnected.
    std::atomic<bool> mNeedsCheckResync{ false };

  public:
    static MultiShip* Instance;
    virtual ~MultiShip() = default;

    // Entry point invoked by the Network menu's "Connect" button.
    // For now this is a stub that does nothing real (see MultiShip.cpp).
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
