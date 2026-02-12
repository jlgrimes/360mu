/**
 * Xbox 360 Address Mapping Unit Tests
 * 
 * This file tests the expected behavior for all address ranges in the Xbox 360
 * memory map. The JIT compiler and Memory class must handle these correctly.
 * 
 * Xbox 360 Physical Memory Map:
 * =============================
 * 0x00000000 - 0x1FFFFFFF : Main RAM (512 MB) - FASTMEM OK
 * 0x20000000 - 0x7FBFFFFF : Reserved/Hardware - MMIO REQUIRED
 * 0x7FC00000 - 0x7FFFFFFF : GPU MMIO Registers (4 MB) - MMIO REQUIRED
 * 0x80000000 - 0xFFFFFFFF : Not physical, see virtual below
 * 
 * Xbox 360 Virtual Address Spaces:
 * ================================
 * 0x00000000 - 0x3FFFFFFF : Physical identity map (usermode can't access all)
 * 0x40000000 - 0x7FFFFFFF : User virtual space
 * 0x80000000 - 0x9FFFFFFF : Cached physical mirror -> physical & 0x1FFFFFFF - FASTMEM OK
 * 0xA0000000 - 0xBFFFFFFF : Uncached physical mirror - MMIO REQUIRED (no simple translation)
 * 0xC0000000 - 0xC3FFFFFF : GPU virtual (maps to 0x7FC00000+) - MMIO REQUIRED  
 * 0xC4000000 - 0xDFFFFFFF : Kernel virtual - MMIO REQUIRED
 * 0xE0000000 - 0xFFFFFFFF : More kernel/hardware - MMIO REQUIRED
 * 
 * Fastmem Constraints:
 * ====================
 * - Fastmem reserves 4GB but only maps first 512MB as read/write
 * - Only physical addresses 0x00000000-0x1FFFFFFF can use fastmem
 * - Virtual addresses 0x80000000-0x9FFFFFFF translate via & 0x1FFFFFFF -> fastmem OK
 * - All other addresses MUST go through MMIO/slow path
 */

#include <gtest/gtest.h>
#include <cstdint>

using u32 = uint32_t;
using u64 = uint64_t;
using GuestAddr = u64;

// Constants matching the JIT compiler
constexpr u64 MAIN_RAM_SIZE = 0x20000000;        // 512 MB
constexpr u64 MAIN_RAM_MASK = 0x1FFFFFFF;        // Mask for 512 MB
constexpr u64 GPU_MMIO_START = 0x7FC00000;
constexpr u64 GPU_MMIO_END = 0x80000000;
constexpr u64 USERMODE_VIRT_START = 0x80000000;
constexpr u64 USERMODE_VIRT_END = 0xA0000000;
constexpr u64 KERNEL_SPACE_START = 0xA0000000;
constexpr u64 GPU_VIRT_START = 0xC0000000;
constexpr u64 GPU_VIRT_END = 0xC4000000;

/**
 * Determines if an address can use the fastmem path.
 * This must match the JIT compiler's logic exactly.
 */
enum class MemoryPath {
    FASTMEM,    // Can use direct memory access via fastmem
    MMIO        // Must go through Memory class (slow path)
};

struct AddressRouting {
    MemoryPath path;
    u64 physical_addr;  // The physical address after translation (if applicable)
};

/**
 * Simulates the JIT compiler's address routing logic.
 * Returns which path should be used and the translated physical address.
 */
AddressRouting route_address(GuestAddr addr) {
    // Step 1: Check for kernel space (>= 0xA0000000)
    // These addresses cannot use the simple AND mask translation
    if (addr >= KERNEL_SPACE_START) {
        // GPU virtual range (0xC0000000-0xC3FFFFFF) -> maps to GPU MMIO
        if (addr >= GPU_VIRT_START && addr < GPU_VIRT_END) {
            u64 phys = GPU_MMIO_START + ((addr - GPU_VIRT_START) & 0x3FFFFF);
            return {MemoryPath::MMIO, phys};
        }
        // All other kernel addresses -> MMIO (Memory class handles translation)
        return {MemoryPath::MMIO, addr};
    }
    
    // Step 2: Check if usermode virtual (0x80000000-0x9FFFFFFF)
    u64 physical;
    if (addr >= USERMODE_VIRT_START && addr < USERMODE_VIRT_END) {
        // Translate to physical via AND mask
        physical = addr & MAIN_RAM_MASK;
    } else {
        // Already physical
        physical = addr;
    }
    
    // Step 3: Check if physical address is in main RAM (< 0x20000000)
    if (physical < MAIN_RAM_SIZE) {
        return {MemoryPath::FASTMEM, physical};
    }
    
    // Step 4: Physical address is outside main RAM -> MMIO
    // This includes GPU MMIO (0x7FC00000+) and other hardware regions
    return {MemoryPath::MMIO, physical};
}

