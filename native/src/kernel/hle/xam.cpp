/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XAM (Xbox Auxiliary Methods) HLE Implementation
 * 
 * XAM provides higher-level Xbox services like:
 * - User profiles and sign-in
 * - Achievements and gamer scores
 * - Networking and matchmaking
 * - Content management (DLC, saves)
 * - Input handling
 * - UI overlays
 * 
 * Many games, including Black Ops, depend heavily on these services.
 */

#include "../kernel.h"
#include "../../cpu/xenon/cpu.h"
#include "../../memory/memory.h"
#include <cstring>
#include <ctime>
#include <chrono>
#include <array>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <random>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XAM] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[XAM WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[XAM ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

//=============================================================================
// XAM Error Codes
//=============================================================================
constexpr u32 ERROR_SUCCESS = 0;
constexpr u32 ERROR_ACCESS_DENIED = 0x00000005;
constexpr u32 ERROR_INVALID_HANDLE = 0x00000006;
constexpr u32 ERROR_INVALID_PARAMETER = 0x00000057;
constexpr u32 ERROR_INSUFFICIENT_BUFFER = 0x0000007A;
constexpr u32 ERROR_NO_MORE_FILES = 0x00000012;
constexpr u32 ERROR_FUNCTION_FAILED = 0x80004005;
constexpr u32 ERROR_NOT_LOGGED_ON = 0x80151001;
constexpr u32 ERROR_NO_SUCH_USER = 0x80151002;
constexpr u32 ERROR_NOT_FOUND = 0x80070002;
constexpr u32 ERROR_CANCELLED = 0x800704C7;
constexpr u32 ERROR_IO_PENDING = 0x800703E5;

// XUSER constants
constexpr u32 XUSER_INDEX_NONE = 0xFFFFFFFF;
constexpr u32 XUSER_MAX_COUNT = 4;

// Sign-in states
constexpr u32 eXUserSigninState_NotSignedIn = 0;
constexpr u32 eXUserSigninState_SignedInLocally = 1;
constexpr u32 eXUserSigninState_SignedInToLive = 2;

// Content types
constexpr u32 XCONTENTTYPE_SAVEDGAME = 0x00000001;
constexpr u32 XCONTENTTYPE_MARKETPLACE = 0x00000002;
constexpr u32 XCONTENTTYPE_PUBLISHER = 0x00000003;
constexpr u32 XCONTENTTYPE_THEMATICSKIN = 0x00030000;

//=============================================================================
// XAM Global State
//=============================================================================
struct XamState {
    // User profiles (up to 4 local users)
    struct UserProfile {
        u32 index;
        u64 xuid;
        char gamertag[16];
        bool signed_in;
        u32 signin_state;
        u32 privileges;
        u64 gamerscore;
        std::vector<u32> unlocked_achievements;
    };
    std::array<UserProfile, 4> users;
    
    // Input state per player
    struct InputState {
        u32 packet_number;
        u16 buttons;
        u8 left_trigger;
        u8 right_trigger;
        s16 left_stick_x;
        s16 left_stick_y;
        s16 right_stick_x;
        s16 right_stick_y;
    };
    std::array<InputState, 4> input_states;
    std::mutex input_mutex;
    
    // Content handles
    struct ContentHandle {
        u32 handle;
        u32 content_type;
        u64 xuid;
        std::string root_path;
        bool is_open;
    };
    std::unordered_map<u32, ContentHandle> content_handles;
    u32 next_content_handle = 0x1000;
    std::mutex content_mutex;
    
    // Enumerator handles
    struct EnumeratorHandle {
        u32 handle;
        u32 type;
        u32 current_index;
        std::vector<u32> items;
    };
    std::unordered_map<u32, EnumeratorHandle> enumerators;
    u32 next_enum_handle = 0x2000;
    
    // Notification listeners
    struct NotificationListener {
        u32 handle;
        u64 notification_mask;
    };
    std::vector<NotificationListener> notification_listeners;
    u32 next_listener_handle = 0x3000;
    
    // Overlapped operations
    struct AsyncOperation {
        u32 overlapped_ptr;
        bool completed;
        u32 result;
        u32 bytes_transferred;
    };
    std::unordered_map<GuestAddr, AsyncOperation> async_ops;
    
    // Title-specific data
    u32 title_id = 0x41560855;  // Black Ops title ID
    u32 title_version = 0;
    
