/**
 * This file handles custom messages relating to Items,
 * such as Get Item messages for non-vanilla items,
 * Vanilla/MQ hints when collecting Maps, Ice Trap messages,
 * etc.
 */
#include <soh/OTRGlobals.h>
#include <soh/util.h>  // SohUtils::GetItemName (foreign MOD_NONE item name, F-040)
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/custom-message/CustomMessageTypes.h"
#include "soh/Enhancements/randomizer/Traps.h"
#include "soh/Enhancements/randomizer/item.h"
#include "soh/Enhancements/randomizer/randomizer.h"
#include "soh/ShipInit.hpp"
#include <soh/ResourceManagerHelpers.h>

#include <cstdarg>

extern "C" {
#include <variables.h>
#include <macros.h>
#include "z64item.h"
extern PlayState* gPlayState;
}

#ifdef ENABLE_MULTISHIP
// F-040: the get-item textbox for a cross-world item shows "You found <Player>'s <item>!".
// gForeignItemOwner is armed for the duration of a foreign get-item (hook_handlers.cpp);
// gForeignItemCheck is the backing RandomizerCheck (so a disguised ice trap can resolve its fake
// model from the Context override); MultiShip_GetPlayerName resolves a world index to its name.
extern "C" s32 Randomizer_GetForeignItemOwner(void);
extern "C" s32 Randomizer_GetForeignItemCheck(void);
std::string MultiShip_GetPlayerName(int world);
#endif

void BuildTriforcePieceMessage(CustomMessage& msg) {
    uint8_t current = gSaveContext.ship.quest.data.randomizer.triforcePiecesCollected + 1;
    uint8_t required = OTRGlobals::Instance->gRandomizer->GetRandoSettingValue(RSK_TRIFORCE_HUNT_PIECES_REQUIRED) + 1;
    uint8_t remaining = required - current;
    float percentageCollected = (float)current / (float)required;

    if (percentageCollected <= 0.25) {
        msg = { "You found a %yTriforce Piece%w!&%g[[current]]%w down, %c[[remaining]]%w to go. It's a start!",
                "Ein %yTriforce-Splitter%w! Du hast&%g[[current]]%w von %c[[required]]%w gefunden. Es ist ein&Anfang!",
                "Vous trouvez un %yFragment de la&Triforce%w! Vous en avez %g[[current]]%w, il en&reste "
                "%c[[remaining]]%w à trouver. C'est un début!" };
    } else if (percentageCollected <= 0.5) {
        msg = { "You found a %yTriforce Piece%w!&%g[[current]]%w down, %c[[remaining]]%w to go. Progress!",
                "Ein %yTriforce-Splitter%w! Du hast&%g[[current]]%w von %c[[required]]%w gefunden. Es geht voran!",
                "Vous trouvez un %yFragment de la&Triforce%w! Vous en avez %g[[current]]%w, il en&reste "
                "%c[[remaining]]%w à trouver. Ça avance!" };
    } else if (percentageCollected <= 0.75) {
        msg = { "You found a %yTriforce Piece%w!&%g[[current]]%w down, %c[[remaining]]%w to go. Over half-way&there!",
                "Ein %yTriforce-Splitter%w! Du hast&schon %g[[current]]%w von %c[[required]]%w gefunden. Schon&über "
                "die Hälfte!",
                "Vous trouvez un %yFragment de la&Triforce%w! Vous en avez %g[[current]]%w, il en&reste "
                "%c[[remaining]]%w à trouver. Il en reste un&peu moins que la moitié!" };
    } else if (percentageCollected < 1.0) {
        msg = {
            "You found a %yTriforce Piece%w!&%g[[current]]%w down, %c[[remaining]]%w to go. Almost done!",
            "Ein %yTriforce-Splitter%w! Du hast&schon %g[[current]]%w von %c[[required]]%w gefunden. Fast&geschafft!",
            "Vous trouvez un %yFragment de la&Triforce%w! Vous en avez %g[[current]]%w, il en&reste %c[[remaining]]%w "
            "à trouver. C'est presque&terminé!"
        };
    } else if (current == required) {
        msg = { "You completed the %yTriforce of&Courage%w! %gGG%w!",
                "Das %yTriforce des Mutes%w! Du hast&alle Splitter gefunden. %gGut gemacht%w!",
                "Vous avez complété la %yTriforce&du Courage%w! %gFélicitations%w!" };
    } else {
        msg = { "You found a spare %yTriforce Piece%w!&You only needed %c[[required]]%w, but you have %g[[current]]%w!",
                "Ein übriger %yTriforce-Splitter%w! Du&hast nun %g[[current]]%w von %c[[required]]%w nötigen gefunden.",
                "Vous avez trouvé un %yFragment de&Triforce%w en plus! Vous n'aviez besoin&que de %c[[required]]%w, "
                "mais vous en avez %g[[current]]%w en&tout!" };
    }
    msg.Replace("[[current]]", std::to_string(current));
    msg.Replace("[[remaining]]", std::to_string(remaining));
    msg.Replace("[[required]]", std::to_string(required));
    msg.Format(ITEM_CUSTOM);
}

