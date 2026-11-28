# astro

Native Matrix Client For Ubuntu Touch, [Qt/QML, a C++ backend and a Rust Matrix core];

This is an early release of the app! There are still some performance optimizations, but works well! I tried really hard with this, and yes I did use some AI tools (wouldn't have been possible alone otherwise).

https://github.com/devabu26/astro


currently supports:
E2EE,
Video Calls, 
Rooms and Spaces -> cannot currently join/leave/edit rooms+spaces,
Live updating chats, -> emoji+replies+edit+delete,
Device verification + Handling,

Working now on stability fixes, 
intended next work will commence on adding / editing rooms and friends and also adding more customization settings.

Building Astro for Ubuntu Touch

Astro is a native Matrix client for Ubuntu Touch. It is built using Clickable, Qt/QML, C++, and Rust.

##Building
This guide covers building the ARM64 Ubuntu Touch application from source and deploying it to a device.

1. Build environment
The current development configuration uses:
#Component
Version / configuration
#Host OS
Fedora KDE
#Build system
Clickable 8.10.0
#Target architecture
ARM64 (aarch64)
#Ubuntu SDK
ubuntu-sdk-20.04.1
#Build container
docker.io/clickable/amd64-20.04-arm64
#UI framework
Qt 5 / QML
#Device platform
Ubuntu Touch 24.04

#Matrix backend
Rust Matrix SDK
Native backend
C++ / Qt
The build is container-based, so the Ubuntu SDK environment is provided by the Clickable build configuration rather than requiring a complete Ubuntu development environment on Fedora.

2. Requirements
Install and configure:
#Git
#Clickable 8.10.0
#Podman or Docker, configured for Clickable
#Android Debug Bridge (adb) for USB device deployment
#An Ubuntu Touch device with Developer Mode enabled

#Verify the installed tools:
git --version
clickable --version
adb version

3. Get the source code

#Clone the repository:
git clone https://github.com/devabu26/Astro.git
cd Astro

#Switch to the main development branch:
git checkout main
git pull origin main

#The primary development branch is main.

4. Build Astro

#From the repository root:
clickable build

Clickable reads the project's clickable.yaml, prepares the configured build container, and builds the application for its configured target architecture.

The project includes both native and QML components, as well as the Rust Matrix backend.

#If you need a clean rebuild:
clickable clean
clickable build

5. Connect an Ubuntu Touch device

#On the device:
Enable Developer Mode.
Connect the device to your computer using USB.
Unlock the device.

Accept the debugging authorisation prompt if requested.

#Check the connection:
adb devices

(or: clickable devices)
The device should appear as connected.

6. Install Astro
#Install the newly built application:
clickable install

#Launch it:
clickable launch

#For a typical development cycle, these commands can be run after each source change:
clickable build
clickable install
clickable launch

## License

Copyright (C) 2026  Marc D.B [madabu], Sydney, Australia,
    
This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License version 3, as published by the
Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranties of MERCHANTABILITY, SATISFACTORY
QUALITY, or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.