//=============================================================================
// Test Cases: Main RAM (Physical)
//=============================================================================

TEST(AddressMapping, PhysicalMainRAM_Start) {
    auto result = route_address(0x00000000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x00000000);
}

TEST(AddressMapping, PhysicalMainRAM_Middle) {
    auto result = route_address(0x10000000);  // 256 MB
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x10000000);
}

TEST(AddressMapping, PhysicalMainRAM_LastByte) {
    auto result = route_address(0x1FFFFFFF);  // Last byte of 512 MB
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x1FFFFFFF);
}

TEST(AddressMapping, PhysicalMainRAM_End) {
    // First address OUTSIDE main RAM - should go to MMIO
    auto result = route_address(0x20000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
    EXPECT_EQ(result.physical_addr, 0x20000000);
}

//=============================================================================
// Test Cases: High Physical Addresses (Hardware/Reserved)
//=============================================================================

TEST(AddressMapping, PhysicalReserved_JustAboveRAM) {
    auto result = route_address(0x20000001);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

TEST(AddressMapping, PhysicalReserved_Middle) {
    auto result = route_address(0x50000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

TEST(AddressMapping, PhysicalGPU_Start) {
    auto result = route_address(0x7FC00000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
    EXPECT_EQ(result.physical_addr, 0x7FC00000);
}

TEST(AddressMapping, PhysicalGPU_Middle) {
    auto result = route_address(0x7FE00000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

TEST(AddressMapping, PhysicalGPU_LastByte) {
    auto result = route_address(0x7FFFFFFF);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Test Cases: Usermode Virtual (0x80000000 - 0x9FFFFFFF)
// These translate to physical via & 0x1FFFFFFF
//=============================================================================

TEST(AddressMapping, UsermodeVirtual_Start) {
    auto result = route_address(0x80000000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x00000000);  // 0x80000000 & 0x1FFFFFFF = 0
}

TEST(AddressMapping, UsermodeVirtual_Middle) {
    auto result = route_address(0x82000000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x02000000);
}

TEST(AddressMapping, UsermodeVirtual_High) {
    auto result = route_address(0x90000000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x10000000);  // 0x90000000 & 0x1FFFFFFF = 0x10000000
}

TEST(AddressMapping, UsermodeVirtual_LastByte) {
    auto result = route_address(0x9FFFFFFF);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x1FFFFFFF);
}

TEST(AddressMapping, UsermodeVirtual_End) {
    // 0xA0000000 is first kernel address - should NOT use fastmem
    auto result = route_address(0xA0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Test Cases: Kernel Space (0xA0000000+) - All must use MMIO
//=============================================================================

TEST(AddressMapping, KernelSpace_UncachedPhysical) {
    // 0xA0000000-0xBFFFFFFF is uncached physical mirror
    auto result = route_address(0xA0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

TEST(AddressMapping, KernelSpace_UncachedPhysicalMid) {
    auto result = route_address(0xB0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Test Cases: GPU Virtual Range (0xC0000000 - 0xC3FFFFFF)
//=============================================================================

TEST(AddressMapping, GPUVirtual_Start) {
    auto result = route_address(0xC0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
    // Maps to physical 0x7FC00000
    EXPECT_EQ(result.physical_addr, 0x7FC00000);
}

TEST(AddressMapping, GPUVirtual_RegisterOffset) {
    // GPU register at offset 0x1000 from GPU base
    auto result = route_address(0xC0001000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
    EXPECT_EQ(result.physical_addr, 0x7FC01000);
}

TEST(AddressMapping, GPUVirtual_End) {
    // Last byte of GPU virtual range
    auto result = route_address(0xC3FFFFFF);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
    EXPECT_EQ(result.physical_addr, 0x7FFFFFFF);
}

TEST(AddressMapping, GPUVirtual_JustAfter) {
    // Just after GPU virtual range - still kernel, still MMIO
    auto result = route_address(0xC4000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Test Cases: Other Kernel Virtual Ranges
//=============================================================================

TEST(AddressMapping, KernelVirtual_AfterGPU) {
    auto result = route_address(0xD0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

TEST(AddressMapping, KernelVirtual_High) {
    auto result = route_address(0xF0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

TEST(AddressMapping, KernelVirtual_Max) {
    auto result = route_address(0xFFFFFFFF);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Test Cases: Edge Cases and Crash Addresses
//=============================================================================

TEST(AddressMapping, CrashAddress_21286000) {
    // This was a crash address - physical 0x21286000 (above 512MB)
    // Must go to MMIO, NOT fastmem
    auto result = route_address(0x21286000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
    EXPECT_EQ(result.physical_addr, 0x21286000);
}

TEST(AddressMapping, EdgeCase_40000000) {
    // User virtual space - might be mapped or not
    auto result = route_address(0x40000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);  // Not in main RAM range
}

TEST(AddressMapping, EdgeCase_7FC00000_Direct) {
    // Direct access to GPU MMIO via physical address
    auto result = route_address(0x7FC00000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Test Cases: Common Game Access Patterns  
//=============================================================================

TEST(AddressMapping, GamePattern_StackAccess) {
    // Stack is typically in usermode virtual (0x80000000 range)
    auto result = route_address(0x82100000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
}

TEST(AddressMapping, GamePattern_HeapAccess) {
    // Heap also in usermode virtual
    auto result = route_address(0x88000000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
    EXPECT_EQ(result.physical_addr, 0x08000000);
}

TEST(AddressMapping, GamePattern_CodeAccess) {
    // Code loaded in usermode virtual
    auto result = route_address(0x82000000);
    EXPECT_EQ(result.path, MemoryPath::FASTMEM);
}

TEST(AddressMapping, GamePattern_GPUCommand) {
    // GPU command buffer write
    auto result = route_address(0xC0000000);
    EXPECT_EQ(result.path, MemoryPath::MMIO);
}

//=============================================================================
// Batch Test: Verify all addresses in a range
//=============================================================================

TEST(AddressMapping, BatchTest_MainRAM) {
    // Sample addresses across main RAM - all should be FASTMEM
    for (u64 addr = 0; addr < MAIN_RAM_SIZE; addr += 0x01000000) {  // Every 16 MB
        auto result = route_address(addr);
        EXPECT_EQ(result.path, MemoryPath::FASTMEM) 
            << "Address 0x" << std::hex << addr << " should use FASTMEM";
        EXPECT_EQ(result.physical_addr, addr);
    }
}

TEST(AddressMapping, BatchTest_AboveMainRAM) {
    // Sample addresses above main RAM - all should be MMIO
    for (u64 addr = MAIN_RAM_SIZE; addr < GPU_MMIO_END; addr += 0x10000000) {
        auto result = route_address(addr);
        EXPECT_EQ(result.path, MemoryPath::MMIO)
            << "Address 0x" << std::hex << addr << " should use MMIO";
    }
}

TEST(AddressMapping, BatchTest_UsermodeVirtual) {
    // Sample usermode virtual addresses - all should translate to FASTMEM
    for (u64 addr = USERMODE_VIRT_START; addr < USERMODE_VIRT_END; addr += 0x02000000) {
        auto result = route_address(addr);
        EXPECT_EQ(result.path, MemoryPath::FASTMEM)
            << "Address 0x" << std::hex << addr << " should use FASTMEM";
        u64 expected_phys = addr & MAIN_RAM_MASK;
        EXPECT_EQ(result.physical_addr, expected_phys);
    }
}

TEST(AddressMapping, BatchTest_KernelSpace) {
    // Sample kernel addresses - all should be MMIO
    for (u64 addr = KERNEL_SPACE_START; addr < 0xFFFFFFFF; addr += 0x10000000) {
        auto result = route_address(addr);
        EXPECT_EQ(result.path, MemoryPath::MMIO)
            << "Address 0x" << std::hex << addr << " should use MMIO";
    }
}
