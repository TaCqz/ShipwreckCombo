#include "savefile.h"
#include "soh/OTRGlobals.h"
#include "soh/ResourceManagerHelpers.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/randomizer/logic.h"
#include "soh/Enhancements/randomizer/randomizer.h"
#ifdef ENABLE_MULTISHIP
#include "soh/Enhancements/randomizer/static_data.h" // Rando::StaticData (F-041 starting reward)
#endif

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"

uint8_t Randomizer_GetSettingValue(RandomizerSettingKey randoSettingKey);
GetItemEntry Randomizer_GetItemFromKnownCheck(RandomizerCheck randomizerCheck, GetItemID ogId);
}

// RANDOTODO: Replace most of these GiveLink functions with calls to
// Item_Give in z_parameter, we'll need to update Item_Give to ensure
// nothing breaks when calling it without a valid play first.
void GiveLinkRupees(int numOfRupees) {
    int maxRupeeCount = 0;
    if (CUR_UPG_VALUE(UPG_WALLET) == 0) {
        maxRupeeCount = 99;
    } else if (CUR_UPG_VALUE(UPG_WALLET) == 1) {
        maxRupeeCount = 200;
    } else if (CUR_UPG_VALUE(UPG_WALLET) == 2) {
        maxRupeeCount = 500;
    }

    int newRupeeCount = gSaveContext.rupees;
    newRupeeCount += numOfRupees;

    if (newRupeeCount > maxRupeeCount) {
        gSaveContext.rupees = maxRupeeCount;
    } else {
        gSaveContext.rupees = newRupeeCount;
    }
}

static uint16_t rupeeCounts[] = {
    1,   // ITEM_RUPEE_GREEN
    5,   // ITEM_RUPEE_BLUE
    20,  // ITEM_RUPEE_RED
    50,  // ITEM_RUPEE_PURPLE
    200, // ITEM_RUPEE_GOLD
};

void StartingItemGive(GetItemEntry getItemEntry, RandomizerCheck randomizerCheck) {
    if (randomizerCheck != RC_MAX) {
        OTRGlobals::Instance->gRandoContext->GetItemLocation(randomizerCheck)->SetCheckStatus(RCSHOW_SAVED);
    }
    if (getItemEntry.modIndex == MOD_NONE) {
        if (getItemEntry.itemId >= ITEM_RUPEE_GREEN && getItemEntry.itemId <= ITEM_RUPEE_GOLD) {
            GiveLinkRupees(rupeeCounts[getItemEntry.itemId - ITEM_RUPEE_GREEN]);
        } else {
            if (getItemEntry.getItemId == GI_SWORD_BGS) {
                gSaveContext.bgsFlag = true;
            }
            Item_Give(NULL, static_cast<uint8_t>(getItemEntry.itemId));
        }
    } else if (getItemEntry.modIndex == MOD_RANDOMIZER) {
        if (getItemEntry.getItemId == RG_ICE_TRAP) {
            gSaveContext.ship.pendingIceTrapCount++;
        } else {
            Randomizer_Item_Give(NULL, getItemEntry);
        }
    }
}

void GiveLinkDekuSticks(int howManySticks) {
    int maxStickCount = 0;
    if (CUR_UPG_VALUE(UPG_STICKS) == 0) {
        INV_CONTENT(ITEM_STICK) = ITEM_STICK;
        Inventory_ChangeUpgrade(UPG_STICKS, 1);
        maxStickCount = 10;
    } else if (CUR_UPG_VALUE(UPG_STICKS) == 1) {
        maxStickCount = 10;
    } else if (CUR_UPG_VALUE(UPG_STICKS) == 2) {
        maxStickCount = 20;
    } else if (CUR_UPG_VALUE(UPG_STICKS) == 3) {
        maxStickCount = 30;
    }

    if ((AMMO(ITEM_STICK) + howManySticks) > maxStickCount) {
        AMMO(ITEM_STICK) = maxStickCount;
    } else {
        AMMO(ITEM_STICK) += howManySticks;
    }
}

void GiveLinkDekuNuts(int howManyNuts) {
    int maxNutCount = 0;
    if (CUR_UPG_VALUE(UPG_NUTS) == 0) {
        INV_CONTENT(ITEM_NUT) = ITEM_NUT;
        Inventory_ChangeUpgrade(UPG_NUTS, 1);
        maxNutCount = 20;
    } else if (CUR_UPG_VALUE(UPG_NUTS) == 1) {
        maxNutCount = 20;
    } else if (CUR_UPG_VALUE(UPG_NUTS) == 2) {
        maxNutCount = 30;
    } else if (CUR_UPG_VALUE(UPG_NUTS) == 3) {
        maxNutCount = 40;
    }

    if ((AMMO(ITEM_NUT) + howManyNuts) > maxNutCount) {
        AMMO(ITEM_NUT) = maxNutCount;
    } else {
        AMMO(ITEM_NUT) += howManyNuts;
    }
}

void GiveLinksPocketItem() {
    if (Randomizer_GetSettingValue(RSK_LINKS_POCKET) != RO_LINKS_POCKET_NOTHING) {
        GetItemEntry getItemEntry = Randomizer_GetItemFromKnownCheck(RC_LINKS_POCKET, (GetItemID)RG_NONE);
        StartingItemGive(getItemEntry, RC_LINKS_POCKET);
        // If we re-add the above, we'll get the item on save creation, now it's given on first load.
        Flags_SetRandomizerInf(RAND_INF_LINKS_POCKET);
    }
}

#ifdef ENABLE_MULTISHIP
// F-041: grant the starting dungeon reward placed at Link's Pocket in a MultiShip save. Mirrors
// GiveLinksPocketItem (the proven rando path) but is driven by the MultiShip seed: the caller
// (MultiShip.cpp) passes the reward RandomizerGet read from the RC_LINKS_POCKET placement and owns
// the once-per-save guard. Dungeon rewards are vanilla (MOD_NONE) items, so StartingItemGive routes
// them through Item_Give, which sets the vanilla quest item directly — no rando game-behavior layer
// is needed (a MultiShip save has none). Called pre-spawn at save load with play == NULL, exactly
// like SetStartingItems / GiveLinksPocketItem.
extern "C" void Randomizer_MultiShipGiveStartingReward(int rgItem) {
    GetItemEntry entry = Rando::StaticData::RetrieveItem((RandomizerGet)rgItem).GetGIEntry_Copy();
    StartingItemGive(entry, RC_LINKS_POCKET);
}

