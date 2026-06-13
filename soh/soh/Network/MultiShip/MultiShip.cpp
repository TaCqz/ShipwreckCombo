#include "MultiShip.h"

#ifdef ENABLE_MULTISHIP

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>
#include "soh/ShipUtils.h"

extern "C" {
extern SaveContext gSaveContext;
}

void MultiShip::Connect() {
    // TODO: Establish the actual connection to the MultiShip server.
    //
    // Empty stub for now
    SPDLOG_INFO("[MultiShip] Connect() called (stub - not yet implemented)");
}

void MultiShip::OnConnected() {
    // TODO: send an initial "connected"/handshake payload to the server.
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

        // TODO: build and send the "save loaded" payload (fileNum).
    });

    // Receiving an item.
    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        // TODO: build and send the "item received" payload (itemEntry).
    });

    // Defeating a boss. OnBossDefeat is already filtered to boss enemies only,
    // so no manual category filtering is needed here.
    COND_HOOK(OnBossDefeat, isConnected, [&](void* refActor) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        Actor* actor = (Actor*)refActor;
        (void)actor;
        // TODO: build and send the "boss defeated" payload (actor->id, actor->params).
    });

    // Dying and getting damaged both surface through the health-change hook.
    // This fires after the health value has been updated, so we can inspect the
    // resulting health to distinguish a death from non-lethal damage.
    COND_HOOK(OnPlayerHealthChange, isConnected, [&](int16_t amount) {
        if (!isConnected || !GameInteractor::IsSaveLoaded())
            return;

        if (gSaveContext.health <= 0) {
            // TODO: build and send the "player died" payload.
        } else if (amount < 0) {
            // TODO: build and send the "player damaged" payload (amount).
        }
    });
}

#endif // ENABLE_MULTISHIP