// MultiShip displays rando's "Power Bracelet" (the grab-ability strength tier) under the vanilla
// name "Goron's Bracelet" — matching the equipment subscreen. Name only; the icon/color/model are
// untouched. Returns rgid unchanged outside MultiShip, so standard rando is unaffected.
static int16_t MultiShipDisplayNameRg(int16_t rgid) {
#ifdef ENABLE_MULTISHIP
    if (IS_MULTISHIP && rgid == RG_POWER_BRACELET) {
        return RG_GORONS_BRACELET;
    }
#endif
    return rgid;
}

void BuildCustomItemMessage(Player* player, CustomMessage& msg) {
    int16_t rgid;
    msg = CustomMessage("You found [[article]][[color]][[name]]%w!",
                        "Du erhältst [[article]][[color]][[name]]%w gefunden!",
                        "Vous avez trouvé [[article]][[color]][[name]]%w!", TEXTBOX_TYPE_BLUE);
    if (player->getItemEntry.objectId != OBJECT_INVALID) {
        rgid = player->getItemEntry.getItemId;
    } else {
        rgid = player->getItemId;
    }
#ifdef ENABLE_MULTISHIP
    // F-040: a cross-world item collected for another player gets a possessive message
    // ("You found <Player>'s <item>!") and drops the article. gForeignItemOwner is armed by the
    // item-queue handler for the duration of this get-item (read here before z_player consumes
    // it). Our own / server-delivered items leave it -1 and fall through to the normal message.
    if (Randomizer_GetForeignItemOwner() >= 0) {
        const int ownerWorld = Randomizer_GetForeignItemOwner();
        std::string owner = MultiShip_GetPlayerName(ownerWorld);
        if (owner.empty()) {
            owner = "World " + std::to_string(ownerWorld + 1);
        }
        // Style matches the local get-item box (TEXTBOX_TYPE_BLUE + the item icon); wording per the
        // MultiShip design: "You've found <item> from <Player>!" with the player name in %ggreen%w
        // and progression (advancement) item names in %rred%w.
        // Vanilla-table (MOD_NONE) item: it has no RandomizerGet, so use its plain vanilla name (no
        // rando color/icon). z_player routed this textbox here; the item still goes to its owner.
        if (player->getItemEntry.modIndex == MOD_NONE) {
            std::string itemName = SohUtils::GetItemName(player->getItemEntry.itemId);
            msg = CustomMessage("You've found %r" + itemName + "%w from %g" + owner + "%w!",
                                "Du hast %r" + itemName + "%w von %g" + owner + "%w gefunden!",
                                "Vous avez trouvé %r" + itemName + "%w de %g" + owner + "%w!", TEXTBOX_TYPE_BLUE);
            msg.AutoFormat();
            return;
        }
        // A foreign ice trap must show its DISGUISE, not "Ice Trap" — read the fake model the
        // override stored for this check (mirrors base rando's shop/textbox disguise resolution).
        if (rgid == RG_ICE_TRAP) {
            const int check = Randomizer_GetForeignItemCheck();
            auto ctx = OTRGlobals::Instance->gRandoContext;
            if (check >= 0 && ctx != nullptr && ctx->overrides.contains(static_cast<RandomizerCheck>(check))) {
                rgid = ctx->overrides[static_cast<RandomizerCheck>(check)].LooksLike();
            }
        }
        // Progression (advancement) items get a red name; everything else keeps its own rando color.
        std::string nameColor = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rgid)).IsAdvancement()
                                    ? "%r"
                                    : Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rgid)).GetColor();
        msg = CustomMessage("You've found " + nameColor + "[[name]]%w from %g" + owner + "%w!",
                            "Du hast " + nameColor + "[[name]]%w von %g" + owner + "%w gefunden!",
                            "Vous avez trouvé " + nameColor + "[[name]]%w de %g" + owner + "%w!", TEXTBOX_TYPE_BLUE);
        CustomMessage foreignName = CustomMessage(
            Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(MultiShipDisplayNameRg(rgid))).GetName(),
            TEXTBOX_TYPE_BLUE);
        msg.Replace("[[name]]", foreignName);
        if (Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rgid)).HasCustomIcon()) {
            msg.AutoFormat(ITEM_CUSTOM);
        } else {
            msg.AutoFormat();
        }
        return;
    }