// F-046: grant the item PLACED at `check` as a starting item, resolving it from the populated
// Context exactly like the native save-init grants (GiveLinksPocketItem, the RC_TOT_MASTER_SWORD
// block in Randomizer_InitSaveFile). Used by MultiShip's owner-aware start auto-collect for our OWN
// adult-start Master Sword pedestal + skipped Song From Impa items — checks with no in-world trigger
// left to deliver them. The caller only invokes this for a check placed in our world, so
// Randomizer_GetItemFromKnownCheck resolves the real placed item (never the unplaced-check fallback).
// play == NULL safe (StartingItemGive routes through Item_Give), like the other start grants.
extern "C" void Randomizer_MultiShipGiveStartCheckItem(int check) {
    GetItemEntry entry = Randomizer_GetItemFromKnownCheck((RandomizerCheck)check, (GetItemID)RG_NONE);
    StartingItemGive(entry, (RandomizerCheck)check);
}
#endif

void SetStartingItems() {
    int startingAge = OTRGlobals::Instance->gRandoContext->GetOption(RSK_SELECTED_STARTING_AGE).Get();
    if (Randomizer_GetSettingValue(RSK_STARTING_KOKIRI_SWORD))
        Item_Give(NULL, ITEM_SWORD_KOKIRI);
    if (Randomizer_GetSettingValue(RSK_STARTING_DEKU_SHIELD))
        Item_Give(NULL, ITEM_SHIELD_DEKU);

    // Songs
    if (Randomizer_GetSettingValue(RSK_STARTING_ZELDAS_LULLABY))
        Item_Give(NULL, ITEM_SONG_LULLABY);
    if (Randomizer_GetSettingValue(RSK_STARTING_EPONAS_SONG))
        Item_Give(NULL, ITEM_SONG_EPONA);
    if (Randomizer_GetSettingValue(RSK_STARTING_SARIAS_SONG))
        Item_Give(NULL, ITEM_SONG_SARIA);
    if (Randomizer_GetSettingValue(RSK_STARTING_SUNS_SONG))
        Item_Give(NULL, ITEM_SONG_SUN);
    if (Randomizer_GetSettingValue(RSK_STARTING_SONG_OF_TIME))
        Item_Give(NULL, ITEM_SONG_TIME);
    if (Randomizer_GetSettingValue(RSK_STARTING_SONG_OF_STORMS))
        Item_Give(NULL, ITEM_SONG_STORMS);
    if (Randomizer_GetSettingValue(RSK_STARTING_MINUET_OF_FOREST))
        Item_Give(NULL, ITEM_SONG_MINUET);
    if (Randomizer_GetSettingValue(RSK_STARTING_BOLERO_OF_FIRE))
        Item_Give(NULL, ITEM_SONG_BOLERO);
    if (Randomizer_GetSettingValue(RSK_STARTING_SERENADE_OF_WATER))
        Item_Give(NULL, ITEM_SONG_SERENADE);
    if (Randomizer_GetSettingValue(RSK_STARTING_REQUIEM_OF_SPIRIT))
        Item_Give(NULL, ITEM_SONG_REQUIEM);
    if (Randomizer_GetSettingValue(RSK_STARTING_NOCTURNE_OF_SHADOW))
        Item_Give(NULL, ITEM_SONG_NOCTURNE);
    if (Randomizer_GetSettingValue(RSK_STARTING_PRELUDE_OF_LIGHT))
        Item_Give(NULL, ITEM_SONG_PRELUDE);

    if (Randomizer_GetSettingValue(RSK_STARTING_SKULLTULA_TOKEN)) {
        gSaveContext.inventory.questItems |= gBitFlags[QUEST_SKULL_TOKEN];
        gSaveContext.inventory.gsTokens = Randomizer_GetSettingValue(RSK_STARTING_SKULLTULA_TOKEN);
    }

    if ((Randomizer_GetSettingValue(RSK_STARTING_HEARTS) + 1) != 3) {
        gSaveContext.healthCapacity = (Randomizer_GetSettingValue(RSK_STARTING_HEARTS) + 1) * 16;
        gSaveContext.health = gSaveContext.healthCapacity;
    }

    if (Randomizer_GetSettingValue(RSK_STARTING_OCARINA)) {
        INV_CONTENT(ITEM_OCARINA_FAIRY) = Randomizer_GetSettingValue(RSK_STARTING_OCARINA) == RO_STARTING_OCARINA_FAIRY
                                              ? ITEM_OCARINA_FAIRY
                                              : ITEM_OCARINA_TIME;
    }

    if (Randomizer_GetSettingValue(RSK_STARTING_STICKS) && !Randomizer_GetSettingValue(RSK_SHUFFLE_DEKU_STICK_BAG)) {
        GiveLinkDekuSticks(10);
    }
    if (Randomizer_GetSettingValue(RSK_STARTING_NUTS) && !Randomizer_GetSettingValue(RSK_SHUFFLE_DEKU_NUT_BAG)) {
        GiveLinkDekuNuts(20);
    }
    if (Randomizer_GetSettingValue(RSK_STARTING_MASTER_SWORD)) {
        if (startingAge == RO_AGE_ADULT) {
            Item_Give(NULL, ITEM_SWORD_MASTER);
        } else {
            gSaveContext.inventory.equipment |= 1 << 1;
        }
    }

    if (Randomizer_GetSettingValue(RSK_FULL_WALLETS)) {
        GiveLinkRupees(9001);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_MAPANDCOMPASS) == RO_DUNGEON_ITEM_LOC_STARTWITH) {
        uint32_t mapBitMask = 1 << 1;
        uint32_t compassBitMask = 1 << 2;
        uint32_t startingDungeonItemsBitMask = mapBitMask | compassBitMask;
        for (int scene = SCENE_DEKU_TREE; scene <= SCENE_ICE_CAVERN; scene++) {
            gSaveContext.inventory.dungeonItems[scene] |= startingDungeonItemsBitMask;
        }
    }

    if (Randomizer_GetSettingValue(RSK_KEYSANITY) == RO_DUNGEON_ITEM_LOC_STARTWITH) {
        gSaveContext.inventory.dungeonKeys[SCENE_FOREST_TEMPLE] = FOREST_TEMPLE_SMALL_KEY_MAX;            // Forest
        gSaveContext.ship.stats.dungeonKeys[SCENE_FOREST_TEMPLE] = FOREST_TEMPLE_SMALL_KEY_MAX;           // Forest
        gSaveContext.inventory.dungeonKeys[SCENE_FIRE_TEMPLE] = FIRE_TEMPLE_SMALL_KEY_MAX;                // Fire
        gSaveContext.ship.stats.dungeonKeys[SCENE_FIRE_TEMPLE] = FIRE_TEMPLE_SMALL_KEY_MAX;               // Fire
        gSaveContext.inventory.dungeonKeys[SCENE_WATER_TEMPLE] = WATER_TEMPLE_SMALL_KEY_MAX;              // Water
        gSaveContext.ship.stats.dungeonKeys[SCENE_WATER_TEMPLE] = WATER_TEMPLE_SMALL_KEY_MAX;             // Water
        gSaveContext.inventory.dungeonKeys[SCENE_SPIRIT_TEMPLE] = SPIRIT_TEMPLE_SMALL_KEY_MAX;            // Spirit
        gSaveContext.ship.stats.dungeonKeys[SCENE_SPIRIT_TEMPLE] = SPIRIT_TEMPLE_SMALL_KEY_MAX;           // Spirit
        gSaveContext.inventory.dungeonKeys[SCENE_SHADOW_TEMPLE] = SHADOW_TEMPLE_SMALL_KEY_MAX;            // Shadow
        gSaveContext.ship.stats.dungeonKeys[SCENE_SHADOW_TEMPLE] = SHADOW_TEMPLE_SMALL_KEY_MAX;           // Shadow
        gSaveContext.inventory.dungeonKeys[SCENE_BOTTOM_OF_THE_WELL] = BOTTOM_OF_THE_WELL_SMALL_KEY_MAX;  // BotW
        gSaveContext.ship.stats.dungeonKeys[SCENE_BOTTOM_OF_THE_WELL] = BOTTOM_OF_THE_WELL_SMALL_KEY_MAX; // BotW
        gSaveContext.inventory.dungeonKeys[SCENE_GERUDO_TRAINING_GROUND] = GERUDO_TRAINING_GROUND_SMALL_KEY_MAX;  // GTG
        gSaveContext.ship.stats.dungeonKeys[SCENE_GERUDO_TRAINING_GROUND] = GERUDO_TRAINING_GROUND_SMALL_KEY_MAX; // GTG
        gSaveContext.inventory.dungeonKeys[SCENE_INSIDE_GANONS_CASTLE] = GANONS_CASTLE_SMALL_KEY_MAX;  // Ganon
        gSaveContext.ship.stats.dungeonKeys[SCENE_INSIDE_GANONS_CASTLE] = GANONS_CASTLE_SMALL_KEY_MAX; // Ganon
    } else if (Randomizer_GetSettingValue(RSK_KEYSANITY) == RO_DUNGEON_ITEM_LOC_VANILLA) {
        // Logic cannot handle vanilla key layout in some dungeons
        // this is because vanilla expects the dungeon major item to be
        // locked behind the keys, which is not always true in rando.
        // We can resolve this by starting with some extra keys.
        if (ResourceMgr_IsSceneMasterQuest(SCENE_SPIRIT_TEMPLE)) {
            // MQ Spirit needs 3 keys.
            gSaveContext.inventory.dungeonKeys[SCENE_SPIRIT_TEMPLE] = 3;
            gSaveContext.ship.stats.dungeonKeys[SCENE_SPIRIT_TEMPLE] = 3;
        }
    }

    if (Randomizer_GetSettingValue(RSK_BOSS_KEYSANITY) == RO_DUNGEON_ITEM_LOC_STARTWITH) {
        gSaveContext.inventory.dungeonItems[SCENE_FOREST_TEMPLE] |= 1; // Forest
        gSaveContext.inventory.dungeonItems[SCENE_FIRE_TEMPLE] |= 1;   // Fire
        gSaveContext.inventory.dungeonItems[SCENE_WATER_TEMPLE] |= 1;  // Water
        gSaveContext.inventory.dungeonItems[SCENE_SPIRIT_TEMPLE] |= 1; // Spirit
        gSaveContext.inventory.dungeonItems[SCENE_SHADOW_TEMPLE] |= 1; // Shadow
    }

    if (Randomizer_GetSettingValue(RSK_GANONS_BOSS_KEY) == RO_GANON_BOSS_KEY_STARTWITH) {
        gSaveContext.inventory.dungeonItems[SCENE_GANONS_TOWER] |= 1;
    }
}