    void init() {
        // Initialize default user (Player 1 signed in locally)
        users[0] = {
            .index = 0,
            .xuid = 0x0009000000000001ULL,
            .gamertag = "Player1",
            .signed_in = true,
            .signin_state = eXUserSigninState_SignedInLocally,
            .privileges = 0xFFFFFFFF,
            .gamerscore = 0
        };
        strcpy(users[0].gamertag, "Player1");
        
        // Other users not signed in
        for (u32 i = 1; i < 4; i++) {
            users[i] = {
                .index = i,
                .xuid = 0x0009000000000000ULL + i + 1,
                .gamertag = "",
                .signed_in = false,
                .signin_state = eXUserSigninState_NotSignedIn,
                .privileges = 0,
                .gamerscore = 0
            };
            snprintf(users[i].gamertag, 16, "Player%u", i + 1);
        }
        
        // Initialize input states
        for (auto& input : input_states) {
            input = {};
        }
    }
};

static XamState g_xam;

//=============================================================================
// User Management Functions
//=============================================================================

static void HLE_XamUserGetXUID(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserGetXUID(DWORD dwUserIndex, PXUID pXuid)
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr xuid_ptr = static_cast<GuestAddr>(args[1]);
    
    if (user_index >= XUSER_MAX_COUNT) {
        *result = ERROR_NO_SUCH_USER;
        return;
    }
    
    const auto& user = g_xam.users[user_index];
    if (!user.signed_in) {
        *result = ERROR_NOT_LOGGED_ON;
        return;
    }
    
    memory->write_u64(xuid_ptr, user.xuid);
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserGetSigninState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // XUSER_SIGNIN_STATE XamUserGetSigninState(DWORD dwUserIndex)
    u32 user_index = static_cast<u32>(args[0]);
    
    if (user_index >= XUSER_MAX_COUNT) {
        *result = eXUserSigninState_NotSignedIn;
        return;
    }
    
    *result = g_xam.users[user_index].signin_state;
}

