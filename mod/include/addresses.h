// Dark Souls 2: Scholar of the First Sin - Memory Addresses & Patterns
// Sources:
//   - Bob Edition v4.09.5 Cheat Table (data offsets)
//   - ds3os project by TLeonardUK (AOB patterns, network protocol)
//   - DS2S-META by pseudostripy/Nordgaren (pointer chains)

#pragma once

#include <cstdint>

namespace DS2Coop {
namespace Addresses {

// ============================================================================
// AOB patterns for finding base data pointers
// These are from community cheat tables and survive game updates
// ============================================================================

struct AOBPattern {
    const char* name;
    const char* pattern;
    const char* mask;
    int offset_from_match;   // Offset within instruction to RIP-relative value
    int pointer_offset;      // Full instruction size for RIP resolution (instruction end)
};

// GameManagerImp: Main game manager containing player data, world state
constexpr AOBPattern GAME_MANAGER_IMP = {
    "GameManagerImp",
    "\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x58\x38\x48\x85\xDB\x74\x00\xF6",
    "xxx????xxxxxxxx?x",
    3, 7
};

// NetSessionManager: Network session manager for multiplayer
constexpr AOBPattern NET_SESSION_MANAGER = {
    "NetSessionManager",
    "\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9\x74\x00\x48\x8B\x49\x18\xE8",
    "xxx????xxxx?xxxxx",
    3, 7
};

// KatanaMainApp: Main application manager (optional)
constexpr AOBPattern KATANA_MAIN_APP = {
    "KatanaMainApp",
    "\x48\x8B\x15\x00\x00\x00\x00\x45\x32\xC0\x85\xC9",
    "xxx????xxxxx",
    3, 7
};

// ============================================================================
// PROTOBUF HOOK PATTERNS (from ds3os - VERIFIED WORKING)
// These hook the game's protobuf serialization/deserialization functions.
// By intercepting these we can catch and block disconnect messages.
// ============================================================================

// SerializeWithCachedSizesToArray - catches ALL outgoing protobuf messages
// This is the function the game calls when sending any network message.
// We hook it to inspect the message type and block disconnect/leave messages.
//
// ds3os found this with a 76-byte AOB scan. We store it as raw bytes + mask.
// The 7 wildcard bytes at offset 28 skip a module-relative address.
namespace ProtobufPatterns {
    // Serialize function prolog
    constexpr const char* SERIALIZE_PATTERN =
        "\x40\x55\x56\x57"
        "\x48\x81\xEC\xD0\x00\x00\x00"
        "\x48\xC7\x44\x24\x28\xFE\xFF\xFF\xFF"
        "\x48\x89\x9C\x24\x00\x01\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00"
        "\x48\x33\xC4"
        "\x48\x89\x84\x24\xC0\x00\x00\x00"
        "\x48\x8B\xF2\x48\x8B\xD9\x33\xFF"
        "\x89\x7C\x24\x24\x48\x8B\x01\xFF\x50\x58"
        "\x48\x63\xE8\x41\x83\xC9\xFF"
        "\x44\x8B\xC5\x48\x8B\xD6\x48\x8D\x4C\x24\x50";

    constexpr const char* SERIALIZE_MASK =
        "xxxx"
        "xxxxxxx"
        "xxxxxxxxx"
        "xxxxxxxx"
        "???????"
        "xxx"
        "xxxxxxxx"
        "xxxxxxxx"
        "xxxxxxxxxx"
        "xxxxxxx"
        "xxxxxxxxxx";

    // ParseFromArray - catches ALL incoming protobuf messages
    // 20-byte pattern, no wildcards
    constexpr const char* PARSE_PATTERN =
        "\x49\x8B\xC0\x4C\x8B\xCA\x4C\x8B\xC1\x48\x8B\xD0\x49\x8B\xC9"
        "\xE9\xAC\xFE\xFF\xFF";

