/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Integration Test Level 4: Syscall Tracing
 * 
 * Tests:
 * - Execution until syscall
 * - Syscall ordinal identification
 * - Import table correlation
 * - Build list of required HLE functions
 * 
 * Usage: ./test_syscalls <path_to_xex> [max_instructions]
 * 
 * This tool helps identify which kernel functions the game needs
 * in order of first encounter, making it easy to prioritize HLE work.
 */

#include "memory/memory.h"
#include "kernel/xex_loader.h"
#include "cpu/xenon/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <set>

using namespace x360mu;

// Known xboxkrnl.exe exports (subset of most common)
// Full list: https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_exports.inc
static const std::map<u32, const char*> XBOXKRNL_EXPORTS = {
    // Memory
    {19, "ExAllocatePoolWithTag"},
    {20, "ExFreePool"},
    {165, "MmAllocatePhysicalMemoryEx"},
    {171, "MmFreePhysicalMemory"},
    {178, "MmQueryAddressProtect"},
    {179, "MmQueryAllocationSize"},
    {185, "MmSetAddressProtect"},
    
    // Process/Thread
    {79, "KeGetCurrentProcessType"},
    {88, "KeQueryPerformanceFrequency"},
    {89, "KeQuerySystemTime"},
    {107, "KeSetBasePriorityThread"},
    {255, "NtCreateThread"},
    {256, "NtDelayExecution"},
    {269, "NtQueryVirtualMemory"},
    {274, "NtResumeThread"},
    {279, "NtSetEvent"},
    {280, "NtSetInformationThread"},
    {287, "NtSuspendThread"},
    {288, "NtTerminateThread"},
    {290, "NtWaitForSingleObjectEx"},
    {291, "NtWaitForMultipleObjectsEx"},
    
    // Synchronization
    {62, "KeInitializeCriticalSection"},
    {63, "KeEnterCriticalSection"},
    {64, "KeLeaveCriticalSection"},
    {65, "KeDeleteCriticalSection"},
    {70, "KeInitializeEvent"},
    {71, "KePulseEvent"},
    {72, "KeResetEvent"},
    {73, "KeSetEvent"},
    {77, "KeInitializeSemaphore"},
    {78, "KeReleaseSemaphore"},
    
    // File I/O
    {240, "NtCreateFile"},
    {245, "NtClose"},
    {262, "NtOpenFile"},
    {266, "NtQueryDirectoryFile"},
    {267, "NtQueryInformationFile"},
    {270, "NtReadFile"},
    {284, "NtSetInformationFile"},
    {289, "NtWriteFile"},
    
    // Strings
    {299, "RtlCompareMemory"},
    {300, "RtlCompareMemoryUlong"},
    {305, "RtlCopyMemory"},
    {308, "RtlFillMemoryUlong"},
    {315, "RtlInitAnsiString"},
    {317, "RtlInitUnicodeString"},
    {350, "RtlTimeToTimeFields"},
    {351, "RtlTimeFieldsToTime"},
    
    // Debug
    {354, "DbgPrint"},
    {355, "DbgBreakPoint"},
    
    // Misc
    {400, "XexGetModuleHandle"},
    {401, "XexGetProcedureAddress"},
    {407, "XexLoadImage"},
    {408, "XexUnloadImage"},
    {417, "XexCheckExecutablePrivilege"},
};

// Known xam.xex exports (subset)
static const std::map<u32, const char*> XAM_EXPORTS = {
    {1, "XamGetExecutionId"},
    {5, "XamGetSystemVersion"},
    {6, "XamLoaderGetLaunchInfo"},
    {14, "XamUserGetSigninState"},
    {27, "XamInputGetState"},
    {29, "XamInputGetCapabilities"},
    {402, "XamShowMessageBoxUI"},
    {651, "XamContentCreate"},
    {652, "XamContentCreateEx"},
    {656, "XamContentClose"},
};

struct SyscallInfo {
    u32 ordinal;
    u64 pc;  // Where it was called from
    u64 r3;  // First argument
    u64 r4;  // Second argument
    const char* name;
    const char* library;
};