// Bakes the one-time world-state flags for the open-world / "area access" settings.
// Split out of Randomizer_InitSaveFile so it can be re-run after the fact: in MultiShip
// the authoritative server settings can arrive AFTER the file was created (the
// connect-before-create race), and the live-context settings reapply alone doesn't fix
// effects that are baked as event/scene flags at creation — the Mido/Kokiri-boy block
// on the forest exit and the freed Gerudo carpenters. Re-running this once the synced
// settings are known re-derives those flags from the live Context.
//
// Every write here is idempotent and "open"-only (it sets progress/scene-switch bits,
// or unsets the now-unneeded Zelda's-letter trade step), so calling it repeatedly — and
// after the player is already in-game — never removes earned progress. The one non-flag
// effect (handing out the Gerudo card under Carpenters Free) is guarded so it can't be
// granted twice.
//
// Note: Zora's Fountain, Sleeping Waterfall and Jabu-Jabu have no init-baked flags —
// their open state is read live from the Context by their actors / VB hooks, so the
// settings reapply already corrects them and they need no re-bake here.
extern "C" void Randomizer_ApplyAreaAccessWorldState() {
    // Kokiri Forest open (or deku-only): pre-satisfy Mido so neither he nor the Kokiri
    // boy block the forest exit before the Deku Tree is beaten.
    if (Randomizer_GetSettingValue(RSK_FOREST) == RO_CLOSED_FOREST_OFF) {
        Flags_SetEventChkInf(EVENTCHKINF_SHOWED_MIDO_SWORD_SHIELD);
        Flags_SetEventChkInf(EVENTCHKINF_SPOKE_TO_MIDO_AFTER_DEKU_TREES_DEATH);
    }

    if (Randomizer_GetSettingValue(RSK_DOOR_OF_TIME) == RO_DOOROFTIME_OPEN) {
        Flags_SetEventChkInf(EVENTCHKINF_OPENED_THE_DOOR_OF_TIME);
    }

    if (Randomizer_GetSettingValue(RSK_KAK_GATE) == RO_KAK_GATE_OPEN) {
        Flags_SetInfTable(INFTABLE_SHOWED_ZELDAS_LETTER_TO_GATE_GUARD);
        Flags_UnsetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_LETTER_ZELDA);
    }

    if (Randomizer_GetSettingValue(RSK_GERUDO_FORTRESS) == RO_GF_CARPENTERS_FAST ||
        Randomizer_GetSettingValue(RSK_GERUDO_FORTRESS) == RO_GF_CARPENTERS_FREE) {
        Flags_SetEventChkInf(EVENTCHKINF_CARPENTERS_FREE(1));
        Flags_SetEventChkInf(EVENTCHKINF_CARPENTERS_FREE(2));
        Flags_SetEventChkInf(EVENTCHKINF_CARPENTERS_FREE(3));
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x02); // heard yells and unlocked doors
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x03);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x04);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x06);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x07);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x08);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x10);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x12);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x13);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].collect |= (1 << 0x0A); // picked up keys
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].collect |= (1 << 0x0E);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].collect |= (1 << 0x0F);
    }

    if (Randomizer_GetSettingValue(RSK_GERUDO_FORTRESS) == RO_GF_CARPENTERS_FREE) {
        Flags_SetEventChkInf(EVENTCHKINF_CARPENTERS_FREE(0));
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x01); // heard yell and unlocked door
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x05);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].swch |= (1 << 0x11);
        gSaveContext.sceneFlags[SCENE_THIEVES_HIDEOUT].collect |= (1 << 0x0C); // picked up key

        Flags_SetRandomizerInf(RAND_INF_TH_ITEM_FROM_LEADER_OF_FORTRESS);
        // Guard the grant so a re-run (e.g. MultiShip late-arriving settings) can't hand
        // out a second Gerudo card.
        if (!Randomizer_GetSettingValue(RSK_SHUFFLE_GERUDO_MEMBERSHIP_CARD) &&
            !CHECK_QUEST_ITEM(QUEST_GERUDO_CARD)) {
            Item_Give(NULL, ITEM_GERUDO_CARD);
        }
    }
}

