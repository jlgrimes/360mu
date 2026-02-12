/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Game Information and Compatibility Checking
 *
 * Extracts XEX metadata and checks import coverage against
 * registered HLE exports to determine game compatibility.
 */

#pragma once

#include "x360mu/types.h"
#include "xex_loader.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace x360mu {

/**
 * Compatibility status for a game
 */
enum class CompatStatus : u32 {
    Untested = 0,
    Boots,       // Gets past XEX loading
    Menu,        // Reaches main menu
    InGame,      // Gets in-game but issues
    Playable,    // Fully playable
};

/**
 * Import coverage info for a single library
 */
struct ImportCoverage {
    std::string library_name;
    u32 total_imports;
    u32 implemented_imports;
    std::vector<u32> missing_ordinals;  // Ordinals with no HLE handler
};

/**
 * Game information extracted from XEX metadata
 */
struct GameInfo {
    // Identity
    u32 title_id;
    u32 media_id;
    std::string module_name;

    // Execution
    u32 entry_point;
    u32 base_address;
    u32 image_size;
    u32 default_stack_size;
    u32 default_heap_size;

    // Disc info
    u8 disc_number;
    u8 disc_count;
    u32 game_region;

    // Version
    u32 version;
    u32 base_version;

    // Compatibility
    CompatStatus compat_status = CompatStatus::Untested;
    f32 import_coverage_percent = 0.0f;
    std::vector<ImportCoverage> import_libraries;
    u32 total_imports = 0;
    u32 total_implemented = 0;
    u32 critical_missing = 0;  // Missing ordinals that are commonly needed
};

/**
 * Extract GameInfo from a loaded XEX module.
 *
 * @param module The loaded XEX module
 * @param hle_functions The registered HLE function table
 * @param make_import_key Function to create lookup key from (module_id, ordinal)
 * @return GameInfo with metadata and import coverage analysis
 */
GameInfo extract_game_info(
    const XexModule& module,
    const std::unordered_map<u64, std::function<void(class Cpu*, class Memory*, u64*, u64*)>>& hle_functions,
    std::function<u64(u32, u32)> make_import_key);

/**
 * Get the module ID for a known import library name.
 * Returns: 0 = xboxkrnl.exe, 1 = xam.xex, 2 = xbdm.xex, etc.
 * Returns ~0u if unknown.
 */
u32 get_module_id(const std::string& library_name);

/**
 * Get human-readable region string from region bitmask.
 */
std::string region_to_string(u32 region);

} // namespace x360mu