#endif
    CustomMessage name = CustomMessage(
        Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(MultiShipDisplayNameRg(rgid))).GetName(),
        TEXTBOX_TYPE_BLUE);
    CustomMessage article = CustomMessage(
        Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rgid)).GetArticle(), TEXTBOX_TYPE_BLUE);
    msg.Replace("[[article]]", article);
    msg.Replace("[[color]]", Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rgid)).GetColor());
    msg.Replace("[[name]]", name);
    if (Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(rgid)).HasCustomIcon()) {
        msg.AutoFormat(ITEM_CUSTOM);
    } else {
        msg.AutoFormat();
    }
}

void LoadCustomItemIcon(bool displayAsEnglish) {
    Player* player = GET_PLAYER(gPlayState);
    const char* customIcon = nullptr;
    CustomIconSize iconSize = ICON_SIZE_32;
    if (player->getItemEntry.objectId != OBJECT_INVALID) {
        RandomizerGet rgid = static_cast<RandomizerGet>(player->getItemEntry.getItemId);
        customIcon = Rando::StaticData::RetrieveItem(rgid).GetCustomIcon();
        iconSize = Rando::StaticData::RetrieveItem(rgid).GetCustomIconSize();
    }
    if (customIcon != nullptr) {
        static int16_t sIconItem32XOffsets[] = { 74, 74, 74, 54 };
        static int16_t sIconItem24XOffsets[] = { 72, 72, 72, 50 };
        MessageContext* msgCtx = &gPlayState->msgCtx;
        uint8_t language = displayAsEnglish ? LANGUAGE_ENG : gSaveContext.language;
        if (iconSize == ICON_SIZE_32) {
            R_TEXTBOX_ICON_XPOS = R_TEXT_INIT_XPOS - sIconItem32XOffsets[language];
            R_TEXTBOX_ICON_YPOS = (R_TEXTBOX_Y + 10) + 6;
            R_TEXTBOX_ICON_SIZE = 32;
        } else {
            R_TEXTBOX_ICON_XPOS = R_TEXT_INIT_XPOS - sIconItem24XOffsets[language];
            R_TEXTBOX_ICON_YPOS = (R_TEXTBOX_Y + 10) + 10;
            R_TEXTBOX_ICON_SIZE = 24;
        }
        strcpy((char*)((uintptr_t)msgCtx->textboxSegment + MESSAGE_STATIC_TEX_SIZE), customIcon);
        msgCtx->msgBufPos++;
        msgCtx->choiceNum = 1;
    }
}