#ifdef ENABLE_MULTISHIP
// F-043 (b): configure Ganon's Trials for a MultiShip save from the synced trial count. The
// generator resolved Ganon's Trials to a concrete count (shipped as RSK_TRIAL_COUNT); mark that
// many trials required and the rest skipped, then bake the COMPLETED flag for every skipped trial
// so its Ganon's-Castle barrier is already dispelled (mirrors the loop in Randomizer_InitSaveFile).
// The required subset is the FIRST `count` trials in canonical TrialKey order — IDENTICAL to the
// generator's selection (MultiShip/Fill.cpp), so the seed's logic and this world agree on exactly
// which trials are required (different trials need different items; the generator only guarantees
// the required ones are clearable, so the subset must match). The caller applies the synced
// settings to the Context first (so RSK_TRIAL_COUNT reads correctly). Idempotent. Standard rando is
// untouched (it sets trials during generation).
extern "C" void Randomizer_MultiShipApplyTrials() {
    auto ctx = Rando::Context::GetInstance();
    if (ctx == nullptr) {
        return;
    }
    int count = Randomizer_GetSettingValue(RSK_TRIAL_COUNT);
    if (count < 0) {
        count = 0;
    }
    if (count > TK_MAX) {
        count = TK_MAX;
    }
    static const TrialKey order[TK_MAX] = { TK_LIGHT_TRIAL,  TK_FOREST_TRIAL, TK_FIRE_TRIAL,
                                            TK_WATER_TRIAL,  TK_SPIRIT_TRIAL, TK_SHADOW_TRIAL };
    for (int i = 0; i < TK_MAX; ++i) {
        ctx->GetTrial(order[i])->SetAsSkipped();
    }
    for (int i = 0; i < count; ++i) {
        ctx->GetTrial(order[i])->SetAsRequired();
    }
    for (auto trialFlag : { EVENTCHKINF_COMPLETED_LIGHT_TRIAL, EVENTCHKINF_COMPLETED_FOREST_TRIAL,
                            EVENTCHKINF_COMPLETED_FIRE_TRIAL, EVENTCHKINF_COMPLETED_WATER_TRIAL,
                            EVENTCHKINF_COMPLETED_SPIRIT_TRIAL, EVENTCHKINF_COMPLETED_SHADOW_TRIAL }) {
        if (!OTRGlobals::Instance->gRandomizer->IsTrialRequired(trialFlag)) {
            Flags_SetEventChkInf(trialFlag);
        }
    }
}

