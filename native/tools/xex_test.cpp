/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX Test Tool
 * 
 * Command-line utility for testing XEX loading and basic validation.
 * Usage: xex_test <xex_file> [options]
 * 
 * Options:
 *   -i, --info       Print module information
 *   -s, --sections   Print section information
 *   -m, --imports    Print import libraries
 *   -e, --exports    Print exports
 *   -d, --disasm     Disassemble entry point
 *   -x, --hexdump    Hex dump at address (requires -a)
 *   -a, --address    Address for hex dump
 *   -n, --count      Number of instructions/bytes
 *   -t, --test       Run validation tests
 *   -v, --verbose    Verbose output
 *   -h, --help       Show help
 */

#include "../src/kernel/xex_loader.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace x360mu;

void print_usage(const char* program) {
    printf("360μ XEX Test Tool\n\n");
    printf("Usage: %s <xex_file> [options]\n\n", program);
    printf("Options:\n");
    printf("  -i, --info       Print module information\n");
    printf("  -s, --sections   Print section information\n");
    printf("  -m, --imports    Print import libraries\n");
    printf("  -e, --exports    Print exports\n");
    printf("  -d, --disasm     Disassemble entry point\n");
    printf("  -x, --hexdump    Hex dump at address (requires -a)\n");
    printf("  -a, --address    Address for hex dump (hex)\n");
    printf("  -n, --count      Number of instructions/bytes (default: 32)\n");
    printf("  -t, --test       Run validation tests\n");
    printf("  -v, --verbose    Verbose output\n");
    printf("  -h, --help       Show this help\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s game.xex -i -s -d -n 64\n", program);
    printf("  %s default.xex -t\n", program);
    printf("\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Parse arguments
    std::string xex_path;
    bool show_info = false;
    bool show_sections = false;
    bool show_imports = false;
    bool show_exports = false;
    bool disassemble = false;
    bool hexdump = false;
    bool run_tests = false;
    bool verbose = false;
    GuestAddr dump_address = 0;
    u32 count = 32;
    
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]);
                return 0;
            }
            else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--info") == 0) {
                show_info = true;
            }
            else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sections") == 0) {
                show_sections = true;
            }
            else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--imports") == 0) {
                show_imports = true;
            }
            else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--exports") == 0) {
                show_exports = true;
            }
            else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--disasm") == 0) {
                disassemble = true;
            }
            else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--hexdump") == 0) {
                hexdump = true;
            }
            else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
                run_tests = true;
            }
            else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                verbose = true;
            }
            else if ((strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--address") == 0) && i + 1 < argc) {
                dump_address = static_cast<GuestAddr>(strtoul(argv[++i], nullptr, 16));
            }
            else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--count") == 0) && i + 1 < argc) {
                count = static_cast<u32>(atoi(argv[++i]));
            }
            else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return 1;
            }
        } else {
            xex_path = argv[i];
        }
    }
    
    if (xex_path.empty()) {
        fprintf(stderr, "Error: No XEX file specified\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // Default to showing info if no options specified
    if (!show_info && !show_sections && !show_imports && !show_exports && 
        !disassemble && !hexdump && !run_tests) {
        show_info = true;
    }
    
    // Create test harness
    XexTestHarness harness;
    
    if (verbose) {
        printf("Initializing test harness...\n");
    }
    
    Status status = harness.initialize();
    if (status != Status::Ok) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }
    
    if (verbose) {
        printf("Loading XEX: %s\n", xex_path.c_str());
    }
    
    status = harness.load_xex(xex_path);
    if (status != Status::Ok) {
        fprintf(stderr, "Failed to load XEX: %s\n", harness.get_loader().get_error().c_str());
        return 1;
    }
    
    printf("Successfully loaded: %s\n", xex_path.c_str());
    
    // Execute requested operations
    if (show_info) {
        harness.print_module_info();
    }
    
    if (show_sections) {
        harness.print_sections();
    }
    
    if (show_imports) {
        harness.print_imports();
    }
    
    if (show_exports) {
        harness.print_exports();
    }
    
    if (disassemble) {
        harness.disassemble_entry(count);
    }
    
    if (hexdump) {
        if (dump_address == 0) {
            // Default to base address
            dump_address = harness.get_loader().get_base_address();
        }
        harness.dump_memory(dump_address, count);
    }
    
    if (run_tests) {
        bool passed = harness.run_tests();
        return passed ? 0 : 1;
    }
    
    return 0;
}

