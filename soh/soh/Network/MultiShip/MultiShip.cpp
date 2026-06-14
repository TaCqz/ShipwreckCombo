#include "MultiShip.h"

#ifdef ENABLE_MULTISHIP

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>
#include "soh/ShipUtils.h"
#include "soh/cvar_prefixes.h"

extern "C" {
extern SaveContext gSaveContext;
}

void MultiShip::Connect() {
    // The "Connect" menu button toggles the connection. The underlying Network
    // base class handles the actual TCP connection (and auto-reconnect) on its
    // own thread; we just feed it the configured host/port.
    if (isEnabled) {
        SPDLOG_INFO("[MultiShip] Disconnecting from server");
        Network::Disable();
        return;
    }

    std::string host = CVarGetString(CVAR_REMOTE_MULTISHIP("Host"), "127.0.0.1");
    uint16_t port = CVarGetInteger(CVAR_REMOTE_MULTISHIP("Port"), 43384);
    SPDLOG_INFO("[MultiShip] Connecting to {}:{}", host, port);
    Network::Enable(host.c_str(), port);
}

void MultiShip::OnConnected() {
    // Announce ourselves so the server has something to display immediately.
    nlohmann::json payload;
    payload["id"] = ShipUtils::Random(0, UINT32_MAX);
    payload["type"] = "hook";
    payload["hook"]["type"] = "OnConnected";
    SendJsonToRemote(payload);

    RegisterHooks();
}

void MultiShip::OnDisconnected() {
    RegisterHooks();
}

void MultiShip::OnIncomingJson(nlohmann::json payload) {
    // TODO: handle incoming messages from the MultiShip server.
    SPDLOG_INFO("[MultiShip] Received payload: \n{}", payload.dump());
}

void MultiShip::RegisterHooks() {
    // Every hook below is gated on `isConnected`, so they are only registered
    // (and only fire) while connected to a MultiShip server.

    // Loading a save file (entering gameplay from the file select).
    COND_HOOK(OnLoadGame, isConnected, [&](int32_t fileNum) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        payload["hook"]["type"] = "OnLoadGame";
        payload["hook"]["fileNum"] = fileNum;
        SendJsonToRemote(payload);
    });

    // Receiving an item.
    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        payload["hook"]["type"] = "OnItemReceive";
        payload["hook"]["tableId"] = itemEntry.tableId;
        payload["hook"]["getItemId"] = itemEntry.getItemId;
        SendJsonToRemote(payload);
    });

    // Defeating a boss. OnBossDefeat is already filtered to boss enemies only,
    // so no manual category filtering is needed here.
    COND_HOOK(OnBossDefeat, isConnected, [&](void* refActor) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        Actor* actor = (Actor*)refActor;
        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        payload["hook"]["type"] = "OnBossDefeat";
        payload["hook"]["actorId"] = actor->id;
        payload["hook"]["params"] = actor->params;
        SendJsonToRemote(payload);
    });

    // Dying and getting damaged both surface through the health-change hook.
    // This fires after the health value has been updated, so we can inspect the
    // resulting health to distinguish a death from non-lethal damage.
    COND_HOOK(OnPlayerHealthChange, isConnected, [&](int16_t amount) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        nlohmann::json payload;
        payload["id"] = ShipUtils::Random(0, UINT32_MAX);
        payload["type"] = "hook";
        if (gSaveContext.health <= 0) {
            payload["hook"]["type"] = "OnPlayerDeath";
        } else if (amount < 0) {
            payload["hook"]["type"] = "OnPlayerDamage";
            payload["hook"]["amount"] = amount;
        } else {
            return;
        }
        SendJsonToRemote(payload);
    });
}

#endif // ENABLE_MULTISHIP
