#include "MultiShip.h"

#ifdef ENABLE_MULTISHIP

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>
#include "soh/ShipUtils.h"
#include "soh/ShipInit.hpp"
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

void MultiShip::SendJsonToRemote(nlohmann::json packet) {
    // Attach the player's name to every packet (not just the handshake) so the
    // server can attribute any message — e.g. which player collected an item —
    // regardless of packet ordering or reconnects.
    packet["userName"] = CVarGetString(CVAR_REMOTE_MULTISHIP("UserName"), "");
    Network::SendJsonToRemote(packet);
}

void MultiShip::SendOnLoadGame() {
    // Reports the currently loaded MultiShip file to the server. Shared by the
    // OnLoadGame hook (fresh load while connected) and OnConnected (connecting
    // while a MultiShip file is already loaded).
    nlohmann::json payload;
    payload["id"] = ShipUtils::Random(0, UINT32_MAX);
    payload["type"] = "hook";
    payload["hook"]["type"] = "OnLoadGame";
    payload["hook"]["fileNum"] = gSaveContext.fileNum;
    // Quest/mode of the loaded file (Quest enum: 0 Normal, 1 Master, 2 Rando,
    // 3 Boss Rush, 4 MultiShip). Sourced from the loaded save, not the carousel.
    payload["hook"]["questId"] = gSaveContext.ship.quest.id;
    SPDLOG_INFO("[MultiShip] Sending OnLoadGame (fileNum {}, questId {})", gSaveContext.fileNum,
                gSaveContext.ship.quest.id);
    SendJsonToRemote(payload);
}

void MultiShip::OnConnected() {
    // Announce ourselves so the server has something to display immediately.
    // The user name is added to every packet by SendJsonToRemote().
    nlohmann::json payload;
    payload["id"] = ShipUtils::Random(0, UINT32_MAX);
    payload["type"] = "hook";
    payload["hook"]["type"] = "OnConnected";
    SendJsonToRemote(payload);

    // If we connect while a MultiShip file is already loaded (the common case:
    // the player opens the in-game menu and connects mid-game), report it right
    // away — the OnLoadGame hook only fires on a fresh load. The game hooks are
    // registered once at boot on the main thread (see the RegisterShipInitFunc
    // at the bottom of this file); they must NOT be registered from this network
    // thread, as GameInteractor's hook registry is only safe to mutate on the
    // main thread.
    if (GameInteractor::IsSaveLoaded() && gSaveContext.ship.quest.id == QUEST_MULTISHIP) {
        SendOnLoadGame();
    }
}

void MultiShip::OnIncomingJson(nlohmann::json payload) {
    // TODO: handle incoming messages from the MultiShip server.
    SPDLOG_INFO("[MultiShip] Received payload: \n{}", payload.dump());
}

void MultiShip::RegisterHooks() {
    // Registered ONCE at boot on the main thread (see the RegisterShipInitFunc
    // below). Each body is gated on `isConnected`, so the hooks only do anything
    // while connected to a MultiShip server. We intentionally register here
    // rather than from OnConnected: OnConnected runs on the network thread, and
    // GameInteractor's hook registry is only safe to mutate on the main thread —
    // registering from the network thread left the hooks silently never firing.

    // Loading a save file (entering gameplay from the file select). Only files
    // created in the MultiShip gamemode are reported — other quests (vanilla,
    // rando, boss rush, ...) are irrelevant to a MultiShip session.
    //
    // NOTE: OnLoadGame fires from the file-select gamestate, before Play_Init
    // runs, so gPlayState is still NULL and GameInteractor::IsSaveLoaded() would
    // return false here. We must NOT gate on it or the packet is never sent. The
    // selected file's quest is already populated in gSaveContext at this point.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnLoadGame>([&](int32_t fileNum) {
        if (!isConnected || gSaveContext.ship.quest.id != QUEST_MULTISHIP)
            return;

        SendOnLoadGame();
    });

    // Receiving an item.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnItemReceive>([&](GetItemEntry itemEntry) {
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
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnBossDefeat>([&](void* refActor) {
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
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerHealthChange>([&](int16_t amount) {
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

// Register the MultiShip game hooks once, at boot, on the main thread. By this
// point both GameInteractor::Instance and MultiShip::Instance have been created
// (see OTRGlobals init order). The hooks stay registered for the whole session
// and no-op while disconnected.
static RegisterShipInitFunc multiShipInitFunc([]() {
    if (MultiShip::Instance != nullptr) {
        MultiShip::Instance->RegisterHooks();
    }
});

#endif // ENABLE_MULTISHIP
