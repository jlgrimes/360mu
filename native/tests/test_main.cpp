/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Test Suite Entry Point
 */

#include <gtest/gtest.h>

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

