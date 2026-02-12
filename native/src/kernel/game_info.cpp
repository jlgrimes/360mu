/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Game Information and Compatibility Checking Implementation
 */

#include "game_info.h"
#include <algorithm>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-gameinfo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[GameInfo] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[GameInfo WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

u32 get_module_id(const std::string& library_name) {
    // Normalize to lowercase for comparison
    std::string name = library_name;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    // Strip .exe / .xex / .dll suffix if present
    auto dot = name.rfind('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }

    if (name == "xboxkrnl") return 0;
    if (name == "xam") return 1;
    if (name == "xbdm") return 2;

    return ~0u;
}

std::string region_to_string(u32 region) {
    if (region == 0xFFFFFFFF || region == 0) return "Region Free";

    std::string result;
    if (region & 0x00FF) {
        if (region & 0x01) result += "NTSC-U ";
        if (region & 0x02) result += "NTSC-J ";
        if (region & 0x04) result += "NTSC-K ";
    }
    if (region & 0xFF00) {
        if (region & 0x0100) result += "PAL-EU ";
        if (region & 0x0200) result += "PAL-AU ";
    }

    if (result.empty()) return "Unknown";
    // Trim trailing space
    if (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

GameInfo extract_game_info(
    const XexModule& module,
    const std::unordered_map<u64, std::function<void(class Cpu*, class Memory*, u64*, u64*)>>& hle_functions,
    std::function<u64(u32, u32)> make_import_key)
{
    GameInfo info{};

    // Copy metadata from XEX module
    info.title_id = module.execution_info.title_id;
    info.media_id = module.execution_info.media_id;
    info.module_name = module.name;
    info.entry_point = module.entry_point;
    info.base_address = module.base_address;
    info.image_size = module.image_size;
    info.default_stack_size = module.default_stack_size;
    info.default_heap_size = module.default_heap_size;
    info.disc_number = module.execution_info.disc_number;
    info.disc_count = module.execution_info.disc_count;
    info.game_region = module.security_info.game_region;
    info.version = module.execution_info.version;
    info.base_version = module.execution_info.base_version;

    // Analyze import coverage for each imported library
    u32 total_imports = 0;
    u32 total_implemented = 0;

    for (const auto& lib : module.imports) {
        ImportCoverage cov;
        cov.library_name = lib.name;
        cov.total_imports = static_cast<u32>(lib.imports.size());

        u32 module_id = get_module_id(lib.name);
        u32 implemented = 0;

        for (const auto& imp : lib.imports) {
            if (module_id != ~0u) {
                u64 key = make_import_key(module_id, imp.ordinal);
                if (hle_functions.count(key)) {
                    implemented++;
                } else {
                    cov.missing_ordinals.push_back(imp.ordinal);
                }
            } else {
                // Unknown library — all missing
                cov.missing_ordinals.push_back(imp.ordinal);
            }
        }

        cov.implemented_imports = implemented;
        total_imports += cov.total_imports;
        total_implemented += implemented;

        info.import_libraries.push_back(std::move(cov));
    }

    info.total_imports = total_imports;
    info.total_implemented = total_implemented;

    if (total_imports > 0) {
        info.import_coverage_percent =
            static_cast<f32>(total_implemented) / static_cast<f32>(total_imports) * 100.0f;
    }

    // Count critical missing — ordinals in xboxkrnl that are commonly needed
    // (threading, memory, sync primitives, file I/O)
    for (const auto& lib : info.import_libraries) {
        if (get_module_id(lib.library_name) == 0) {  // xboxkrnl
            info.critical_missing += static_cast<u32>(lib.missing_ordinals.size());
        }
    }

    LOGI("Game: %s (Title ID: 0x%08X)", info.module_name.c_str(), info.title_id);
    LOGI("  Import coverage: %u/%u (%.1f%%)",
         total_implemented, total_imports, info.import_coverage_percent);
    LOGI("  Critical missing (xboxkrnl): %u", info.critical_missing);

    for (const auto& lib : info.import_libraries) {
        if (!lib.missing_ordinals.empty()) {
            LOGI("  %s: %u/%u implemented, %zu missing",
                 lib.library_name.c_str(),
                 lib.implemented_imports, lib.total_imports,
                 lib.missing_ordinals.size());
        }
    }

    return info;
}

} // namespace x360mu
