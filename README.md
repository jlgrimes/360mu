# 360μ (360mu)

<div align="center">

**An experimental Xbox 360 emulator for Android**

[![License](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](LICENSE)
[![Android](https://img.shields.io/badge/Android-10%2B-green.svg)](https://developer.android.com)
[![Build](https://img.shields.io/badge/Build-In%20Development-orange.svg)]()

</div>

---

## ⚠️ Project Status

**360μ is in early development.** It cannot run commercial games yet. This is a long-term research project aimed at bringing Xbox 360 emulation to Android devices.

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
./gradlew installDebug
```

## 📁 Project Structure

```
360mu/
├── android/          # Android app (Kotlin + Jetpack Compose)
├── native/           # Emulator core (C++)
│   ├── src/cpu/      # PowerPC Xenon emulation
│   ├── src/gpu/      # ATI Xenos emulation (Vulkan)
│   ├── src/apu/      # Audio processing (XMA)
│   ├── src/kernel/   # Xbox kernel HLE
│   └── src/memory/   # Memory management
├── shaders/          # Vulkan shaders
└── docs/             # Documentation
```

## 🗺️ Roadmap

See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for the detailed technical plan.

### Phase 1: Foundation ⏳
- [x] Project setup
- [ ] Memory system
- [ ] XEX loader
- [ ] CPU interpreter

### Phase 2: CPU JIT
- [ ] JIT framework
- [ ] Full PPC support
- [ ] VMX128 emulation

### Phase 3: GPU
- [ ] Vulkan backend
- [ ] Shader translator
- [ ] eDRAM emulation

### Phase 4: Audio & I/O
- [ ] XMA decoder
- [ ] Input system
- [ ] File system

## 🤝 Contributing

Contributions are welcome! Please read our contributing guidelines before submitting PRs.

### Areas needing help:
- PowerPC JIT optimization
- Vulkan rendering
- Xenos shader translation
- Game compatibility testing

## 📚 Resources

- [Xbox 360 Architecture](https://www.copetti.org/writings/consoles/xbox-360/)
- [Xenia Emulator](https://github.com/xenia-project/xenia) (Reference implementation)
- [Free60 Wiki](https://free60.org/)

## ⚖️ Legal

This emulator does not include any Xbox 360 system software or games. Users must legally own any games they wish to emulate.

**360μ is not affiliated with Microsoft Corporation.**

## 📜 License

360μ is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

---

<div align="center">

**Made with ❤️ for the emulation community**

</div>

