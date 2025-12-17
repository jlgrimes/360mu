/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XAM (Xbox Auxiliary Methods) HLE Implementation
 * 
 * XAM provides higher-level Xbox services like user profiles,
 * achievements, networking, content management, etc.
 * Many games depend heavily on these services.
 */

#include "../kernel.h"
#include "../../cpu/xenon/cpu.h"
#include "../../memory/memory.h"
#include <cstring>
#include <ctime>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XAM] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[XAM WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

// XAM Error codes
constexpr u32 ERROR_SUCCESS = 0;
constexpr u32 ERROR_NO_MORE_FILES = 0x80070012;
constexpr u32 ERROR_FUNCTION_FAILED = 0x80004005;
constexpr u32 ERROR_NOT_LOGGED_ON = 0x80151001;
constexpr u32 ERROR_NO_SUCH_USER = 0x80151002;

// User index (0-3 for local players)
constexpr u32 XUSER_INDEX_NONE = 0xFFFFFFFF;

// XUID (Xbox User ID) - 64-bit identifier
struct XUID {
    u64 value;
};

// Local user profile data
struct LocalUserProfile {
    u32 index;
    XUID xuid;
    char gamertag[16];
    bool signed_in;
    u32 privileges;
    u64 total_gamerscore;
};

// Global user profiles (up to 4 local users)
static LocalUserProfile g_local_users[4] = {
    {0, {0x0009000000000001ULL}, "Player1", true, 0xFFFFFFFF, 0},
    {1, {0x0009000000000002ULL}, "Player2", false, 0, 0},
    {2, {0x0009000000000003ULL}, "Player3", false, 0, 0},
    {3, {0x0009000000000004ULL}, "Player4", false, 0, 0},
};

//=============================================================================
// User Management
//=============================================================================

static void HLE_XamUserGetXUID(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserGetXUID(DWORD dwUserIndex, PXUID pXuid)
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr xuid_ptr = static_cast<GuestAddr>(args[1]);
    
    if (user_index >= 4 || !g_local_users[user_index].signed_in) {
        *result = ERROR_NOT_LOGGED_ON;
        return;
    }
    
    memory->write_u64(xuid_ptr, g_local_users[user_index].xuid.value);
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserGetSigninState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // XUSER_SIGNIN_STATE XamUserGetSigninState(DWORD dwUserIndex)
    // Returns: 0 = NotSignedIn, 1 = SignedInLocally, 2 = SignedInToLive
    u32 user_index = static_cast<u32>(args[0]);
    
    if (user_index >= 4) {
        *result = 0; // Not signed in
        return;
    }
    
    *result = g_local_users[user_index].signed_in ? 1 : 0;
}