    constexpr const char* PARSE_MASK =
        "xxxxxxxxxxxxxxxxxxxx";
}

// ============================================================================
// DS2 SERVER CONNECTION INFO (from ds3os)
// ============================================================================
constexpr const wchar_t* DS2_SERVER_HOSTNAME = L"frpg2-steam64-ope-login.fromsoftware-game.net";
constexpr uint16_t DS2_LOGIN_PORT = 50031;

// ============================================================================
// DS2 NETWORK PROTOCOL MESSAGE OPCODES (from ds3os reverse engineering)
// These are the protobuf message type IDs used in DS2's network protocol.
// ============================================================================
namespace NetOpcodes {
    // Session lifecycle - THE CRITICAL ONES FOR SEAMLESS CO-OP
    constexpr uint16_t NotifyJoinSession         = 0x03EA;
    constexpr uint16_t NotifyLeaveSession         = 0x03EB;
    constexpr uint16_t NotifyDisconnectSession    = 0x03F9;
    constexpr uint16_t NotifyJoinGuestPlayer       = 0x03E8;
    constexpr uint16_t NotifyLeaveGuestPlayer      = 0x03E9;

    // Death/Kill
    constexpr uint16_t NotifyDeath                = 0x03F1;
    constexpr uint16_t NotifyKillPlayer            = 0x03ED;
    constexpr uint16_t NotifyKillEnemy             = 0x03F6;

    // Summon signs
    constexpr uint16_t CreateSign                 = 0x0394;
    constexpr uint16_t GetSignList                = 0x0397;
    constexpr uint16_t SummonSign                 = 0x0398;
    constexpr uint16_t PushSummonSign             = 0x039B;
    constexpr uint16_t PushRejectSign             = 0x039C;
    constexpr uint16_t PushRemoveSign             = 0x039D;

    // Invasions
    constexpr uint16_t BreakInTarget              = 0x03D3;
    constexpr uint16_t PushBreakInTarget          = 0x03FB;

    // Player data
    constexpr uint16_t WaitForUserLogin           = 0x0386;
    constexpr uint16_t UpdatePlayerStatus         = 0x03B8;

    // Bell / Covenant
    constexpr uint16_t NotifyRingBell             = 0x03EE;
}

// ============================================================================
// DS2 ONLINE AREA IDs (from ds3os)
// ============================================================================
namespace OnlineAreas {
    constexpr uint32_t ThingsBetwixt             = 0x0098E4A0;
    constexpr uint32_t Majula                    = 0x009932C0;
    constexpr uint32_t ForestOfFallenGiants      = 0x009A1D20;
    constexpr uint32_t HeidesTowerOfFlame        = 0x009D5170;
    constexpr uint32_t NomansWharf               = 0x009B55A0;
    constexpr uint32_t TheLostBastille           = 0x009B0780;
    constexpr uint32_t HuntsmansCopse            = 0x009C18F0;
    constexpr uint32_t HarvestValley             = 0x009B2E90;
    constexpr uint32_t IronKeep                  = 0x009B7CB0;
    constexpr uint32_t ShadedWoods               = 0x009D0350;
    constexpr uint32_t DoorsOfPharros            = 0x009D9F90;
    constexpr uint32_t BrightstoneCove           = 0x009AB960;
    constexpr uint32_t GraveOfSaints             = 0x009DC6A0;
    constexpr uint32_t TheGutter                 = 0x009C6710;
    constexpr uint32_t DrangleicCastle           = 0x01346150;
    constexpr uint32_t ShrineOfAmana             = 0x0132DAB0;
    constexpr uint32_t UndeadCrypt               = 0x0134D680;
    constexpr uint32_t AldiasKeep                = 0x009AE070;
    constexpr uint32_t DragonAerie               = 0x009CB530;
    constexpr uint32_t ShulvaDLC1                = 0x030047B0;
    constexpr uint32_t BrumeTowerDLC2            = 0x03006EC0;
    constexpr uint32_t FrozenEleumLoyceDLC3      = 0x030095D0;
}

// ============================================================================
// GAME DATA OFFSETS (from Bob Edition v4.09.5 Cheat Table - VERIFIED)
// ============================================================================
namespace Offsets {