// F-044: apply the one-time starting STATE for a MultiShip save from the synced settings. This is
// the analog of the per-setting blocks in Randomizer_InitSaveFile — which does NOT run for a
// QUEST_MULTISHIP file — for Tab 1 section 1.1 "Logic": the adult-start Master Sword, the
// full-wallet rupees, the Skip Child Zelda letter + sequence-skip flags, and the completed mask
// quest. Pooled checks that Skip Child Zelda auto-collects (Song From Impa) are NOT granted here:
// in a multiworld they can hold a foreign item, so MultiShip.cpp routes them owner-aware through
// the F-040 flow. The caller (MultiShip_ApplyStartState) copies the synced settings into the
// Context first — so the Randomizer_GetSettingValue reads below see the right values at both file
// creation and load — and guards this to run exactly once via the persisted start-state marker.
// The flag writes are idempotent; the item grants (Master Sword, rupees, letter) are the part the
// marker must gate so a reload never duplicates them. Standard rando + vanilla are untouched.
extern "C" void Randomizer_MultiShipApplyStartState() {
    int startingAge = Randomizer_GetSettingValue(RSK_SELECTED_STARTING_AGE);

    // Starting age + Master Sword. Sram_InitSave built a child file; for an adult start switch to
    // adult and spawn at the Temple of Time (mirrors Randomizer_InitSaveFile:487-492 + the adult
    // equip in z_sram.c). For this batch Master Sword shuffle is off (it's a Tab 3 / F-046 setting,
    // not shipped or copied here), so an adult start ALWAYS gets the Master Sword. When F-046 lands
    // it adds the shuffle key to the honored/copied set and, when on, grants the generator's free
    // item at RC_TOT_MASTER_SWORD instead — slot that branch into the `else` below.
    if (startingAge == RO_AGE_ADULT) {
        gSaveContext.savedSceneNum = -1;
        gSaveContext.linkAge = LINK_AGE_ADULT;
        gSaveContext.entranceIndex = ENTR_TEMPLE_OF_TIME_WARP_PAD;
        gSaveContext.cutsceneIndex = 0;
        // F-046: with Master Sword shuffle ON the pedestal is a real check, so an adult start does NOT
        // get the Master Sword for free — the generator placed some item at RC_TOT_MASTER_SWORD (own
        // or foreign), and MultiShip_ApplyStartState's owner-aware auto-collect grants/routes it (the
        // analog of Randomizer_InitSaveFile, which likewise skips the equip and grants the pedestal
        // item when the sword is shuffled). Only auto-equip the Master Sword when it is NOT shuffled.
        if (!Randomizer_GetSettingValue(RSK_SHUFFLE_MASTER_SWORD) &&
            !CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_MASTER)) {
            gSaveContext.inventory.equipment |= OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_MASTER);
            gSaveContext.equips.buttonItems[0] = ITEM_SWORD_MASTER;
            gSaveContext.equips.equipment &= ~(0xF << (EQUIP_TYPE_SWORD * 4));
            gSaveContext.equips.equipment |= EQUIP_VALUE_SWORD_MASTER << (EQUIP_TYPE_SWORD * 4);
        }
    }

    // The basic (child) wallet is owned from the start. MultiShip never shuffles it (the child-wallet
    // shuffle isn't offered), so set RAND_INF_HAS_WALLET unconditionally — the rando init gates this on
    // SHUFFLE_CHILD_WALLET being off, which is always the case here. Without it a collected Progressive
    // Wallet resolves to the CHILD wallet (which you already effectively have, holding 99 rupees) instead
    // of advancing Adult -> Giant -> Tycoon.
    Flags_SetRandomizerInf(RAND_INF_HAS_WALLET);

    // Full Wallets: fill the wallet (GiveLinkRupees caps to the current wallet max).
    if (Randomizer_GetSettingValue(RSK_FULL_WALLETS)) {
        GiveLinkRupees(9001);
    }

    // Skip Child Zelda: start with Zelda's Lullaby + the Letter + the sequence-skip flags
    // (Malon/Talon back at the ranch, letter obtained, lullaby learned, milk crates moved). Mirrors
    // Randomizer_InitSaveFile:514-540 MINUS the Weird Egg (excluded — Malon no longer waits).
    //
    // Grant the lullaby DIRECTLY with Item_Give. Song shuffle is NOT honored (Tab 3), so the engine
    // places nothing at Song-From-Impa — it is the vanilla, own-world lullaby. (Do NOT resolve it via
    // Randomizer_GetItemFromKnownCheck: on an unplaced check that falls back to the (GetItemID) cast
    // of RG_ZELDAS_LULLABY — a different enum — and hands out a garbage item, e.g. Bolero of Fire.)
    // The lullaby is REQUIRED beyond being the "extra item": it sets QUEST_SONG_LULLABY, without
    // which the vanilla !IS_RANDO fixup in Sram_OpenSave reverts the child-trade slot to a Chicken
    // (that fixup is also now gated off for MultiShip). When song shuffle is honored, Song-From-Impa
    // becomes a placed (possibly foreign) check and must be delivered owner-aware via F-040 instead.
    if (Randomizer_GetSettingValue(RSK_SKIP_CHILD_ZELDA)) {
        // F-046: only grant the vanilla lullaby directly when songs are NOT shuffled. With song
        // shuffle on, Song From Impa holds a placed (possibly foreign) item, delivered/routed by
        // MultiShip_ApplyStartState's owner-aware start auto-collect of RC_SONG_FROM_IMPA — granting
        // the lullaby here too would be the wrong item and would leave the placed one uncollected.
        // (The EVENTCHKINF_LEARNED_ZELDAS_LULLABY sequence flag is still set below; it fires no
        // collection here because the F-040 flag handler isn't registered until OnLoadGame.)
        if (Randomizer_GetSettingValue(RSK_SHUFFLE_SONGS) == RO_SONG_SHUFFLE_OFF) {
            Item_Give(NULL, ITEM_SONG_LULLABY);
        }

        Flags_SetEventChkInf(EVENTCHKINF_OBTAINED_POCKET_EGG);
        Flags_SetRandomizerInf(RAND_INF_WEIRD_EGG);
        Flags_SetEventChkInf(EVENTCHKINF_TALON_WOKEN_IN_CASTLE);
        Flags_SetEventChkInf(EVENTCHKINF_TALON_RETURNED_FROM_CASTLE);
        Flags_SetEventChkInf(EVENTCHKINF_OBTAINED_ZELDAS_LETTER);
        Flags_SetRandomizerInf(RAND_INF_ZELDAS_LETTER);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_LETTER_ZELDA);
        Flags_SetEventChkInf(EVENTCHKINF_LEARNED_ZELDAS_LULLABY);
        gSaveContext.sceneFlags[SCENE_HYRULE_CASTLE].swch |= (1 << 0x4); // move milk crates to the moat
        INV_CONTENT(ITEM_LETTER_ZELDA) = ITEM_LETTER_ZELDA;             // always start with the letter
    }

    // Mask Quest = Completed: grant ALL masks up front (no shop borrowing). Sets the same
    // completed-quest flags as Randomizer_InitSaveFile:573-590, then drops a mask into the
    // child-trade slot so the inventory mask-cycle (gated by CanMaskSelect, which is now
    // IS_MULTISHIP-aware in z_kaleido_item.c) lets the player rotate through all eight. The vanilla
    // cycle path that MultiShip uses walks ITEM_MASK_KEATON..ITEM_MASK_TRUTH directly, so no further
    // per-mask grant is needed.
    if (Randomizer_GetSettingValue(RSK_MASK_QUEST) == RO_MASK_QUEST_COMPLETED) {
        Flags_SetInfTable(INFTABLE_GATE_GUARD_PUT_ON_KEATON_MASK);
        Flags_SetEventChkInf(EVENTCHKINF_PAID_BACK_BUNNY_HOOD_FEE);

        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_KEATON);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_SKULL);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_SPOOKY);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_BUNNY);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_GORON);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_ZORA);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_GERUDO);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_TRUTH);

        gSaveContext.itemGetInf[3] |= 0x100;  // Sold Keaton Mask
        gSaveContext.itemGetInf[3] |= 0x200;  // Sold Skull Mask
        gSaveContext.itemGetInf[3] |= 0x400;  // Sold Spooky Mask
        gSaveContext.itemGetInf[3] |= 0x800;  // Bunny Hood
        gSaveContext.itemGetInf[3] |= 0x8000; // Obtained Mask of Truth

        // Put a mask in the child-trade slot so one is shown + the cycle has a starting point. Only
        // if the slot is free — Skip Child Zelda may have placed the Letter there first (the player
        // cycles letter -> masks as in vanilla).
        if (INV_CONTENT(ITEM_TRADE_CHILD) == ITEM_NONE) {
            INV_CONTENT(ITEM_TRADE_CHILD) = ITEM_MASK_KEATON;
        }
    }

    // Skip Epona Race: mark Epona obtained so playing Epona's Song summons her without racing Ingo.
    // EVENTCHKINF_EPONA_OBTAINED is the ownership flag En_Horse checks for the summon (winning the
    // race normally sets it). RENTED_HORSE_FROM_INGO alone — what rando sets for every save — is
    // only the rental step and does NOT enable the summon, which is why it appeared unfixed. Both
    // are set; with Epona owned, Ingo's race horse is also removed at Lon Lon.
    if (Randomizer_GetSettingValue(RSK_SKIP_EPONA_RACE)) {
        Flags_SetEventChkInf(EVENTCHKINF_RENTED_HORSE_FROM_INGO);
        Flags_SetEventChkInf(EVENTCHKINF_EPONA_OBTAINED);
    }

    // F-045 — Tab 2 dungeon items, "Start With" modes. Mirrors the Randomizer_InitSaveFile
    // start-with blocks (which don't run for QUEST_MULTISHIP) for Maps & Compasses, Small Keys,
    // Boss Keys and Ganon's Boss Key. Reads the dungeon-item settings copied into the Context by
    // MultiShip_CopyHonoredSettingsToContext (the keys default to 0 == Start-With in an unpopulated
    // Context, so the copy is REQUIRED — without it every save would wrongly grant). The other
    // dungeon-item modes (Vanilla / Own / Any / Overworld / Anywhere) place the items in the world
    // and are delivered via the F-040 flow + Randomizer_Item_Give, so they need no grant here.
    // Gerudo Fortress keys and Key Rings have no Start-With mode, so they are intentionally absent.
    if (Randomizer_GetSettingValue(RSK_SHUFFLE_MAPANDCOMPASS) == RO_DUNGEON_ITEM_LOC_STARTWITH) {
        uint32_t startingDungeonItemsBitMask = (1 << 1) | (1 << 2);  // map | compass
        for (int scene = SCENE_DEKU_TREE; scene <= SCENE_ICE_CAVERN; scene++) {
            gSaveContext.inventory.dungeonItems[scene] |= startingDungeonItemsBitMask;
        }
    }
    if (Randomizer_GetSettingValue(RSK_KEYSANITY) == RO_DUNGEON_ITEM_LOC_STARTWITH) {
        gSaveContext.inventory.dungeonKeys[SCENE_FOREST_TEMPLE]            = FOREST_TEMPLE_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_FIRE_TEMPLE]              = FIRE_TEMPLE_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_WATER_TEMPLE]             = WATER_TEMPLE_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_SPIRIT_TEMPLE]            = SPIRIT_TEMPLE_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_SHADOW_TEMPLE]            = SHADOW_TEMPLE_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_BOTTOM_OF_THE_WELL]       = BOTTOM_OF_THE_WELL_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_GERUDO_TRAINING_GROUND]   = GERUDO_TRAINING_GROUND_SMALL_KEY_MAX;
        gSaveContext.inventory.dungeonKeys[SCENE_INSIDE_GANONS_CASTLE]     = GANONS_CASTLE_SMALL_KEY_MAX;
    }
    if (Randomizer_GetSettingValue(RSK_BOSS_KEYSANITY) == RO_DUNGEON_ITEM_LOC_STARTWITH) {
        gSaveContext.inventory.dungeonItems[SCENE_FOREST_TEMPLE] |= 1;  // Forest
        gSaveContext.inventory.dungeonItems[SCENE_FIRE_TEMPLE]   |= 1;  // Fire
        gSaveContext.inventory.dungeonItems[SCENE_WATER_TEMPLE]  |= 1;  // Water
        gSaveContext.inventory.dungeonItems[SCENE_SPIRIT_TEMPLE] |= 1;  // Spirit
        gSaveContext.inventory.dungeonItems[SCENE_SHADOW_TEMPLE] |= 1;  // Shadow
    }
    if (Randomizer_GetSettingValue(RSK_GANONS_BOSS_KEY) == RO_GANON_BOSS_KEY_STARTWITH) {
        gSaveContext.inventory.dungeonItems[SCENE_GANONS_TOWER] |= 1;
    }
}
#endif

