#include "MultiShipHints.h"

#ifdef ENABLE_MULTISHIP

#include <map>
#include <set>
#include <string>
#include <vector>

#include "MultiShipSeed.h"
#include "soh/Enhancements/randomizer/SeedContext.h"       // Rando::Context
#include "soh/Enhancements/randomizer/static_data.h"        // Rando::StaticData (+ staticHintInfoMap, hintTextTable)
#include "soh/Enhancements/randomizer/hint.h"               // Rando::Hint
#include "soh/Enhancements/randomizer/item.h"               // Rando::Item
#include "soh/Enhancements/randomizer/location.h"           // Rando::Location
#include "soh/Enhancements/randomizer/3drando/hints.hpp"    // StaticHintInfo

// Static per-check region names (RandomizerCheckArea -> English name). MultiShip never runs the
// logic engine, so the per-location logical areas used by standard rando's hints are empty; this
// static table (defined in randomizer_check_objects.cpp) is our region source for reverse hints.
extern std::map<RandomizerCheckArea, std::string> rcAreaNames;

// Free function defined in 3drando/hints.cpp (global scope), not declared in its header.
RandomizerHintTextKey GetRandomGanonJoke();

namespace {

// Display name of world `world` — the player's chosen name, or a "World N" fallback.
std::string MsWorldName(const MultiShipSeed::Data& d, int world) {
    if (world >= 0 && world < static_cast<int>(d.players.size()) && !d.players[world].empty()) {
        return d.players[world];
    }
    return "World " + std::to_string(world + 1);
}

// Owner-aware, coloured display name for RandomizerGet `item` owned by world `ownerWorld`.
// Own items show their plain coloured name; foreign items get the F-037 possessive
// ("<Player>'s <item>" — player in green, item in its own rando colour).
CustomMessage MsItemName(const MultiShipSeed::Data& d, int item, int ownerWorld) {
    Rando::Item& it = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(item));
    const std::string col = it.GetColor();
    const std::string en = it.GetName().GetEnglish();
    const std::string de = it.GetName().GetGerman();
    const std::string fr = it.GetName().GetFrench();
    if (ownerWorld == d.worldId) {
        return CustomMessage(col + en + "%w", col + de + "%w", col + fr + "%w");
    }
    const std::string p = MsWorldName(d, ownerWorld);
    return CustomMessage("%g" + p + "%w's " + col + en + "%w",       // english
                         col + de + "%w von %g" + p + "%w",          // german
                         col + fr + "%w de %g" + p + "%w");          // french
}

// The own-world vanilla location of RandomizerGet `item`, or RC_UNKNOWN_CHECK if none. Used as a
// fallback for reverse hints when the item isn't a routed placement in the store (e.g. an
// un-shuffled item the generator didn't emit).
RandomizerCheck MsVanillaLocationOf(int item) {
    for (int rc = 1; rc < RC_MAX; ++rc) {
        Rando::Location* loc = Rando::StaticData::GetLocation(static_cast<RandomizerCheck>(rc));
        if (loc != nullptr && loc->GetVanillaItem() == static_cast<RandomizerGet>(item)) {
            return static_cast<RandomizerCheck>(rc);
        }
    }
    return RC_UNKNOWN_CHECK;
}

// Cross-world region string for where OUR copy of RandomizerGet `item` lives (reverse hints).
// In our world -> the static region name; in the other player's world -> "<Player>'s world".
CustomMessage MsItemArea(const MultiShipSeed::Data& d, int item) {
    int locWorld = -1;
    int loc = -1;
    for (const auto& pl : d.placements) {
        if (pl.item == item && pl.ownerWorld == d.worldId) {
            locWorld = pl.locWorld;
            loc = pl.loc;
            break;
        }
    }
    if (loc < 0) {
        // Not routed to us in the store — fall back to its vanilla own-world location.
        RandomizerCheck van = MsVanillaLocationOf(item);
        if (van != RC_UNKNOWN_CHECK) {
            locWorld = d.worldId;
            loc = static_cast<int>(van);
        }
    }
    if (loc < 0) {
        return CustomMessage("an unknown place", "einem unbekannten Ort", "un lieu inconnu");
    }
    if (locWorld == d.worldId) {
        RandomizerCheckArea rca = Rando::StaticData::GetLocation(static_cast<RandomizerCheck>(loc))->GetArea();
        auto entry = rcAreaNames.find(rca);
        const std::string name = (entry != rcAreaNames.end()) ? entry->second : std::string("an unknown place");
        return CustomMessage(name, name, name);  // the static region table is English-only
    }
    const std::string p = MsWorldName(d, locWorld);
    return CustomMessage("%g" + p + "%w's world", "%g" + p + "%ws Welt", "le monde de %g" + p + "%w");
}