void DrawCustomItemIcon(Gfx** p) {
    Gfx* gfx = *p;
    MessageContext* msgCtx = &gPlayState->msgCtx;
    Player* player = GET_PLAYER(gPlayState);
    CustomIconSize iconSize = ICON_SIZE_32;
    if (player->getItemEntry.objectId != OBJECT_INVALID) {
        RandomizerGet rgid = static_cast<RandomizerGet>(player->getItemEntry.getItemId);
        iconSize = Rando::StaticData::RetrieveItem(rgid).GetCustomIconSize();
    }
    if (iconSize == ICON_SIZE_24) {
        gDPLoadTextureBlock(gfx++, (uintptr_t)msgCtx->textboxSegment + MESSAGE_STATIC_TEX_SIZE, G_IM_FMT_RGBA,
                            G_IM_SIZ_32b, 24, 24, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                            G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    } else {
        gDPLoadTextureBlock(gfx++, (uintptr_t)msgCtx->textboxSegment + MESSAGE_STATIC_TEX_SIZE, G_IM_FMT_RGBA,
                            G_IM_SIZ_32b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK,
                            G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    }
    *p = gfx;
}

void BuildItemMessage(u16* textId, bool* loadFromMessageTable) {
    Player* player = GET_PLAYER(gPlayState);
    CustomMessage msg;

    if (player->getItemEntry.getItemId == RG_ICE_TRAP
#ifdef ENABLE_MULTISHIP
        // A FOREIGN ice trap fires on its owner, not on us — show its disguise ("You found
        // <Player>'s <fake item>!") instead of the troll/reveal message. Our own ice traps still
        // get the troll message below.
        && Randomizer_GetForeignItemOwner() < 0
#endif
    ) {
        Rando::Traps::BuildIceTrapMessage(msg, player->getItemEntry);
    } else if (player->getItemEntry.getItemId == RG_TRIFORCE_PIECE) {
        BuildTriforcePieceMessage(msg);
    } else {
        BuildCustomItemMessage(player, msg);
    }
    *loadFromMessageTable = false;
    msg.LoadIntoFont();
}

void BuildMapMessage(uint16_t* textId, bool* loadFromMessageTable) {
    GetItemEntry itemEntry = GET_PLAYER(gPlayState)->getItemEntry;
    auto ctx = OTRGlobals::Instance->gRandoContext;
    CustomMessage msg =
        CustomMessage("You found the %g[[name]]%w! [[typeHint]]", "Du erhältst das %g[[name]]%w! [[typeHint]]",
                      "Vous ebtenez %g[[name]]%w! [[typeHint]]", TEXTBOX_TYPE_BLUE);
    int sceneNum;
    switch (itemEntry.getItemId) {
        case RG_DEKU_TREE_MAP:
            sceneNum = SCENE_DEKU_TREE;
            break;
        case RG_DODONGOS_CAVERN_MAP:
            sceneNum = SCENE_DODONGOS_CAVERN;
            break;
        case RG_JABU_JABUS_BELLY_MAP:
            sceneNum = SCENE_JABU_JABU;
            break;
        case RG_FOREST_TEMPLE_MAP:
            sceneNum = SCENE_FOREST_TEMPLE;
            break;
        case RG_FIRE_TEMPLE_MAP:
            sceneNum = SCENE_FIRE_TEMPLE;
            break;
        case RG_WATER_TEMPLE_MAP:
            sceneNum = SCENE_WATER_TEMPLE;
            break;
        case RG_SPIRIT_TEMPLE_MAP:
            sceneNum = SCENE_SPIRIT_TEMPLE;
            break;
        case RG_SHADOW_TEMPLE_MAP:
            sceneNum = SCENE_SHADOW_TEMPLE;
            break;
        case RG_BOTTOM_OF_THE_WELL_MAP:
            sceneNum = SCENE_BOTTOM_OF_THE_WELL;
            break;
        case RG_ICE_CAVERN_MAP:
            sceneNum = SCENE_ICE_CAVERN;
            break;
    }
    CustomMessage name =
        CustomMessage(Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(itemEntry.getItemId)).GetName());
    msg.Replace("[[name]]", name);
    if (ctx->GetOption(RSK_MQ_DUNGEON_RANDOM).Is(RO_MQ_DUNGEONS_NONE) ||
        (ctx->GetOption(RSK_MQ_DUNGEON_RANDOM).Is(RO_MQ_DUNGEONS_SET_NUMBER) &&
         ctx->GetOption(RSK_MQ_DUNGEON_COUNT).Is(MAX_MQ_DUNGEON_COUNT))) {
        msg.Replace("[[typeHint]]", "");
    } else if (ResourceMgr_IsSceneMasterQuest(sceneNum)) {
        msg.Replace("[[typeHint]]", Rando::StaticData::hintTextTable[RHT_DUNGEON_MASTERFUL].GetHintMessage());
    } else {
        msg.Replace("[[typeHint]]", Rando::StaticData::hintTextTable[RHT_DUNGEON_ORDINARY].GetHintMessage());
    }
    *loadFromMessageTable = false;
    msg.AutoFormat(ITEM_DUNGEON_MAP);
    msg.LoadIntoFont();
}

void BuildBossKeyMessage(uint16_t* textId, bool* loadFromMessageTable) {
    Player* player = GET_PLAYER(gPlayState);
    if (player->getItemEntry.getItemId == RG_GANONS_CASTLE_BOSS_KEY &&
        !DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_GANONS_BOSS_KEY)) {
        return;
    }
    if (player->getItemEntry.getItemId != RG_GANONS_CASTLE_BOSS_KEY &&
        !DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_BOSS_KEYSANITY)) {
        return;
    }
    CustomMessage msg;
    BuildCustomItemMessage(player, msg);
    *loadFromMessageTable = false;
    msg.LoadIntoFont();
}

