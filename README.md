<p align="center">
  <img src="https://github.com/devabu26/astro/blob/main/assets/logo.svg" alt="Astro" width = 200 height = 200> <br>
  Astro
</p>
  
<p align="center">
  <strong>A native Matrix client for Ubuntu Touch</strong><br>
  Private conversations, rich media and real-time calls — built for the Ubuntu Touch experience.
</p>

<p align="center">
  <a href="https://github.com/devabu26/astro"><img src="https://img.shields.io/badge/platform-Ubuntu%20Touch-E95420?logo=ubuntu&logoColor=white" alt="Ubuntu Touch"></a>
  <a href="https://matrix.org/"><img src="https://img.shields.io/badge/protocol-Matrix-0DBD8B?logo=matrix&logoColor=white" alt="Matrix"></a>
  <img src="https://img.shields.io/badge/language-C%2B%2B%20%7C%20Rust%20%7C%20QML-6C8EBF" alt="C++, Rust and QML">
  <img src="https://img.shields.io/badge/license-GPL--3.0-blue" alt="GPL-3.0">
  <img src="https://img.shields.io/badge/status-early%20release-orange" alt="Early release">
</p>

Astro is an open-source Matrix client built specifically for Ubuntu Touch. It combines a Qt/QML interface with a C++ application backend and a Rust-powered Matrix core.

> **Project status:** Astro is in early release. Core messaging and calling features are available, while stability, performance and room-management capabilities continue to be developed.

## ✨ Features

### 💬 Messaging and conversations
- Live-updating Matrix room timelines.
- Room and Space browsing.
- Message replies, editing and deletion.
- Emoji support.
- Unread-message and mention tracking.
- Older-message pagination.

### 🔐 Privacy and security
- Matrix end-to-end encrypted (E2EE) conversations.
- Matrix device verification and device handling.
- Encrypted event and media support.

### 📷 Media and calls
- Image and video content in conversations.
- Image viewer with pinch-to-zoom and panning.
- Voice and video calling.
- Front and rear camera support.
- Camera rotation handling and video resolution controls.
- Call controls and encrypted-call indication.

### 🔔 Ubuntu Touch integration
- Native Ubuntu Touch interface.
- In-app notification banners.
- Matrix-driven notification handling and push-notification integration.
- ARM64 Click packaging for Ubuntu Touch devices.

## 🚧 Known limitations and work in progress

Astro is actively developed. Some capabilities are not yet complete:

- Joining, leaving and editing rooms or Spaces is not currently supported.
- Room and contact management is still planned.
- Additional customisation settings are planned.
- Media playback, calling stability and performance continue to be refined.

## 🛠️ Technology

| Component | Technology |
| --- | --- |
| User interface | Qt 5 / QML |
| Native application backend | C++ / Qt |
| Matrix protocol implementation | Rust Matrix SDK |
| Messaging protocol | Matrix |
| Target platform | Ubuntu Touch |
| Target architecture | ARM64 (AArch64) |
| Build system | Clickable / CMake |

## 🧱 Building from source

These instructions build Astro for an Ubuntu Touch device. The repository's `clickable.yaml` is the authoritative source for the current build configuration.

### Requirements

- Git
- [Clickable](https://clickable-ut.dev/) **8.9.0 or newer**
- Docker or Podman configured for Clickable
- An Ubuntu Touch device with Developer Mode enabled (for installation and testing)
- ADB for USB device access

The Clickable configuration selects the Ubuntu Touch 24.04 framework and prepares the required native and Rust cross-compilation toolchain inside the build environment.

### 1. Clone the repository

```bash
git clone https://github.com/devabu26/astro.git
cd astro
git checkout main
git pull origin main
```

### 2. Build

From the repository root:

```bash
clickable build
```

For a clean rebuild:

```bash
clickable clean
clickable build
```

### 3. Connect your device

Enable Developer Mode on the Ubuntu Touch device, connect it over USB, unlock it and accept the debugging authorisation prompt if one appears.

Check that ADB can see the device:

```bash
adb devices
```

### 4. Install and launch

```bash
clickable install
clickable launch
```

A typical development cycle after making changes is:

```bash
clickable build
clickable install
clickable launch
```

### 5. Debugging

View application logs with Clickable:

```bash
clickable logs
```

For system-level diagnostics on the device:

```bash
adb shell journalctl -b --no-pager
```

Filter for Astro messages:

```bash
adb shell "journalctl -b --no-pager | grep -i astro"
```

## 🤝 Contributing

Contributions, bug reports and testing feedback are welcome. Astro is under active development, so please include useful diagnostic information when reporting a problem:

- Ubuntu Touch device and OS version.
- Steps to reproduce the issue.
- Relevant Astro or system logs.
- Whether the issue is repeatable.

Please check the existing repository issues and discussions before opening a new report.

## 📄 License

Astro is free software released under the **GNU General Public License v3.0**.

Copyright © 2026 Marc D.B. (madabu).

See the [GNU GPL v3.0](https://www.gnu.org/licenses/gpl-3.0.html) for the full license terms.