// Build a HINT_TYPE_MESSAGE Hint from the real hint-text template(s), substituting `inserts` into
// their [[n]] slots. Reusing the templates keeps standard rando's exact wording + translations;
// GetClear() sidesteps the (un-honored) hint-clarity setting. The unchanged StaticHints.cpp
// builders render messages[id] and apply the final AutoFormat.
Rando::Hint MsBuild(RandomizerHint rh, const std::vector<RandomizerHintTextKey>& keys,
                    const std::vector<CustomMessage>& inserts) {
    std::vector<CustomMessage> msgs;
    msgs.reserve(keys.size());
    for (RandomizerHintTextKey key : keys) {
        CustomMessage m = Rando::StaticData::hintTextTable[key].GetClear();
        m.InsertNames(inserts);
        msgs.push_back(m);
    }
    return Rando::Hint(rh, msgs);
}

bool MsOptOn(RandomizerSettingKey k) {
    return Rando::Context::GetInstance()->GetOption(k).Get() != RO_GENERIC_OFF;
}

// Temple of Time altar (child/adult). Always emits the win-condition text (door-of-time for child;
// Rainbow Bridge + Ganon's Boss Key requirements for adult, all settings-computed). The spiritual
// stone / medallion REGIONS are revealed only when RSK_TOT_ALTAR_HINT is on. Mirrors the engine's
// HINT_TYPE_ALTAR_* rendering (hint.cpp) but with cross-world-aware regions.
void MsBuildAltar(const MultiShipSeed::Data& d, bool adult) {
    auto ctx = Rando::Context::GetInstance();
    const RandomizerHint rh = adult ? RH_ALTAR_ADULT : RH_ALTAR_CHILD;

    CustomMessage msg;
    if (MsOptOn(RSK_TOT_ALTAR_HINT)) {
        if (adult) {
            msg = Rando::StaticData::hintTextTable[RHT_ADULT_ALTAR_MEDALLIONS].GetClear();
            msg.InsertNames({ MsItemArea(d, RG_LIGHT_MEDALLION), MsItemArea(d, RG_FOREST_MEDALLION),
                              MsItemArea(d, RG_FIRE_MEDALLION), MsItemArea(d, RG_WATER_MEDALLION),
                              MsItemArea(d, RG_SPIRIT_MEDALLION), MsItemArea(d, RG_SHADOW_MEDALLION) });
        } else {
            msg = Rando::StaticData::hintTextTable[RHT_CHILD_ALTAR_STONES].GetClear();
            msg.InsertNames({ MsItemArea(d, RG_KOKIRI_EMERALD), MsItemArea(d, RG_GORON_RUBY),
                              MsItemArea(d, RG_ZORA_SAPPHIRE) });
        }
    } else {
        msg = CustomMessage("");
        msg.SetTextBoxType(TEXTBOX_TYPE_BLUE);
    }

    if (adult) {
        msg += Rando::Hint::GetBridgeReqsText();
        msg += Rando::Hint::GetGanonBossKeyText();
        msg += Rando::StaticData::hintTextTable[RHT_ADULT_ALTAR_TEXT_END].GetClear();
    } else if (ctx->GetOption(RSK_DOOR_OF_TIME).Is(RO_DOOROFTIME_OPEN)) {
        msg += Rando::StaticData::hintTextTable[RHT_CHILD_ALTAR_TEXT_END_DOTOPEN].GetClear();
    } else if (ctx->GetOption(RSK_DOOR_OF_TIME).Is(RO_DOOROFTIME_SONGONLY)) {
        msg += Rando::StaticData::hintTextTable[RHT_CHILD_ALTAR_TEXT_END_DOTSONGONLY].GetClear();
    } else {
        msg += Rando::StaticData::hintTextTable[RHT_CHILD_ALTAR_TEXT_END_DOTCLOSED].GetClear();
    }

    ctx->AddHint(rh, Rando::Hint(rh, { msg }));
}

} // namespace