extern "C" void Randomizer_InitSaveFile() {
    auto ctx = Rando::Context::GetInstance();
    ctx->GetLogic()->SetSaveContext(&gSaveContext);

    // Starts pending ice traps out at 0 before potentially incrementing them down the line.
    gSaveContext.ship.pendingIceTrapCount = 0;

    // Reset triforce pieces collected.
    gSaveContext.ship.quest.data.randomizer.triforcePiecesCollected = 0;

    // Reset Bombchu Bag Upgrade
    gSaveContext.ship.quest.data.randomizer.bombchuUpgradeLevel = 0;

    SetStartingItems();

    // Set Cutscene flags and texts to skip them.
    Flags_SetEventChkInf(EVENTCHKINF_FIRST_SPOKE_TO_MIDO);
    Flags_SetInfTable(INFTABLE_SPOKE_TO_KAEPORA_IN_LAKE_HYLIA);
    Flags_SetEventChkInf(EVENTCHKINF_SHEIK_SPAWNED_AT_MASTER_SWORD_PEDESTAL);
    Flags_SetEventChkInf(EVENTCHKINF_RENTED_HORSE_FROM_INGO);
    Flags_SetInfTable(INFTABLE_SPOKE_TO_POE_COLLECTOR_IN_RUINED_MARKET);
    Flags_SetEventChkInf(EVENTCHKINF_WATCHED_GANONS_CASTLE_COLLAPSE_CAUGHT_BY_GERUDO);

    // Go away Ruto (Water Temple first cutscene).
    gSaveContext.sceneFlags[SCENE_WATER_TEMPLE].swch |= (1 << 0x10);

    if (Randomizer_GetSettingValue(RSK_STARTING_BEANS)) {
        INV_CONTENT(ITEM_BEAN) = ITEM_BEAN;
        if (Randomizer_GetSettingValue(RSK_SHUFFLE_MERCHANTS) != RO_SHUFFLE_MERCHANTS_BEANS_ONLY &&
            Randomizer_GetSettingValue(RSK_SHUFFLE_MERCHANTS) != RO_SHUFFLE_MERCHANTS_ALL) {
            BEANS_BOUGHT = 10;
        }
        if (Randomizer_GetSettingValue(RSK_SKIP_PLANTING_BEANS)) {
            AMMO(ITEM_BEAN) = 0;
            gSaveContext.sceneFlags[SCENE_DEATH_MOUNTAIN_CRATER].swch |= (1 << 3);
            gSaveContext.sceneFlags[SCENE_DEATH_MOUNTAIN_TRAIL].swch |= (1 << 6);
            gSaveContext.sceneFlags[SCENE_DESERT_COLOSSUS].swch |= (1 << 24);
            gSaveContext.sceneFlags[SCENE_GERUDO_VALLEY].swch |= (1 << 3);
            gSaveContext.sceneFlags[SCENE_GRAVEYARD].swch |= (1 << 3);
            gSaveContext.sceneFlags[SCENE_KOKIRI_FOREST].swch |= (1 << 9);
            gSaveContext.sceneFlags[SCENE_LAKE_HYLIA].swch |= (1 << 1);
            gSaveContext.sceneFlags[SCENE_LOST_WOODS].swch |= (1 << 4) | (1 << 18);
            gSaveContext.sceneFlags[SCENE_ZORAS_RIVER].swch |= (1 << 3);
        } else {
            AMMO(ITEM_BEAN) = 10;
        }
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_BEAN_SOULS) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_DEATH_MOUNTAIN_CRATER_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_DEATH_MOUNTAIN_TRAIL_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_DESERT_COLOSSUS_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_GERUDO_VALLEY_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_GRAVEYARD_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_KOKIRI_FOREST_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_LAKE_HYLIA_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_LOST_WOODS_BRIDGE_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_LOST_WOODS_BEAN_SOUL);
        Flags_SetRandomizerInf(RAND_INF_ZORAS_RIVER_BEAN_SOUL);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_OCARINA_BUTTONS) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_HAS_OCARINA_A);
        Flags_SetRandomizerInf(RAND_INF_HAS_OCARINA_C_LEFT);
        Flags_SetRandomizerInf(RAND_INF_HAS_OCARINA_C_RIGHT);
        Flags_SetRandomizerInf(RAND_INF_HAS_OCARINA_C_UP);
        Flags_SetRandomizerInf(RAND_INF_HAS_OCARINA_C_DOWN);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_SWIM) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_CAN_SWIM);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_GRAB) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_CAN_GRAB);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_CLIMB) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_CAN_CLIMB);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_CRAWL) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_CAN_CRAWL);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_SPEAK) == RO_GENERIC_OFF) {
        Flags_SetEventChkInf(EVENTCHKINF_SPOKE_TO_NABOORU_IN_SPIRIT_TEMPLE);
        Flags_SetRandomizerInf(RAND_INF_CAN_SPEAK_DEKU);
        Flags_SetRandomizerInf(RAND_INF_CAN_SPEAK_GERUDO);
        Flags_SetRandomizerInf(RAND_INF_CAN_SPEAK_GORON);
        Flags_SetRandomizerInf(RAND_INF_CAN_SPEAK_HYLIAN);
        Flags_SetRandomizerInf(RAND_INF_CAN_SPEAK_KOKIRI);
        Flags_SetRandomizerInf(RAND_INF_CAN_SPEAK_ZORA);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_OPEN_CHEST) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_CAN_OPEN_CHEST);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_CHILD_WALLET) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_HAS_WALLET);
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_FISHING_POLE) == RO_GENERIC_OFF) {
        Flags_SetRandomizerInf(RAND_INF_FISHING_POLE_FOUND);
    }

    // Give Link's pocket item
    GiveLinksPocketItem();

    // Remove One Time Scrubs with Scrubsanity off
    if (Randomizer_GetSettingValue(RSK_SHUFFLE_SCRUBS) == RO_SCRUBS_OFF) {
        Flags_SetItemGetInf(ITEMGETINF_DEKU_SCRUB_HEART_PIECE);
        Flags_SetInfTable(INFTABLE_BOUGHT_STICK_UPGRADE);
        Flags_SetInfTable(INFTABLE_BOUGHT_NUT_UPGRADE);
    }

    int startingAge = OTRGlobals::Instance->gRandoContext->GetOption(RSK_SELECTED_STARTING_AGE).Get();
    gSaveContext.savedSceneNum = -1;
    switch (startingAge) {
        case RO_AGE_ADULT: // Adult
            gSaveContext.linkAge = LINK_AGE_ADULT;
            gSaveContext.entranceIndex = ENTR_TEMPLE_OF_TIME_WARP_PAD;
            gSaveContext.cutsceneIndex = 0;
            break;
        case RO_AGE_CHILD: // Child
            gSaveContext.linkAge = LINK_AGE_CHILD;
            break;
        default:
            break;
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_OVERWORLD_SPAWNS)) {
        // Override the spawn entrance so entrance rando can take control,
        // and to prevent remember save location from breaking initial spawn.
        gSaveContext.entranceIndex = -1;
    }

    for (auto trialFlag : { EVENTCHKINF_COMPLETED_LIGHT_TRIAL, EVENTCHKINF_COMPLETED_FOREST_TRIAL,
                            EVENTCHKINF_COMPLETED_FIRE_TRIAL, EVENTCHKINF_COMPLETED_WATER_TRIAL,
                            EVENTCHKINF_COMPLETED_SPIRIT_TRIAL, EVENTCHKINF_COMPLETED_SHADOW_TRIAL }) {
        if (!OTRGlobals::Instance->gRandomizer->IsTrialRequired(trialFlag)) {
            Flags_SetEventChkInf(trialFlag);
        }
    }

    if (Randomizer_GetSettingValue(RSK_SKIP_CHILD_ZELDA)) {
        GetItemEntry getItemEntry = Randomizer_GetItemFromKnownCheck(RC_SONG_FROM_IMPA, (GetItemID)RG_ZELDAS_LULLABY);
        StartingItemGive(getItemEntry, RC_SONG_FROM_IMPA);
        getItemEntry = Randomizer_GetItemFromKnownCheck(RC_HC_MALON_EGG, (GetItemID)RG_WEIRD_EGG);
        StartingItemGive(getItemEntry, RC_HC_ZELDAS_LETTER);
        getItemEntry = Randomizer_GetItemFromKnownCheck(RC_HC_ZELDAS_LETTER, (GetItemID)RG_ZELDAS_LETTER);
        StartingItemGive(getItemEntry, RC_HC_MALON_EGG);

        // Malon/Talon back at ranch.
        Flags_SetEventChkInf(EVENTCHKINF_OBTAINED_POCKET_EGG);
        Flags_SetRandomizerInf(RAND_INF_WEIRD_EGG);
        Flags_SetEventChkInf(EVENTCHKINF_TALON_WOKEN_IN_CASTLE);
        Flags_SetEventChkInf(EVENTCHKINF_TALON_RETURNED_FROM_CASTLE);

        // Set "Got Zelda's Letter" flag. Also ensures Saria is back at SFM.
        Flags_SetEventChkInf(EVENTCHKINF_OBTAINED_ZELDAS_LETTER);
        Flags_SetRandomizerInf(RAND_INF_ZELDAS_LETTER);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_LETTER_ZELDA);

        // Got item from Impa.
        Flags_SetEventChkInf(EVENTCHKINF_LEARNED_ZELDAS_LULLABY);

        gSaveContext.sceneFlags[SCENE_HYRULE_CASTLE].swch |= (1 << 0x4); // Move milk crates in Hyrule Castle to moat.

        // Set this at the end to ensure we always start with the letter.
        // This is for the off chance, we got the Weird Egg from Impa (which should never happen).
        INV_CONTENT(ITEM_LETTER_ZELDA) = ITEM_LETTER_ZELDA;
    }

    if (Randomizer_GetSettingValue(RSK_SHUFFLE_MASTER_SWORD) && startingAge == RO_AGE_ADULT) {
        GetItemEntry getItemEntry = Randomizer_GetItemFromKnownCheck(RC_TOT_MASTER_SWORD, GI_NONE);
        StartingItemGive(getItemEntry, RC_TOT_MASTER_SWORD);
        Flags_SetRandomizerInf(RAND_INF_TOT_MASTER_SWORD);
    }

    HIGH_SCORE(HS_POE_POINTS) = 1000 - (100 * Randomizer_GetSettingValue(RSK_BIG_POE_COUNT));

    // Open lowest Vanilla Fire Temple locked door (to prevent key logic lockouts).
    // Not done on Keysanity since this lockout is a non-issue when Fire Keys can be found outside the temple.
    u8 keysanity = Randomizer_GetSettingValue(RSK_KEYSANITY) == RO_DUNGEON_ITEM_LOC_ANYWHERE ||
                   Randomizer_GetSettingValue(RSK_KEYSANITY) == RO_DUNGEON_ITEM_LOC_OVERWORLD ||
                   Randomizer_GetSettingValue(RSK_KEYSANITY) == RO_DUNGEON_ITEM_LOC_ANY_DUNGEON;
    if (!ResourceMgr_IsSceneMasterQuest(SCENE_FIRE_TEMPLE) && !keysanity) {
        gSaveContext.sceneFlags[SCENE_FIRE_TEMPLE].swch |= (1 << 0x17);
    }

    // Opens locked Water Temple door in vanilla to prevent softlocks.
    // West door on the middle level that leads to the water raising thing.
    // Happens in 3DS rando and N64 rando as well.
    if (!ResourceMgr_IsSceneMasterQuest(SCENE_WATER_TEMPLE)) {
        gSaveContext.sceneFlags[SCENE_WATER_TEMPLE].swch |= (1 << 0x15);
    }

    // Bake the one-time world-state flags for the open-world / area-access settings
    // (forest, Door of Time, Kakariko gate, Gerudo fortress). Factored out so MultiShip
    // can re-run it when the authoritative server settings arrive after file creation.
    Randomizer_ApplyAreaAccessWorldState();

    // complete mask quest
    if (Randomizer_GetSettingValue(RSK_MASK_QUEST) == RO_MASK_QUEST_COMPLETED) {
        Flags_SetInfTable(INFTABLE_GATE_GUARD_PUT_ON_KEATON_MASK);
        Flags_SetEventChkInf(EVENTCHKINF_PAID_BACK_BUNNY_HOOD_FEE);

        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_KEATON);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_SKULL);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_SPOOKY);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_BUNNY);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_GORON);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_ZORA);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_GERUDO);
        Flags_SetRandomizerInf(RAND_INF_CHILD_TRADES_HAS_MASK_TRUTH);

        gSaveContext.itemGetInf[3] |= 0x100;  // Sold Keaton Mask
        gSaveContext.itemGetInf[3] |= 0x200;  // Sold Skull Mask
        gSaveContext.itemGetInf[3] |= 0x400;  // Sold Spooky Mask
        gSaveContext.itemGetInf[3] |= 0x800;  // Bunny Hood related
        gSaveContext.itemGetInf[3] |= 0x8000; // Obtained Mask of Truth
    }
}