    namespace GameManager {
        constexpr uint32_t PlayerData = 0x38;

        // Player position (from player entity base)
        constexpr uint32_t PositionX = 0x30;   // float
        constexpr uint32_t PositionY = 0x34;   // float
        constexpr uint32_t PositionZ = 0x38;   // float
        constexpr uint32_t RotationY = 0x3C;   // float — facing angle (radians)

        // Player stats (from PlayerData base)
        // HP / MaxHP verified in Bob Edition v4.09.5 CT — separate sub-object
        // accessed via PlayerData + 0xD8 -> Health/MaxHP, but the CT also
        // exposes HP directly at PlayerData + 0x3C and MaxHP at + 0x40.
        // Using direct offsets confirmed by DS2S-META:
        constexpr uint32_t Level     = 0xD0;
        constexpr uint32_t Health    = 0x3C;   // int32 — current HP (was incorrectly 0x0)
        constexpr uint32_t MaxHealth = 0x40;   // int32 — max HP   (was incorrectly 0x1C)
        constexpr uint32_t Stamina   = 0x44;   // float
        constexpr uint32_t SoulMemory = 0xF4;

        // Character info
        constexpr uint32_t CharacterName = 0x24;
        constexpr uint32_t Covenant = 0x1AD;
        constexpr uint32_t TeamType = 0x3D;
        constexpr uint32_t LastBonfire = 0x16C;
        constexpr uint32_t Hollowing = 0x1AC;

        // Inventory (from PlayerData base)
        constexpr uint32_t InventoryBase = 0x12EC;
        constexpr uint32_t InventorySlotSize = 0x8;

        // Stats block (PlayerData + 0x490)
        constexpr uint32_t Stats = 0x490;
        constexpr uint32_t Vigor = Stats + 0x8;
        constexpr uint32_t Endurance = Stats + 0xA;
        constexpr uint32_t Vitality = Stats + 0xC;
        constexpr uint32_t Strength = Stats + 0x10;
        constexpr uint32_t Dexterity = Stats + 0x12;
        constexpr uint32_t Intelligence = Stats + 0x14;
        constexpr uint32_t Faith = Stats + 0x16;
        constexpr uint32_t Adaptability = Stats + 0x18;
    }

    namespace NetSession {
        constexpr uint32_t SessionPointer = 0x18;
        constexpr uint32_t PlayerPointer = 0x20;
        constexpr uint32_t PhantomData = 0x1E8;
        constexpr uint32_t ConnectionState = 0x8;
        constexpr uint32_t PlayerName = 0x234;
        constexpr uint32_t AllottedTime = 0x17C;
        constexpr uint32_t PhantomType = 0x1F4;
    }
}

// ============================================================================
// Item Give function (from DS2S-META by pseudostripy/Nordgaren)
// ============================================================================
namespace ItemGib {
    // ItemGive function — AOB from DS2S-META DS2REData.cs
    constexpr const char* ITEM_GIVE_PATTERN =
        "\x48\x89\x5C\x24\x18\x56\x57\x41\x56\x48\x83\xEC\x30\x45\x8B\xF1\x41";
    constexpr const char* ITEM_GIVE_MASK =
        "xxxxxxxxxxxxxxxxx";

    // AvailableItemBag pointer chain from BaseA (GameManagerImp):
    //   [BaseA] -> +0xA8 -> +0x10 -> +0x10
    constexpr uint32_t AvailItemBag_Off1 = 0xA8;
    constexpr uint32_t AvailItemBag_Off2 = 0x10;
    constexpr uint32_t AvailItemBag_Off3 = 0x10;
}

// ============================================================================
// Item IDs and struct
// ============================================================================
namespace ItemIDs {
    constexpr uint32_t WhiteSignSoapstone      = 0x03B280B0;
    constexpr uint32_t SmallWhiteSignSoapstone  = 0x03B2A7C0;
}

// Item category types
namespace ItemCategory {
    constexpr int32_t Consumable = 3;
}

} // namespace Addresses
} // namespace DS2Coop