CustomMessage MultiShip_HintItemName(RandomizerCheck check) {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (d.ready && d.worldId >= 0) {
        for (const auto& pl : d.placements) {
            if (pl.locWorld == d.worldId && pl.loc == static_cast<int>(check)) {
                return MsItemName(d, pl.item, pl.ownerWorld);
            }
        }
        // Not routed — the check holds its own-world vanilla item.
        Rando::Location* loc = Rando::StaticData::GetLocation(check);
        if (loc != nullptr) {
            return MsItemName(d, static_cast<int>(loc->GetVanillaItem()), d.worldId);
        }
    }
    return CustomMessage("an unknown item", "einem unbekannten Gegenstand", "un objet inconnu");
}

void MultiShip_BuildStaticHints() {
    MultiShipSeed::Data d = MultiShipSeed::Snapshot();
    if (!d.ready || d.worldId < 0) {
        return;
    }
    auto ctx = Rando::Context::GetInstance();
    if (ctx == nullptr) {
        return;
    }

    // Static hints render as CLEAR. RSK_HINT_CLARITY is not a MultiShip setting and its default
    // index is Obscure(0); GetBridgeReqsText/GetGanonBossKeyText and the templates read it, so pin
    // it to Clear for a MultiShip game (harmless — the setting is otherwise unused here).
    ctx->GetOption(RSK_HINT_CLARITY).Set(static_cast<uint8_t>(RO_HINT_CLARITY_CLEAR));

    // Data-driven NPC hints: every entry in the engine's static-hint table except the 6 MultiShip-
    // omitted givers and the specially-shaped Ganondorf / Altar (handled below; not in this map).
    static const std::set<RandomizerHint> kOmitted = {
        RH_LOACH_HINT, RH_FISHING_POLE, RH_MALON_HINT, RH_MASK_SHOP_HINT,
    };
    for (const auto& [rh, info] : Rando::StaticData::staticHintInfoMap) {
        if (kOmitted.count(rh)) {
            continue;
        }
        if (!MsOptOn(info.setting)) {
            continue;
        }
        std::vector<CustomMessage> inserts;
        if (!info.targetChecks.empty()) {
            // Forward: reveal the item at each fixed reward check (owner-aware).
            for (RandomizerCheck c : info.targetChecks) {
                inserts.push_back(MultiShip_HintItemName(c));
            }
        } else {
            // Reverse: reveal the region where our copy of each target item lives (cross-world).
            for (RandomizerGet it : info.targetItems) {
                inserts.push_back(MsItemArea(d, static_cast<int>(it)));
            }
        }
        ctx->AddHint(rh, MsBuild(rh, info.hintKeys, inserts));
    }

    // Ganondorf: LA-only, or the 3-variant Light-Arrows/Master-Sword set under Master Sword shuffle
    // (mirrors CreateGanondorfHint; item order LA-then-MS drives the templates' [[1]]/[[2]] slots).
    // The joke line is shown by the builder when the player already holds the Light Arrows.
    if (MsOptOn(RSK_GANONDORF_HINT)) {
        if (MsOptOn(RSK_SHUFFLE_MASTER_SWORD) && ctx->GetOption(RSK_STARTING_MASTER_SWORD).Is(RO_GENERIC_OFF)) {
            ctx->AddHint(RH_GANONDORF_HINT,
                         MsBuild(RH_GANONDORF_HINT,
                                 { RHT_GANONDORF_HINT_LA_ONLY, RHT_GANONDORF_HINT_MS_ONLY,
                                   RHT_GANONDORF_HINT_LA_AND_MS },
                                 { MsItemArea(d, RG_LIGHT_ARROWS), MsItemArea(d, RG_MASTER_SWORD) }));
        } else {
            ctx->AddHint(RH_GANONDORF_HINT, MsBuild(RH_GANONDORF_HINT, { RHT_GANONDORF_HINT_LA_ONLY },
                                                    { MsItemArea(d, RG_LIGHT_ARROWS) }));
        }
        ctx->AddHint(RH_GANONDORF_JOKE,
                     Rando::Hint(RH_GANONDORF_JOKE, HINT_TYPE_HINT_KEY, { GetRandomGanonJoke() }));
    }

    // Temple of Time altar (child + adult).
    MsBuildAltar(d, false);
    MsBuildAltar(d, true);
}

#endif // ENABLE_MULTISHIP