static void HLE_XamUserGetSigninInfo(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserGetSigninInfo(DWORD dwUserIndex, DWORD dwFlags, PXUSER_SIGNIN_INFO pSigninInfo)
    u32 user_index = static_cast<u32>(args[0]);
    u32 flags = static_cast<u32>(args[1]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= XUSER_MAX_COUNT) {
        *result = ERROR_NO_SUCH_USER;
        return;
    }
    
    const auto& user = g_xam.users[user_index];
    if (!user.signed_in) {
        *result = ERROR_NOT_LOGGED_ON;
        return;
    }
    
    // XUSER_SIGNIN_INFO structure
    memory->write_u64(info_ptr + 0, user.xuid);
    memory->write_u32(info_ptr + 8, flags);
    memory->write_u32(info_ptr + 12, user.signin_state);
    memory->write_u32(info_ptr + 16, 0);  // GuestNumber
    memory->write_u32(info_ptr + 20, XUSER_INDEX_NONE);  // SponsorUserIndex
    
    // Write gamertag (16 bytes)
    for (int i = 0; i < 16; i++) {
        memory->write_u8(info_ptr + 24 + i, static_cast<u8>(user.gamertag[i]));
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserGetName(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserGetName(DWORD dwUserIndex, LPSTR pUserName, DWORD cchUserName)
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr name_ptr = static_cast<GuestAddr>(args[1]);
    u32 name_size = static_cast<u32>(args[2]);
    
    if (user_index >= XUSER_MAX_COUNT) {
        *result = ERROR_NO_SUCH_USER;
        return;
    }
    
    const auto& user = g_xam.users[user_index];
    if (!user.signed_in) {
        *result = ERROR_NOT_LOGGED_ON;
        return;
    }
    
    u32 tag_len = static_cast<u32>(strlen(user.gamertag));
    u32 copy_len = std::min(name_size - 1, tag_len);
    
    for (u32 i = 0; i < copy_len; i++) {
        memory->write_u8(name_ptr + i, static_cast<u8>(user.gamertag[i]));
    }
    memory->write_u8(name_ptr + copy_len, 0);
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserCheckPrivilege(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserCheckPrivilege(DWORD dwUserIndex, DWORD dwPrivilegeType, PBOOL pfResult)
    u32 user_index = static_cast<u32>(args[0]);
    u32 privilege_type = static_cast<u32>(args[1]);
    GuestAddr result_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= XUSER_MAX_COUNT || !g_xam.users[user_index].signed_in) {
        memory->write_u32(result_ptr, 0);  // FALSE
        *result = ERROR_SUCCESS;
        return;
    }
    
    // Check if privilege is granted (all privileges for offline play)
    bool has_privilege = (g_xam.users[user_index].privileges & (1 << privilege_type)) != 0;
    memory->write_u32(result_ptr, has_privilege ? 1 : 0);
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserAreUsersFriends(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // For offline, return not friends
    GuestAddr result_ptr = static_cast<GuestAddr>(args[2]);
    if (result_ptr) {
        memory->write_u32(result_ptr, 0);
    }
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Achievement Functions
//=============================================================================

static void HLE_XamUserWriteAchievements(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    u32 count = static_cast<u32>(args[1]);
    GuestAddr achievements_ptr = static_cast<GuestAddr>(args[2]);
    
    LOGI("XamUserWriteAchievements: user=%u, count=%u", user_index, count);
    
    // Track achievements locally
    if (user_index < XUSER_MAX_COUNT) {
        for (u32 i = 0; i < count; i++) {
            u32 achievement_id = memory->read_u32(achievements_ptr + i * 8);
            g_xam.users[user_index].unlocked_achievements.push_back(achievement_id);
            LOGI("Achievement unlocked: %u", achievement_id);
        }
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserCreateAchievementEnumerator(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Create enumerator for achievements
    u32 title_id = static_cast<u32>(args[0]);
    u32 user_index = static_cast<u32>(args[1]);
    u64 xuid = args[2];
    u32 flags = static_cast<u32>(args[3]);
    u32 starting_index = static_cast<u32>(args[4]);
    u32 count = static_cast<u32>(args[5]);
    GuestAddr buffer_size_ptr = static_cast<GuestAddr>(args[6]);
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[7]);
    
    // Return buffer size required
    if (buffer_size_ptr) {
        memory->write_u32(buffer_size_ptr, count * 0x24);  // Approximate size
    }
    
    // Create enumerator handle
    u32 handle = g_xam.next_enum_handle++;
    if (handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    g_xam.enumerators[handle] = {
        .handle = handle,
        .type = 1,  // Achievement enumerator
        .current_index = starting_index,
        .items = {}
    };
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserReadAchievementPicture(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Not implemented - would need to provide achievement images
    *result = ERROR_NOT_FOUND;
}

//=============================================================================
// Profile Functions
//=============================================================================

static void HLE_XamUserReadProfileSettings(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Read user profile settings
    u32 title_id = static_cast<u32>(args[0]);
    u32 user_index = static_cast<u32>(args[1]);
    u32 num_settings = static_cast<u32>(args[2]);
    GuestAddr setting_ids = static_cast<GuestAddr>(args[3]);
    GuestAddr buffer_size_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr results = static_cast<GuestAddr>(args[5]);
    GuestAddr overlapped = static_cast<GuestAddr>(args[6]);
    
    u32 required_size = num_settings * 0x28;
    
    if (buffer_size_ptr) {
        u32 buffer_size = memory->read_u32(buffer_size_ptr);
        if (buffer_size < required_size) {
            memory->write_u32(buffer_size_ptr, required_size);
            *result = ERROR_INSUFFICIENT_BUFFER;
            return;
        }
    }
    
    // Zero the results buffer
    if (results) {
        memory->zero_bytes(results, required_size);
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserWriteProfileSettings(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Write profile settings - just succeed
    LOGD("XamUserWriteProfileSettings called");
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Content Management Functions
//=============================================================================

static void HLE_XamContentCreate(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamContentCreate(
    //   DWORD dwUserIndex,
    //   PCSTR szRootName,
    //   PXCONTENT_DATA pContentData,
    //   DWORD dwFlags,
    //   PDWORD pdwDisposition,
    //   PDWORD pdwLicenseMask,
    //   PHANDLE phContent
    // );
    
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr root_name_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr content_data_ptr = static_cast<GuestAddr>(args[2]);
    u32 flags = static_cast<u32>(args[3]);
    GuestAddr disposition_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr license_mask_ptr = static_cast<GuestAddr>(args[5]);
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[6]);
    
    // Read root name
    std::string root_name;
    if (root_name_ptr) {
        char c;
        while ((c = static_cast<char>(memory->read_u8(root_name_ptr + root_name.length()))) != 0) {
            root_name += c;
            if (root_name.length() > 64) break;
        }
    }
    
    // Create content handle
    std::lock_guard<std::mutex> lock(g_xam.content_mutex);
    
    u32 handle = g_xam.next_content_handle++;
    g_xam.content_handles[handle] = {
        .handle = handle,
        .content_type = XCONTENTTYPE_SAVEDGAME,
        .xuid = user_index < XUSER_MAX_COUNT ? g_xam.users[user_index].xuid : 0,
        .root_path = "save/" + root_name,
        .is_open = true
    };
    
    if (disposition_ptr) {
        memory->write_u32(disposition_ptr, 1);  // XCONTENTCREATED_NEW
    }
    
    if (license_mask_ptr) {
        memory->write_u32(license_mask_ptr, 0xFFFFFFFF);  // Full license
    }
    
    if (handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    LOGD("XamContentCreate: root='%s', handle=0x%X", root_name.c_str(), handle);
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentClose(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr root_name_ptr = static_cast<GuestAddr>(args[0]);
    u32 handle = static_cast<u32>(args[1]);
    
    std::lock_guard<std::mutex> lock(g_xam.content_mutex);
    
    auto it = g_xam.content_handles.find(handle);
    if (it != g_xam.content_handles.end()) {
        it->second.is_open = false;
        g_xam.content_handles.erase(it);
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentGetLicenseMask(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr mask_ptr = static_cast<GuestAddr>(args[1]);
    
    // Return full license
    if (mask_ptr) {
        memory->write_u32(mask_ptr, 0xFFFFFFFF);
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentCreateEnumerator(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    u32 device_id = static_cast<u32>(args[1]);
    u32 content_type = static_cast<u32>(args[2]);
    u32 flags = static_cast<u32>(args[3]);
    u32 max_items = static_cast<u32>(args[4]);
    GuestAddr buffer_size_ptr = static_cast<GuestAddr>(args[5]);
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[6]);
    
    if (buffer_size_ptr) {
        memory->write_u32(buffer_size_ptr, max_items * 0x360);  // XCONTENT_DATA size
    }
    
    u32 handle = g_xam.next_enum_handle++;
    if (handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    g_xam.enumerators[handle] = {
        .handle = handle,
        .type = 2,  // Content enumerator
        .current_index = 0,
        .items = {}
    };
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentGetCreator(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return current user as content creator
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr result_ptr = static_cast<GuestAddr>(args[3]);
    
    if (result_ptr && user_index < XUSER_MAX_COUNT) {
        memory->write_u64(result_ptr, g_xam.users[user_index].xuid);
    }
    
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Networking Functions (Stubs for Offline Play)
//=============================================================================

static void HLE_XNetStartup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("XNetStartup - network disabled for offline play");
    *result = ERROR_SUCCESS;
}

static void HLE_XNetCleanup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetRandom(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr buffer = static_cast<GuestAddr>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    
    // Fill with random bytes
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<u32> dist(0, 255);
    
    for (u32 i = 0; i < size; i++) {
        memory->write_u8(buffer + i, static_cast<u8>(dist(gen)));
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XNetGetEthernetLinkStatus(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return disconnected
    *result = 0;
}

static void HLE_XNetGetTitleXnAddr(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr xnaddr_ptr = static_cast<GuestAddr>(args[0]);
    
    // Fill with zeroes (no network)
    if (xnaddr_ptr) {
        memory->zero_bytes(xnaddr_ptr, 0x24);  // XNADDR size
    }
    
    *result = 0;  // XNET_GET_XNADDR_PENDING
}

static void HLE_XNetQosListen(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetQosLookup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetQosServiceLookup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetQosRelease(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetCreateKey(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr xnkid_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr xnkey_ptr = static_cast<GuestAddr>(args[1]);
    
    // Generate random session key
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<u32> dist(0, 255);
    
    for (u32 i = 0; i < 8; i++) {  // XNKID is 8 bytes
        memory->write_u8(xnkid_ptr + i, static_cast<u8>(dist(gen)));
    }
    for (u32 i = 0; i < 16; i++) {  // XNKEY is 16 bytes
        memory->write_u8(xnkey_ptr + i, static_cast<u8>(dist(gen)));
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XNetRegisterKey(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetUnregisterKey(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XOnlineStartup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("XOnlineStartup - online features disabled");
    *result = ERROR_SUCCESS;
}

static void HLE_XOnlineCleanup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Input Functions
//=============================================================================

static void HLE_XamInputGetCapabilities(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    u32 flags = static_cast<u32>(args[1]);
    GuestAddr caps_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= XUSER_MAX_COUNT) {
        *result = ERROR_FUNCTION_FAILED;
        return;
    }
    
    // XINPUT_CAPABILITIES structure
    memory->write_u8(caps_ptr + 0, 1);      // Type = GAMEPAD
    memory->write_u8(caps_ptr + 1, 1);      // SubType = GAMEPAD
    memory->write_u16(caps_ptr + 2, 0);     // Flags
    
    // Gamepad section - all buttons supported
    memory->write_u16(caps_ptr + 4, 0xF3FF);  // wButtons
    memory->write_u8(caps_ptr + 6, 255);       // bLeftTrigger
    memory->write_u8(caps_ptr + 7, 255);       // bRightTrigger
    memory->write_u16(caps_ptr + 8, 32767);    // sThumbLX max
    memory->write_u16(caps_ptr + 10, 32767);   // sThumbLY max
    memory->write_u16(caps_ptr + 12, 32767);   // sThumbRX max
    memory->write_u16(caps_ptr + 14, 32767);   // sThumbRY max
    
    // Vibration section
    memory->write_u16(caps_ptr + 16, 65535);   // wLeftMotorSpeed max
    memory->write_u16(caps_ptr + 18, 65535);   // wRightMotorSpeed max
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamInputGetState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    u32 flags = static_cast<u32>(args[1]);
    GuestAddr state_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= XUSER_MAX_COUNT) {
        *result = ERROR_FUNCTION_FAILED;
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_xam.input_mutex);
    const auto& input = g_xam.input_states[user_index];
    
    // XINPUT_STATE structure
    memory->write_u32(state_ptr + 0, input.packet_number);
    
    // XINPUT_GAMEPAD
    memory->write_u16(state_ptr + 4, input.buttons);
    memory->write_u8(state_ptr + 6, input.left_trigger);
    memory->write_u8(state_ptr + 7, input.right_trigger);
    memory->write_u16(state_ptr + 8, static_cast<u16>(input.left_stick_x));
    memory->write_u16(state_ptr + 10, static_cast<u16>(input.left_stick_y));
    memory->write_u16(state_ptr + 12, static_cast<u16>(input.right_stick_x));
    memory->write_u16(state_ptr + 14, static_cast<u16>(input.right_stick_y));
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamInputSetState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    u32 flags = static_cast<u32>(args[1]);
    GuestAddr vibration_ptr = static_cast<GuestAddr>(args[2]);
    
    // Would forward vibration to Android haptics
    // XINPUT_VIBRATION: u16 wLeftMotorSpeed, u16 wRightMotorSpeed
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamInputGetKeystroke(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    u32 flags = static_cast<u32>(args[1]);
    GuestAddr keystroke_ptr = static_cast<GuestAddr>(args[2]);
    
    // No keystroke available
    *result = ERROR_FUNCTION_FAILED;
}

// Function to update input state from Android
void xam_set_input_state(u32 user_index, u16 buttons, u8 lt, u8 rt,
                         s16 lx, s16 ly, s16 rx, s16 ry) {
    if (user_index >= XUSER_MAX_COUNT) return;
    
    std::lock_guard<std::mutex> lock(g_xam.input_mutex);
    auto& input = g_xam.input_states[user_index];
    
    input.packet_number++;
    input.buttons = buttons;
    input.left_trigger = lt;
    input.right_trigger = rt;
    input.left_stick_x = lx;
    input.left_stick_y = ly;
    input.right_stick_x = rx;
    input.right_stick_y = ry;
}

//=============================================================================
// UI / Notification Functions
//=============================================================================

static void HLE_XamShowMessageBoxUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr title_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr text_ptr = static_cast<GuestAddr>(args[2]);
    u32 button_count = static_cast<u32>(args[3]);
    GuestAddr buttons_ptr = static_cast<GuestAddr>(args[4]);
    u32 focus_button = static_cast<u32>(args[5]);
    u32 flags = static_cast<u32>(args[6]);
    GuestAddr result_ptr = static_cast<GuestAddr>(args[7]);
    GuestAddr overlapped = static_cast<GuestAddr>(args[8]);
    
    LOGI("XamShowMessageBoxUI called");
    
    // Auto-select first button
    if (result_ptr) {
        memory->write_u32(result_ptr, 0);
    }
    
    // Complete immediately
    if (overlapped) {
        memory->write_u32(overlapped + 0, ERROR_SUCCESS);
        memory->write_u32(overlapped + 4, 0);  // Extended error
        memory->write_u32(overlapped + 8, 0);  // Bytes transferred
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamShowSigninUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 num_panes = static_cast<u32>(args[0]);
    u32 flags = static_cast<u32>(args[1]);
    
    LOGI("XamShowSigninUI: panes=%u, flags=0x%X", num_panes, flags);
    
    // Player already signed in
    *result = ERROR_SUCCESS;
}

static void HLE_XamShowKeyboardUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Would need to show Android keyboard
    LOGI("XamShowKeyboardUI called");
    *result = ERROR_CANCELLED;
}

static void HLE_XamShowGamerCardUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGD("XamShowGamerCardUI called");
    *result = ERROR_SUCCESS;
}

static void HLE_XNotifyQueueUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 type = static_cast<u32>(args[0]);
    u32 user_index = static_cast<u32>(args[1]);
    u64 area = args[2];
    GuestAddr string_ptr = static_cast<GuestAddr>(args[3]);
    
    LOGD("XNotifyQueueUI: type=%u, user=%u", type, user_index);
    *result = ERROR_SUCCESS;
}

static void HLE_XNotifyCreateListener(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u64 notification_mask = args[0];
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[1]);
    
    u32 handle = g_xam.next_listener_handle++;
    g_xam.notification_listeners.push_back({handle, notification_mask});
    
    if (handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XNotifyGetNext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    u32 match_id = static_cast<u32>(args[1]);
    GuestAddr id_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr param_ptr = static_cast<GuestAddr>(args[3]);
    
    // No notifications pending
    *result = 0;  // FALSE
}

static void HLE_XNotifyDestroyListener(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    
    // Remove listener
    auto it = std::find_if(g_xam.notification_listeners.begin(),
                          g_xam.notification_listeners.end(),
                          [handle](const auto& l) { return l.handle == handle; });
    if (it != g_xam.notification_listeners.end()) {
        g_xam.notification_listeners.erase(it);
    }
    
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Storage Functions
//=============================================================================

static void HLE_XamContentGetDeviceName(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 device_id = static_cast<u32>(args[0]);
    GuestAddr name_ptr = static_cast<GuestAddr>(args[1]);
    u32 name_size = static_cast<u32>(args[2]);
    
    const char* device_name = "Hard Drive";
    u32 copy_len = std::min(name_size - 1, static_cast<u32>(strlen(device_name)));
    
    for (u32 i = 0; i < copy_len; i++) {
        memory->write_u8(name_ptr + i, static_cast<u8>(device_name[i]));
    }
    memory->write_u8(name_ptr + copy_len, 0);
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentGetDeviceState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 device_id = static_cast<u32>(args[0]);
    GuestAddr state_ptr = static_cast<GuestAddr>(args[1]);
    
    // Return device ready
    if (state_ptr) {
        memory->write_u32(state_ptr, 1);  // Ready
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentGetDeviceData(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 device_id = static_cast<u32>(args[0]);
    GuestAddr data_ptr = static_cast<GuestAddr>(args[1]);
    
    // XDEVICE_DATA structure
    if (data_ptr) {
        memory->write_u32(data_ptr + 0, device_id);
        memory->write_u64(data_ptr + 4, 4ULL * 1024 * 1024 * 1024);  // 4GB total
        memory->write_u64(data_ptr + 12, 2ULL * 1024 * 1024 * 1024); // 2GB free
    }
    
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Miscellaneous Functions
//=============================================================================

static void HLE_XamGetExecutionId(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr exec_id_ptr = static_cast<GuestAddr>(args[0]);
    
    // EXECUTION_ID structure
    memory->write_u32(exec_id_ptr + 0, 4);                 // Size
    memory->write_u32(exec_id_ptr + 4, 2);                 // Version
    memory->write_u32(exec_id_ptr + 8, g_xam.title_id);    // TitleID
    memory->write_u32(exec_id_ptr + 12, 0);                // Platform
    memory->write_u32(exec_id_ptr + 16, g_xam.title_id);   // ExecutableVersion
    memory->write_u32(exec_id_ptr + 20, 0);                // BaseVersion
    memory->write_u16(exec_id_ptr + 24, g_xam.title_version); // TitleVersion
    memory->write_u8(exec_id_ptr + 26, 0);                 // DiscNum
    memory->write_u8(exec_id_ptr + 27, 1);                 // DiscsInSet
    memory->write_u32(exec_id_ptr + 28, 0);                // SaveGameID
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamLoaderGetMediaInfo(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr info_ptr = static_cast<GuestAddr>(args[0]);
    
    // XMEDIA_INFO structure
    if (info_ptr) {
        memory->write_u32(info_ptr + 0, 1);   // MediaType (1 = DVD)
        memory->write_u32(info_ptr + 4, 0);   // Flags
        memory->write_u32(info_ptr + 8, 0);   // Reserved
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamEnumerate(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr buffer = static_cast<GuestAddr>(args[1]);
    u32 buffer_size = static_cast<u32>(args[2]);
    GuestAddr items_ptr = static_cast<GuestAddr>(args[3]);
    GuestAddr overlapped = static_cast<GuestAddr>(args[4]);
    
    auto it = g_xam.enumerators.find(handle);
    if (it == g_xam.enumerators.end()) {
        *result = ERROR_INVALID_HANDLE;
        return;
    }
    
    // Return no more items
    if (items_ptr) {
        memory->write_u32(items_ptr, 0);
    }
    
    if (overlapped) {
        memory->write_u32(overlapped + 0, ERROR_NO_MORE_FILES);
    }
    
    *result = ERROR_NO_MORE_FILES;
}

static void HLE_XamAlloc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 flags = static_cast<u32>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    GuestAddr ptr_ptr = static_cast<GuestAddr>(args[2]);
    
    // Use heap allocator
    static GuestAddr heap_ptr = 0x30000000;
    
    size = align_up(size, 16u);
    GuestAddr alloc_addr = heap_ptr;
    heap_ptr += size;
    
    memory->allocate(alloc_addr, size, MemoryRegion::Read | MemoryRegion::Write);
    memory->write_u32(ptr_ptr, alloc_addr);
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamFree(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // No-op for now (no deallocation)
    *result = ERROR_SUCCESS;
}

static void HLE_XamGetSystemVersion(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr version_ptr = static_cast<GuestAddr>(args[0]);
    
    // Return system version
    memory->write_u16(version_ptr + 0, 2);     // Major
    memory->write_u16(version_ptr + 2, 0);     // Minor
    memory->write_u16(version_ptr + 4, 17559); // Build
    memory->write_u16(version_ptr + 6, 0);     // QFE
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamGetCurrentTitleId(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = g_xam.title_id;
}

static void HLE_XamIsSystemTitleId(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 title_id = static_cast<u32>(args[0]);
    
    // Check if title ID is a system title (starts with 0xFFFE)
    *result = ((title_id >> 16) == 0xFFFE) ? 1 : 0;
}

static void HLE_XamLoaderSetLaunchData(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Store launch data for next title
    *result = ERROR_SUCCESS;
}

static void HLE_XamLoaderGetLaunchData(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // No launch data available
    *result = ERROR_NOT_FOUND;
}

static void HLE_XamLoaderTerminateTitle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("XamLoaderTerminateTitle called");
    *result = ERROR_SUCCESS;
}

static void HLE_XamLoaderLaunchTitle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("XamLoaderLaunchTitle called");
    *result = ERROR_SUCCESS;
}

static void HLE_XamTaskScheduleTask(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Schedule async task
    *result = ERROR_SUCCESS;
}

static void HLE_XamTaskCloseHandle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Session Functions (Multiplayer stubs)
//=============================================================================

static void HLE_XSessionCreate(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGD("XSessionCreate called - multiplayer disabled");
    *result = ERROR_FUNCTION_FAILED;
}

static void HLE_XSessionDelete(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XSessionStart(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_FUNCTION_FAILED;
}

static void HLE_XSessionEnd(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Leaderboard/Ranking Functions (Stubs)
//=============================================================================

static void HLE_XamUserReadStats(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // No stats to read
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserWriteStats(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Silently succeed
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Registration
//=============================================================================

void Kernel::register_xam() {
    // Initialize XAM state
    g_xam.init();
    
    // User management
    hle_functions_[make_import_key(1, 1)] = HLE_XamUserGetXUID;
    hle_functions_[make_import_key(1, 2)] = HLE_XamUserGetSigninState;
    hle_functions_[make_import_key(1, 3)] = HLE_XamUserGetSigninInfo;
    hle_functions_[make_import_key(1, 4)] = HLE_XamUserGetName;
    hle_functions_[make_import_key(1, 5)] = HLE_XamUserCheckPrivilege;
    hle_functions_[make_import_key(1, 6)] = HLE_XamUserAreUsersFriends;
    
    // Achievements
    hle_functions_[make_import_key(1, 10)] = HLE_XamUserWriteAchievements;
    hle_functions_[make_import_key(1, 11)] = HLE_XamUserCreateAchievementEnumerator;
    hle_functions_[make_import_key(1, 12)] = HLE_XamUserReadAchievementPicture;
    
    // Profile
    hle_functions_[make_import_key(1, 15)] = HLE_XamUserReadProfileSettings;
    hle_functions_[make_import_key(1, 16)] = HLE_XamUserWriteProfileSettings;
    
    // Content
    hle_functions_[make_import_key(1, 20)] = HLE_XamContentCreate;
    hle_functions_[make_import_key(1, 21)] = HLE_XamContentClose;
    hle_functions_[make_import_key(1, 22)] = HLE_XamContentGetLicenseMask;
    hle_functions_[make_import_key(1, 23)] = HLE_XamContentCreateEnumerator;
    hle_functions_[make_import_key(1, 24)] = HLE_XamContentGetCreator;
    
    // Networking
    hle_functions_[make_import_key(1, 40)] = HLE_XNetStartup;
    hle_functions_[make_import_key(1, 41)] = HLE_XNetCleanup;
    hle_functions_[make_import_key(1, 42)] = HLE_XNetRandom;
    hle_functions_[make_import_key(1, 43)] = HLE_XNetGetEthernetLinkStatus;
    hle_functions_[make_import_key(1, 44)] = HLE_XNetGetTitleXnAddr;
    hle_functions_[make_import_key(1, 45)] = HLE_XNetQosListen;
    hle_functions_[make_import_key(1, 46)] = HLE_XNetQosLookup;
    hle_functions_[make_import_key(1, 47)] = HLE_XNetQosServiceLookup;
    hle_functions_[make_import_key(1, 48)] = HLE_XNetQosRelease;
    hle_functions_[make_import_key(1, 49)] = HLE_XNetCreateKey;
    hle_functions_[make_import_key(1, 50)] = HLE_XNetRegisterKey;
    hle_functions_[make_import_key(1, 51)] = HLE_XNetUnregisterKey;
    hle_functions_[make_import_key(1, 55)] = HLE_XOnlineStartup;
    hle_functions_[make_import_key(1, 56)] = HLE_XOnlineCleanup;
    
    // Input
    hle_functions_[make_import_key(1, 60)] = HLE_XamInputGetCapabilities;
    hle_functions_[make_import_key(1, 61)] = HLE_XamInputGetState;
    hle_functions_[make_import_key(1, 62)] = HLE_XamInputSetState;
    hle_functions_[make_import_key(1, 63)] = HLE_XamInputGetKeystroke;
    
    // UI
    hle_functions_[make_import_key(1, 70)] = HLE_XamShowMessageBoxUI;
    hle_functions_[make_import_key(1, 71)] = HLE_XamShowSigninUI;
    hle_functions_[make_import_key(1, 72)] = HLE_XamShowKeyboardUI;
    hle_functions_[make_import_key(1, 73)] = HLE_XamShowGamerCardUI;
    hle_functions_[make_import_key(1, 74)] = HLE_XNotifyQueueUI;
    hle_functions_[make_import_key(1, 75)] = HLE_XNotifyCreateListener;
    hle_functions_[make_import_key(1, 76)] = HLE_XNotifyGetNext;
    hle_functions_[make_import_key(1, 77)] = HLE_XNotifyDestroyListener;
    
    // Storage
    hle_functions_[make_import_key(1, 80)] = HLE_XamContentGetDeviceName;
    hle_functions_[make_import_key(1, 81)] = HLE_XamContentGetDeviceState;
    hle_functions_[make_import_key(1, 82)] = HLE_XamContentGetDeviceData;
    
    // Misc
    hle_functions_[make_import_key(1, 90)] = HLE_XamGetExecutionId;
    hle_functions_[make_import_key(1, 91)] = HLE_XamLoaderGetMediaInfo;
    hle_functions_[make_import_key(1, 100)] = HLE_XamEnumerate;
    hle_functions_[make_import_key(1, 101)] = HLE_XamAlloc;
    hle_functions_[make_import_key(1, 102)] = HLE_XamFree;
    hle_functions_[make_import_key(1, 103)] = HLE_XamGetSystemVersion;
    hle_functions_[make_import_key(1, 104)] = HLE_XamGetCurrentTitleId;
    hle_functions_[make_import_key(1, 105)] = HLE_XamIsSystemTitleId;
    hle_functions_[make_import_key(1, 106)] = HLE_XamLoaderSetLaunchData;
    hle_functions_[make_import_key(1, 107)] = HLE_XamLoaderGetLaunchData;
    hle_functions_[make_import_key(1, 108)] = HLE_XamLoaderTerminateTitle;
    hle_functions_[make_import_key(1, 109)] = HLE_XamLoaderLaunchTitle;
    hle_functions_[make_import_key(1, 110)] = HLE_XamTaskScheduleTask;
    hle_functions_[make_import_key(1, 111)] = HLE_XamTaskCloseHandle;
    
    // Session
    hle_functions_[make_import_key(1, 120)] = HLE_XSessionCreate;
    hle_functions_[make_import_key(1, 121)] = HLE_XSessionDelete;
    hle_functions_[make_import_key(1, 122)] = HLE_XSessionStart;
    hle_functions_[make_import_key(1, 123)] = HLE_XSessionEnd;
    
    // Stats
    hle_functions_[make_import_key(1, 130)] = HLE_XamUserReadStats;
    hle_functions_[make_import_key(1, 131)] = HLE_XamUserWriteStats;
    
    LOGI("Registered XAM HLE functions (%zu total)", hle_functions_.size());
}

// Set title ID for the current game
void xam_set_title_id(u32 title_id) {
    g_xam.title_id = title_id;
}

} // namespace x360mu
