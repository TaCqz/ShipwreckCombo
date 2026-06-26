#include "MultiShipSeed.h"

#ifdef ENABLE_MULTISHIP

#include <cstdint>
#include <spdlog/spdlog.h>

namespace {

// The whole store lives behind one mutex: the network thread writes it (seed-info +
// the granted seed both arrive on Network's receive thread), while the main thread
// reads it (menu grey-out, file creation) and SaveManager serializes it.
std::mutex gMutex;
std::string gSeedIdInfo;                 // seed id from the seed-info push
std::vector<std::string> gKnownPlayers;  // world names from the seed-info push (for validation)
MultiShipSeed::Data gData;               // the full deserialized seed (gData.ready once present)
std::string gStatus;                     // last 'Start Multiworld Save' request status line
std::set<int> gCollected;                // F-040: RandomizerCheck ids already collected in our world

// --- base64 (RFC 4648, standard alphabet, '=' padding). The server base64-encodes the
// v3 blob because the transport is NUL-delimited and the raw bytes contain '\0'. ------
int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' padding or any other char
}

bool Base64Decode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' ) break;            // padding: nothing more to decode
        if (c == '\n' || c == '\r' || c == ' ') continue;  // tolerate whitespace
        int v = B64Val(c);
        if (v < 0) return false;          // invalid character
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

// --- Little-endian byte reader over the decoded v3 blob. Reads match the server's
// SerializeV3 (raw LE writes): every multi-byte field is LE; a str is a u8 length
// prefix followed by that many bytes. Any over-read sets `ok=false` and stops. -------
struct Reader {
    const unsigned char* p;
    size_t n;
    size_t i = 0;
    bool ok = true;

    bool need(size_t k) {
        if (!ok || i + k > n) { ok = false; return false; }
        return true;
    }
    uint8_t u8() {
        if (!need(1)) return 0;
        return p[i++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = (uint16_t)p[i] | ((uint16_t)p[i + 1] << 8);
        i += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) |
                     ((uint32_t)p[i + 3] << 24);
        i += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    std::string str() {
        uint8_t len = u8();
        if (!need(len)) return std::string();
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
};

constexpr uint32_t kV3Version = 3;

} // namespace

namespace MultiShipSeed {

void SetKnownPlayers(const std::string& seedId, const std::vector<std::string>& players) {
    std::lock_guard<std::mutex> lk(gMutex);
    gSeedIdInfo = seedId;
    gKnownPlayers = players;
}

std::vector<std::string> GetKnownPlayers() {
    std::lock_guard<std::mutex> lk(gMutex);
    return gKnownPlayers;
}

bool IsNameValid(const std::string& name) {
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lk(gMutex);
    for (const std::string& p : gKnownPlayers) {
        if (p == name) return true;
    }
    return false;
}

bool DeserializeV3FromBase64(const std::string& base64, int worldId, std::string& err) {
    std::string bytes;
    if (!Base64Decode(base64, bytes)) {
        err = "base64 decode failed";
        return false;
    }
    Reader r{ reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size() };

    // Header: "MSHP" | version:u32 | seed:u64 | seedId:str | numWorlds:u8
    char magic[4] = { (char)r.u8(), (char)r.u8(), (char)r.u8(), (char)r.u8() };
    if (!r.ok || magic[0] != 'M' || magic[1] != 'S' || magic[2] != 'H' || magic[3] != 'P') {
        err = "bad magic (expected MSHP)";
        return false;
    }
    uint32_t version = r.u32();
    if (version != kV3Version) {
        err = "unsupported version " + std::to_string(version) + " (expected 3)";
        return false;
    }

    Data d;
    d.seed = r.u64();
    d.seedId = r.str();
    uint8_t numWorlds = r.u8();
    d.players.reserve(numWorlds);
    for (uint8_t w = 0; w < numWorlds && r.ok; ++w) {
        d.players.push_back(r.str());
    }

    // placementCount:u32 | per placement: locWorld:u8, loc:u16, ownerWorld:u8, item:u16
    uint32_t placementCount = r.u32();
    d.placements.reserve(placementCount);
    for (uint32_t k = 0; k < placementCount && r.ok; ++k) {
        Placement pl;
        pl.locWorld = r.u8();
        pl.loc = r.u16();
        pl.ownerWorld = r.u8();
        pl.item = r.u16();
        d.placements.push_back(pl);
    }

    // settingCount:u16 | per setting: key:u16, value:u16
    uint16_t settingCount = r.u16();
    d.settings.reserve(settingCount);
    for (uint16_t s = 0; s < settingCount && r.ok; ++s) {
        Setting st;
        st.key = r.u16();
        st.value = r.u16();
        d.settings.push_back(st);
    }

    if (!r.ok) {
        err = "truncated / malformed v3 payload";
        return false;
    }

    d.worldId = worldId;
    d.ready = true;
    const std::string sid = d.seedId;  // captured for the log before `d` is moved out
    {
        std::lock_guard<std::mutex> lk(gMutex);
        gData = std::move(d);
        // A freshly received seed is a brand-new world: start with no collected checks.
        // The load path (SetCollected from the save) overwrites this for an existing file.
        gCollected.clear();
    }
    SPDLOG_INFO("[MultiShip] Deserialized v3 seed {} (worldId {}, {} players, {} placements, {} settings)",
                sid, worldId, (int)numWorlds, (int)placementCount, (int)settingCount);
    return true;
}

bool IsReady() {
    std::lock_guard<std::mutex> lk(gMutex);
    return gData.ready;
}

Data Snapshot() {
    std::lock_guard<std::mutex> lk(gMutex);
    return gData;
}

void LoadFromSnapshot(const Data& data) {
    std::lock_guard<std::mutex> lk(gMutex);
    gData = data;
    gData.ready = true;
}

void Clear() {
    std::lock_guard<std::mutex> lk(gMutex);
    gData = Data{};
}

void MarkCollected(int check) {
    std::lock_guard<std::mutex> lk(gMutex);
    gCollected.insert(check);
}

bool IsCollected(int check) {
    std::lock_guard<std::mutex> lk(gMutex);
    return gCollected.count(check) != 0;
}

std::vector<int> GetCollected() {
    std::lock_guard<std::mutex> lk(gMutex);
    return std::vector<int>(gCollected.begin(), gCollected.end());
}

void SetCollected(const std::vector<int>& checks) {
    std::lock_guard<std::mutex> lk(gMutex);
    gCollected = std::set<int>(checks.begin(), checks.end());
}

void ClearCollected() {
    std::lock_guard<std::mutex> lk(gMutex);
    gCollected.clear();
}

void SetStatus(const std::string& status) {
    std::lock_guard<std::mutex> lk(gMutex);
    gStatus = status;
}

std::string GetStatus() {
    std::lock_guard<std::mutex> lk(gMutex);
    return gStatus;
}

} // namespace MultiShipSeed

// C-callable readiness gate for the file-creation code (z_sram.c / z_file_choose.c).
extern "C" bool MultiShip_IsSeedReady(void) {
    return MultiShipSeed::IsReady();
}

#endif // ENABLE_MULTISHIP