static void HLE_XamUserGetSigninInfo(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserGetSigninInfo(DWORD dwUserIndex, DWORD dwFlags, PXUSER_SIGNIN_INFO pSigninInfo)
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= 4 || !g_local_users[user_index].signed_in) {
        *result = ERROR_NOT_LOGGED_ON;
        return;
    }
    
    const auto& user = g_local_users[user_index];
    
    // XUSER_SIGNIN_INFO structure
    memory->write_u64(info_ptr, user.xuid.value);      // xuid
    memory->write_u32(info_ptr + 8, 0);                 // dwFlags
    memory->write_u32(info_ptr + 12, 1);                // SigninState
    memory->write_u32(info_ptr + 16, 0);                // GuestNumber
    memory->write_u32(info_ptr + 20, 0);                // SponsorUserIndex
    
    // Write gamertag
    for (int i = 0; i < 16; i++) {
        memory->write_u8(info_ptr + 24 + i, user.gamertag[i]);
    }
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserGetName(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XamUserGetName(DWORD dwUserIndex, LPSTR pUserName, DWORD cchUserName)
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr name_ptr = static_cast<GuestAddr>(args[1]);
    u32 name_size = static_cast<u32>(args[2]);
    
    if (user_index >= 4 || !g_local_users[user_index].signed_in) {
        *result = ERROR_NOT_LOGGED_ON;
        return;
    }
    
    const auto& user = g_local_users[user_index];
    u32 copy_len = std::min(name_size - 1, static_cast<u32>(strlen(user.gamertag)));
    
    for (u32 i = 0; i < copy_len; i++) {
        memory->write_u8(name_ptr + i, user.gamertag[i]);
    }
    memory->write_u8(name_ptr + copy_len, 0);
    
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Achievements
//=============================================================================

static void HLE_XamUserWriteAchievements(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Write achievements - just succeed silently
    LOGI("XamUserWriteAchievements called");
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserCreateAchievementEnumerator(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Create achievement enumerator - return empty
    *result = ERROR_SUCCESS;
}

static void HLE_XamUserReadAchievementPicture(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Content Management
//=============================================================================

static void HLE_XamContentCreate(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Create content package
    LOGD("XamContentCreate called");
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentClose(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentGetLicenseMask(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return full license
    GuestAddr mask_ptr = static_cast<GuestAddr>(args[1]);
    memory->write_u32(mask_ptr, 0xFFFFFFFF);
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentCreateEnumerator(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Content enumeration
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Networking (stubs for offline play)
//=============================================================================

static void HLE_XNetStartup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("XNetStartup - network disabled");
    *result = ERROR_SUCCESS;
}

static void HLE_XNetCleanup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetGetEthernetLinkStatus(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return disconnected
    *result = 0;
}

static void HLE_XNetQosListen(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetQosLookup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XNetCreateKey(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

static void HLE_XOnlineStartup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("XOnlineStartup - online disabled");
    *result = ERROR_SUCCESS;
}

static void HLE_XOnlineCleanup(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Input
//=============================================================================

static void HLE_XamInputGetCapabilities(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr caps_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= 4) {
        *result = ERROR_FUNCTION_FAILED;
        return;
    }
    
    // XINPUT_CAPABILITIES structure
    memory->write_u8(caps_ptr, 1);      // Type = GAMEPAD
    memory->write_u8(caps_ptr + 1, 1);  // SubType = GAMEPAD
    memory->write_u16(caps_ptr + 2, 0); // Flags
    
    // Gamepad capabilities
    memory->write_u16(caps_ptr + 4, 0xFFFF);  // Buttons - all supported
    memory->write_u8(caps_ptr + 6, 255);       // bLeftTrigger
    memory->write_u8(caps_ptr + 7, 255);       // bRightTrigger
    memory->write_u16(caps_ptr + 8, 32767);    // sThumbLX
    memory->write_u16(caps_ptr + 10, 32767);   // sThumbLY
    memory->write_u16(caps_ptr + 12, 32767);   // sThumbRX
    memory->write_u16(caps_ptr + 14, 32767);   // sThumbRY
    
    // Vibration
    memory->write_u16(caps_ptr + 16, 65535);   // wLeftMotorSpeed
    memory->write_u16(caps_ptr + 18, 65535);   // wRightMotorSpeed
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamInputGetState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 user_index = static_cast<u32>(args[0]);
    GuestAddr state_ptr = static_cast<GuestAddr>(args[2]);
    
    if (user_index >= 4) {
        *result = ERROR_FUNCTION_FAILED;
        return;
    }
    
    // Return idle state - actual input comes from Android touch controls
    // XINPUT_STATE structure
    memory->write_u32(state_ptr, 0);      // dwPacketNumber
    memory->write_u16(state_ptr + 4, 0);  // wButtons
    memory->write_u8(state_ptr + 6, 0);   // bLeftTrigger
    memory->write_u8(state_ptr + 7, 0);   // bRightTrigger
    memory->write_u16(state_ptr + 8, 0);  // sThumbLX
    memory->write_u16(state_ptr + 10, 0); // sThumbLY
    memory->write_u16(state_ptr + 12, 0); // sThumbRX
    memory->write_u16(state_ptr + 14, 0); // sThumbRY
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamInputSetState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Set vibration - could forward to Android device vibration
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Notification / UI
//=============================================================================

static void HLE_XamShowMessageBoxUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Show message box - would need Android UI integration
    LOGI("XamShowMessageBoxUI called");
    *result = ERROR_SUCCESS;
}

static void HLE_XamShowSigninUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Show sign-in UI
    LOGI("XamShowSigninUI called");
    *result = ERROR_SUCCESS;
}

static void HLE_XNotifyQueueUI(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Queue notification
    LOGD("XNotifyQueueUI called");
    *result = ERROR_SUCCESS;
}

static void HLE_XamNotifyCreateListener(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Create notification listener handle
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[1]);
    memory->write_u32(handle_ptr, 0x1000);  // Fake handle
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Storage
//=============================================================================

static void HLE_XamContentGetDeviceName(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr name_ptr = static_cast<GuestAddr>(args[1]);
    u32 name_size = static_cast<u32>(args[2]);
    
    const char* device_name = "Hard Drive";
    u32 copy_len = std::min(name_size - 1, static_cast<u32>(strlen(device_name)));
    
    for (u32 i = 0; i < copy_len; i++) {
        memory->write_u8(name_ptr + i, device_name[i]);
    }
    memory->write_u8(name_ptr + copy_len, 0);
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamContentGetDeviceState(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return device ready
    GuestAddr state_ptr = static_cast<GuestAddr>(args[1]);
    memory->write_u32(state_ptr, 1);  // Ready
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Miscellaneous
//=============================================================================

static void HLE_XamGetExecutionId(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr exec_id_ptr = static_cast<GuestAddr>(args[0]);
    
    // Return a valid execution ID structure
    // This is used for save game identification
    memory->write_u32(exec_id_ptr, 4);                    // Size
    memory->write_u32(exec_id_ptr + 4, 2);                // Version
    memory->write_u32(exec_id_ptr + 8, 0x41560001);       // TitleID
    memory->write_u32(exec_id_ptr + 12, 0);               // Platform
    memory->write_u32(exec_id_ptr + 16, 0x41560001);      // ExecutableVersion
    memory->write_u32(exec_id_ptr + 20, 0);               // BaseVersion
    memory->write_u32(exec_id_ptr + 24, 0x3B3A);          // TitleVersion
    memory->write_u8(exec_id_ptr + 28, 0);                // DiscNum
    memory->write_u8(exec_id_ptr + 29, 1);                // DiscsInSet
    memory->write_u8(exec_id_ptr + 30, 0);                // SaveGameID
    memory->write_u8(exec_id_ptr + 31, 0);                // Reserved
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamLoaderGetMediaInfo(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr info_ptr = static_cast<GuestAddr>(args[0]);
    
    // XMEDIA_INFO structure
    memory->write_u32(info_ptr, 1);       // MediaType (1 = DVD)
    memory->write_u32(info_ptr + 4, 0);   // Flags
    memory->write_u32(info_ptr + 8, 0);   // Reserved
    
    *result = ERROR_SUCCESS;
}

static void HLE_XamEnumerate(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Generic enumeration - return no more items
    *result = ERROR_NO_MORE_FILES;
}

static void HLE_XamAlloc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // XAM memory allocation
    u32 flags = static_cast<u32>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    GuestAddr ptr_ptr = static_cast<GuestAddr>(args[2]);
    
    // Use simple heap allocation
    static GuestAddr heap_ptr = 0x30000000;
    
    GuestAddr alloc_addr = heap_ptr;
    heap_ptr += align_up(size, 16u);
    
    memory->write_u32(ptr_ptr, alloc_addr);
    *result = ERROR_SUCCESS;
}

static void HLE_XamFree(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Free memory - no-op for now
    *result = ERROR_SUCCESS;
}

static void HLE_XamGetSystemVersion(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return system version
    GuestAddr version_ptr = static_cast<GuestAddr>(args[0]);
    
    memory->write_u16(version_ptr, 2);      // Major
    memory->write_u16(version_ptr + 2, 0);  // Minor
    memory->write_u16(version_ptr + 4, 17559); // Build
    memory->write_u16(version_ptr + 6, 0);  // QFE
    
    *result = ERROR_SUCCESS;
}

//=============================================================================
// Registration
//=============================================================================

void Kernel::register_xam() {
    // User management
    hle_functions_[make_import_key(1, 1)] = HLE_XamUserGetXUID;
    hle_functions_[make_import_key(1, 2)] = HLE_XamUserGetSigninState;
    hle_functions_[make_import_key(1, 3)] = HLE_XamUserGetSigninInfo;
    hle_functions_[make_import_key(1, 4)] = HLE_XamUserGetName;
    
    // Achievements
    hle_functions_[make_import_key(1, 5)] = HLE_XamUserWriteAchievements;
    hle_functions_[make_import_key(1, 6)] = HLE_XamUserCreateAchievementEnumerator;
    hle_functions_[make_import_key(1, 7)] = HLE_XamUserReadAchievementPicture;
    
    // Content
    hle_functions_[make_import_key(1, 20)] = HLE_XamContentCreate;
    hle_functions_[make_import_key(1, 21)] = HLE_XamContentClose;
    hle_functions_[make_import_key(1, 22)] = HLE_XamContentGetLicenseMask;
    hle_functions_[make_import_key(1, 23)] = HLE_XamContentCreateEnumerator;
    
    // Networking
    hle_functions_[make_import_key(1, 40)] = HLE_XNetStartup;
    hle_functions_[make_import_key(1, 41)] = HLE_XNetCleanup;
    hle_functions_[make_import_key(1, 42)] = HLE_XNetGetEthernetLinkStatus;
    hle_functions_[make_import_key(1, 43)] = HLE_XNetQosListen;
    hle_functions_[make_import_key(1, 44)] = HLE_XNetQosLookup;
    hle_functions_[make_import_key(1, 45)] = HLE_XNetCreateKey;
    hle_functions_[make_import_key(1, 50)] = HLE_XOnlineStartup;
    hle_functions_[make_import_key(1, 51)] = HLE_XOnlineCleanup;
    
    // Input
    hle_functions_[make_import_key(1, 60)] = HLE_XamInputGetCapabilities;
    hle_functions_[make_import_key(1, 61)] = HLE_XamInputGetState;
    hle_functions_[make_import_key(1, 62)] = HLE_XamInputSetState;
    
    // UI
    hle_functions_[make_import_key(1, 70)] = HLE_XamShowMessageBoxUI;
    hle_functions_[make_import_key(1, 71)] = HLE_XamShowSigninUI;
    hle_functions_[make_import_key(1, 72)] = HLE_XNotifyQueueUI;
    hle_functions_[make_import_key(1, 73)] = HLE_XamNotifyCreateListener;
    
    // Storage
    hle_functions_[make_import_key(1, 80)] = HLE_XamContentGetDeviceName;
    hle_functions_[make_import_key(1, 81)] = HLE_XamContentGetDeviceState;
    
    // Misc
    hle_functions_[make_import_key(1, 90)] = HLE_XamGetExecutionId;
    hle_functions_[make_import_key(1, 91)] = HLE_XamLoaderGetMediaInfo;
    hle_functions_[make_import_key(1, 100)] = HLE_XamEnumerate;
    hle_functions_[make_import_key(1, 101)] = HLE_XamAlloc;
    hle_functions_[make_import_key(1, 102)] = HLE_XamFree;
    hle_functions_[make_import_key(1, 103)] = HLE_XamGetSystemVersion;
    
    LOGI("Registered XAM HLE functions");
}

} // namespace x360mu