void BuildSmallKeyMessage(uint16_t* textId, bool* loadFromMessageTable) {
    Player* player = GET_PLAYER(gPlayState);
    if (player->getItemEntry.getItemId == RG_GERUDO_FORTRESS_SMALL_KEY &&
        OTRGlobals::Instance->gRandoContext->GetOption(RSK_GERUDO_KEYS).Is(RO_GERUDO_KEYS_VANILLA)) {
        return;
    }
    if (player->getItemEntry.getItemId != RG_GERUDO_FORTRESS_SMALL_KEY &&
        DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_KEYSANITY)) {
        return;
    }
    CustomMessage msg;
    BuildCustomItemMessage(player, msg);
    *loadFromMessageTable = false;
    msg.LoadIntoFont();
}

void RegisterItemMessages() {
    COND_ID_HOOK(OnOpenText, TEXT_RANDOMIZER_CUSTOM_ITEM, IS_RANDO, BuildItemMessage);
    COND_ID_HOOK(OnOpenText, TEXT_ITEM_DUNGEON_MAP, DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_SHUFFLE_MAPANDCOMPASS),
                 BuildMapMessage);
    COND_ID_HOOK(OnOpenText, TEXT_ITEM_COMPASS, DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_SHUFFLE_MAPANDCOMPASS),
                 BuildItemMessage);
    COND_ID_HOOK(OnOpenText, TEXT_ITEM_KEY_BOSS,
                 (DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_BOSS_KEYSANITY) ||
                  DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_GANONS_BOSS_KEY)),
                 BuildBossKeyMessage);
    COND_ID_HOOK(OnOpenText, TEXT_ITEM_KEY_SMALL,
                 (OTRGlobals::Instance->gRandoContext->GetOption(RSK_GERUDO_KEYS).IsNot(RO_GERUDO_KEYS_VANILLA) ||
                  DUNGEON_ITEMS_CAN_BE_OUTSIDE_DUNGEON(RSK_KEYSANITY)),
                 BuildSmallKeyMessage);
}

static RegisterShipInitFunc initFunc(RegisterItemMessages, { "IS_RANDO" });

void RegisterCustomIconHooks() {
    // The custom in-textbox item icon for MOD_RANDOMIZER items also applies to a MultiShip game
    // (it reuses the same get-item textbox); without it the icon control code renders as a stray
    // glyph. Additive: the IS_RANDO branch is unchanged, so standard rando is unaffected.
    bool customIconCond = IS_RANDO;
#ifdef ENABLE_MULTISHIP
    customIconCond = customIconCond || IS_MULTISHIP;
#endif
    COND_VB_SHOULD(VB_LOAD_ITEM_ICON, customIconCond, {
        if (*should == false) {
            LoadCustomItemIcon(static_cast<bool>(va_arg(args, int)));
        }
    });
    COND_VB_SHOULD(VB_DRAW_ITEM_ICON, customIconCond, {
        if (*should == false) {
            DrawCustomItemIcon(va_arg(args, Gfx**));
        }
    });
}

static RegisterShipInitFunc customIconInitFunc(RegisterCustomIconHooks, { "IS_RANDO" });