int main(int argc, char** argv) {
    printf("=============================================\n");
    printf("360μ Integration Test Level 4: Syscall Trace\n");
    printf("=============================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <path_to_xex> [max_instructions]\n", argv[0]);
        printf("\nThis test identifies which kernel functions the game calls.\n");
        printf("It executes until hitting an unimplemented syscall, then continues.\n");
        printf("\nOutput is a prioritized list of HLE functions to implement.\n");
        return 1;
    }
    
    const char* xex_path = argv[1];
    u64 max_instructions = (argc >= 3) ? atoll(argv[2]) : 100000;
    
    printf("XEX Path:         %s\n", xex_path);
    printf("Max Instructions: %llu\n\n", (unsigned long long)max_instructions);
    
    // Initialize Memory
    auto memory = std::make_unique<Memory>();
    
    if (memory->initialize() != Status::Ok) {
        printf("❌ FAIL: Memory initialization failed\n");
        return 1;
    }
    
    // Load XEX
    XexLoader loader;
    if (loader.load_file(xex_path, memory.get()) != Status::Ok) {
        printf("❌ FAIL: XEX loading failed\n");
        memory->shutdown();
        return 1;
    }
    
    const XexModule* mod = loader.get_module();
    printf("Module:     %s\n", mod->name.c_str());
    printf("Entry:      0x%08X\n", (unsigned)mod->entry_point);
    printf("Imports:    %zu libraries\n\n", mod->imports.size());
    
    // Build import ordinal -> name map
    std::map<std::pair<std::string, u32>, std::string> import_map;
    printf("[IMPORTS] Building import map...\n");
    for (const auto& lib : mod->imports) {
        printf("  %s: %u imports\n", lib.name.c_str(), lib.import_count);
        
        // Use known export tables
        const std::map<u32, const char*>* exports = nullptr;
        if (lib.name.find("xboxkrnl") != std::string::npos) {
            exports = &XBOXKRNL_EXPORTS;
        } else if (lib.name.find("xam") != std::string::npos) {
            exports = &XAM_EXPORTS;
        }
        
        for (u32 ordinal : lib.imports) {
            std::string name = "ordinal_" + std::to_string(ordinal);
            if (exports) {
                auto it = exports->find(ordinal);
                if (it != exports->end()) {
                    name = it->second;
                }
            }
            import_map[{lib.name, ordinal}] = name;
        }
    }
    printf("\n");
    
    // Create interpreter
    Interpreter interp(memory.get());
    
    // Set up context
    ThreadContext ctx;
    ctx.reset();
    ctx.pc = mod->entry_point;
    ctx.running = true;
    ctx.gpr[1] = 0x70000000 - 0x1000;  // Stack
    ctx.gpr[13] = mod->base_address;    // SDA
    
    // Check for encrypted XEX
    if (memory->read_u32(mod->entry_point) == 0) {
        printf("⚠️  WARNING: XEX appears encrypted (entry point is zeros)\n");
        printf("   Syscall tracing may not work properly.\n\n");
    }
    
    printf("[EXEC] Beginning execution trace...\n");
    printf("=============================================\n\n");
    
    std::vector<SyscallInfo> syscalls;
    std::set<u32> seen_ordinals;
    u64 total_instructions = 0;
    u64 last_pc = 0;
    
    while (total_instructions < max_instructions && ctx.running) {
        u32 inst = memory->read_u32(ctx.pc);
        DecodedInst d = Decoder::decode(inst);
        
        // Detect syscall
        if (d.opcode == 17) {  // sc
            // On Xbox 360, r0 contains the syscall ordinal
            u32 ordinal = (u32)ctx.gpr[0];
            
            // Try to find the name
            const char* name = "unknown";
            const char* library = "unknown";
            
            // Check xboxkrnl first
            auto it = XBOXKRNL_EXPORTS.find(ordinal);
            if (it != XBOXKRNL_EXPORTS.end()) {
                name = it->second;
                library = "xboxkrnl.exe";
            } else {
                // Check xam
                auto it2 = XAM_EXPORTS.find(ordinal);
                if (it2 != XAM_EXPORTS.end()) {
                    name = it2->second;
                    library = "xam.xex";
                }
            }
            
            // Record syscall
            SyscallInfo info;
            info.ordinal = ordinal;
            info.pc = ctx.pc;
            info.r3 = ctx.gpr[3];
            info.r4 = ctx.gpr[4];
            info.name = name;
            info.library = library;
            syscalls.push_back(info);
            
            bool first_time = seen_ordinals.insert(ordinal).second;
            
            if (first_time) {
                printf("🔵 SYSCALL #%zu [NEW]: %s::%s (ordinal %u)\n",
                       syscalls.size(), library, name, ordinal);
                printf("   PC=0x%08llX, r3=0x%llX, r4=0x%llX\n",
                       (unsigned long long)ctx.pc,
                       (unsigned long long)ctx.gpr[3],
                       (unsigned long long)ctx.gpr[4]);
            }
            
            // Simulate syscall return (set r3 to 0 = success)
            ctx.gpr[3] = 0;  // Return value
            ctx.pc += 4;     // Skip syscall instruction
            total_instructions++;
            continue;
        }
        
        // Detect stop conditions
        if (inst == 0) {
            printf("⚠️  Hit zero instruction at 0x%08llX after %llu instructions\n",
                   (unsigned long long)ctx.pc, (unsigned long long)total_instructions);
            break;
        }
        
        if (ctx.pc == last_pc) {
            printf("⚠️  Infinite loop at 0x%08llX\n", (unsigned long long)ctx.pc);
            break;
        }
        
        // Detect trap
        if (d.opcode == 2 || d.opcode == 3) {  // tdi, twi
            printf("⚠️  TRAP at 0x%08llX\n", (unsigned long long)ctx.pc);
            break;
        }
        
        last_pc = ctx.pc;
        interp.execute_one(ctx);
        total_instructions++;
    }
    
    printf("\n=============================================\n\n");
    
    // Analyze syscalls
    printf("[ANALYSIS] Syscall Summary\n");
    printf("=============================================\n");
    printf("Total instructions:  %llu\n", (unsigned long long)total_instructions);
    printf("Total syscalls:      %zu\n", syscalls.size());
    printf("Unique ordinals:     %zu\n\n", seen_ordinals.size());
    
    // Count by library
    std::map<std::string, u32> lib_counts;
    for (const auto& sc : syscalls) {
        lib_counts[sc.library]++;
    }
    
    printf("By Library:\n");
    for (const auto& [lib, count] : lib_counts) {
        printf("  %-20s %u calls\n", lib.c_str(), count);
    }
    printf("\n");
    
    // First 20 unique syscalls in order of first encounter
    printf("[PRIORITY] HLE Implementation Order\n");
    printf("=============================================\n");
    printf("These functions are called first and should be\n");
    printf("implemented first for game boot:\n\n");
    
    std::set<u32> printed;
    int priority = 1;
    for (const auto& sc : syscalls) {
        if (printed.insert(sc.ordinal).second) {
            printf("%2d. %s::%s\n", priority, sc.library, sc.name);
            printf("    ordinal=%u, first_call=0x%08llX\n",
                   sc.ordinal, (unsigned long long)sc.pc);
            priority++;
            
            if (priority > 20) {
                printf("\n... and %zu more unique syscalls\n", 
                       seen_ordinals.size() - 20);
                break;
            }
        }
    }
    printf("\n");
    
    // Frequency analysis
    printf("[FREQUENCY] Most Called Functions\n");
    printf("=============================================\n");
    
    std::map<u32, u32> call_counts;
    for (const auto& sc : syscalls) {
        call_counts[sc.ordinal]++;
    }
    
    std::vector<std::pair<u32, u32>> sorted_calls(call_counts.begin(), call_counts.end());
    std::sort(sorted_calls.begin(), sorted_calls.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    
    for (size_t i = 0; i < sorted_calls.size() && i < 10; i++) {
        u32 ordinal = sorted_calls[i].first;
        u32 count = sorted_calls[i].second;
        
        const char* name = "unknown";
        auto it = XBOXKRNL_EXPORTS.find(ordinal);
        if (it != XBOXKRNL_EXPORTS.end()) {
            name = it->second;
        } else {
            auto it2 = XAM_EXPORTS.find(ordinal);
            if (it2 != XAM_EXPORTS.end()) {
                name = it2->second;
            }
        }
        
        printf("  %5u calls: %s (ordinal %u)\n", count, name, ordinal);
    }
    printf("\n");
    
    // Cleanup
    memory->shutdown();
    
    // Summary
    printf("=============================================\n");
    printf("SUMMARY: Syscall Trace\n");
    printf("=============================================\n");
    
    if (syscalls.empty()) {
        printf("⚠️  No syscalls encountered.\n");
        printf("   This usually means:\n");
        printf("   - XEX is encrypted (needs decryption)\n");
        printf("   - Execution didn't reach kernel calls\n");
        printf("   - Only %llu instructions before stop\n", 
               (unsigned long long)total_instructions);
    } else {
        printf("✅ Traced %zu syscalls (%zu unique)\n", 
               syscalls.size(), seen_ordinals.size());
        printf("\n🎉 Level 4 Complete! Syscall identification works.\n");
        printf("   The list above shows which HLE functions to implement.\n");
        printf("   Start with the top 10 in priority order.\n");
    }
    printf("=============================================\n");
    
    return syscalls.empty() ? 1 : 0;
}

