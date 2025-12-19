# 360μ (360mu)

<div align="center">

**An experimental Xbox 360 emulator for Android**

[![License](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](LICENSE)
[![Android](https://img.shields.io/badge/Android-10%2B-green.svg)](https://developer.android.com)
[![Build](https://img.shields.io/badge/Build-In%20Development-orange.svg)]()

</div>

---

## ⚠️ Project Status

**360μ is in early development.** It cannot run commercial games yet. The emulator boots and executes game code, but games currently get stuck during initialization due to incomplete GPU command processing.

### Current State (December 2024)

| Component        | Status     | Notes                                                 |
| ---------------- | ---------- | ----------------------------------------------------- |
| Memory System    | ✅ Working | 512MB main RAM, fastmem optimization                  |
| XEX Loader       | ✅ Working | Loads encrypted/compressed Xbox 360 executables       |
| CPU Interpreter  | ✅ Working | Full PowerPC instruction set                          |
| CPU JIT          | ✅ Working | PowerPC to ARM64 translation, ~80% coverage           |
| VMX128 (SIMD)    | 🟡 Partial | Basic vector instructions                             |
| Kernel HLE       | ✅ Working | ~60 syscalls implemented                              |
| Thread Scheduler | ✅ Working | 1:1 threading model (each guest thread = host thread) |
| GPU (Vulkan)     | 🟡 Partial | Initialized, waiting for ring buffer commands         |
| Audio (XMA)      | 🟡 Partial | Decoder present, not receiving data                   |
| File System      | ✅ Working | ISO mounting, VFS layer                               |

### Recent Progress (December 2024)

**Major Threading Fixes Completed:**

1. ✅ **JIT syscall PC advancement** - Fixed infinite syscall loop
2. ✅ **Context synchronization** - JIT and HLE now share context properly
3. ✅ **Address translation** - Consistent 0x1FFFFFFF masking across JIT and HLE
4. ✅ **Thread-local storage** - Proper thread identification in multi-threaded mode
5. ✅ **1:1 threading model** - Each guest thread has its own host thread

**Current Blocker:** Games progress past initial boot but get stuck in secondary polling loops. The GPU ring buffer is not yet receiving commands from the game.

See [NEXT_STEPS.md](docs/NEXT_STEPS.md) for the detailed roadmap.

## 🎯 Goals

- Create a performant Xbox 360 emulator for modern Android devices
- Leverage Vulkan for GPU emulation on mobile
- Use JIT recompilation for CPU emulation (PowerPC to ARM64)
- Support flagship devices (Snapdragon 8 Gen 2+, Dimensity 9000+)

## 📋 Requirements

### Minimum

- Android 10 (API 29)
- ARM64 processor
- Vulkan 1.1 support
- 6GB RAM

### Recommended

- Android 12+
- Snapdragon 8 Gen 2 or newer
- 8GB+ RAM
- 30GB+ free storage

## 🏗️ Building

### Prerequisites

- Android Studio Hedgehog (2023.1.1) or newer
- Android NDK r26 or newer
- CMake 3.22+
- JDK 17

### Build Steps

```bash
# Clone the repository
git clone https://github.com/yourname/360mu.git
cd 360mu

# Open in Android Studio or build from command line
cd android
./gradlew assembleDebug

# Install on device
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 📁 Project Structure

```
360mu/
├── android/              # Android app (Kotlin + Jetpack Compose)
│   └── app/src/main/
│       ├── java/         # Kotlin UI code
│       └── cpp/          # JNI bridge
├── native/               # Emulator core (C++)
│   └── src/
│       ├── cpu/          # PowerPC Xenon emulation
│       │   ├── xenon/    # Interpreter, CPU state, threading
│       │   ├── jit/      # ARM64 JIT compiler
│       │   └── vmx128/   # Vector unit (SIMD)
│       ├── gpu/          # ATI Xenos emulation
│       │   └── xenos/    # Vulkan backend, command processor
│       ├── apu/          # Audio processing (XMA)
│       ├── kernel/       # Xbox kernel HLE
│       │   └── hle/      # High-level syscall emulation
│       ├── memory/       # Memory management, fastmem
│       ├── loader/       # XEX/ISO loading
│       └── vfs/          # Virtual file system
├── shaders/              # Vulkan shaders (GLSL)
└── docs/                 # Documentation
    ├── architecture/     # Architecture documentation
    ├── issues/           # Known issues (historical)
    └── tasks/            # Development task tracking
```

## 🗺️ Roadmap

See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for the detailed technical plan.

### Phase 1: Foundation ✅

- [x] Project setup
- [x] Memory system (512MB + fastmem)
- [x] XEX loader (encryption, compression, imports)
- [x] CPU interpreter (full PPC instruction set)

### Phase 2: CPU JIT ✅

- [x] JIT framework (ARM64 emitter)
- [x] Block cache with invalidation
- [x] ~80% PPC instruction coverage
- [ ] VMX128 JIT (currently interpreted)

### Phase 3: Kernel & Threading ✅

- [x] Syscall dispatch
- [x] Thread creation/scheduling
- [x] 1:1 threading model
- [x] Context synchronization
- [x] Thread-local storage
- [ ] Additional synchronization primitives

### Phase 4: GPU 🟡 (Current Focus)

- [x] Vulkan backend initialization
- [ ] **Ring buffer command processing** ← Current blocker
- [ ] Shader translator
- [ ] eDRAM emulation

### Phase 5: Audio & I/O

- [x] XMA decoder framework
- [ ] Audio output integration
- [ ] Controller input
- [ ] Save states

## 🐛 Debugging

```bash
# View emulator logs
adb logcat -s 360mu:* 360mu-cpu:* 360mu-jit:* 360mu-kernel:*

# Check GPU status
adb logcat -d | grep "GPU\|ring buffer"

# Check thread creation
adb logcat -d | grep "Created thread"

# Check syscall dispatch
adb logcat -d | grep "First call to syscall"

# Check for unimplemented syscalls
adb logcat -d | grep "UNIMPLEMENTED"
```

## 🤝 Contributing

Contributions are welcome! Please read our contributing guidelines before submitting PRs.

### Areas needing help:

- **GPU command processing** - Ring buffer parsing, PM4 packets
- **Shader translation** - Xenos shaders to SPIR-V
- **PowerPC JIT optimization** - VMX128, missing instructions
- **Additional syscall implementations**
- **Game compatibility testing**

## 📚 Resources

- [Xbox 360 Architecture](https://www.copetti.org/writings/consoles/xbox-360/)
- [Xenia Emulator](https://github.com/xenia-project/xenia) (Reference implementation)
- [Free60 Wiki](https://free60.org/)
- [Xbox Dev Wiki](https://xboxdevwiki.net/)

## ⚖️ Legal

This emulator does not include any Xbox 360 system software or games. Users must legally own any games they wish to emulate.

**360μ is not affiliated with Microsoft Corporation.**

## 📜 License

360μ is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

---

<div align="center">

**Made with ❤️ for the emulation community**

</div>
