# CYD 3D Maze Desk Clock

An interactive retro **3D Dungeon Crawler Desk Clock** running natively on the **ESP32-2432S028** (Cheap Yellow Display / CYD) 2-USB port version!

![CYD 3D Maze Desk Clock Demo](demo.gif)

---

## Overview

The **CYD 3D Maze Desk Clock** transforms your ESP32 Cheap Yellow Display into a dynamic retro 3D desk clock. Featuring a real-time raycasting 3D engine, this desk clock autonomously navigates procedural dungeons, battles monsters, loots treasures, and displays a customizable digital clock with interactive touch controls.

It serves as an eye-catching, animated desk clock and screensaver for retro gaming enthusiasts and makers!

---

## Prebuilt Firmware (.bin)

Don't want to set up Arduino IDE or compile from source? You can flash the prebuilt `.bin` firmware directly!

Download the latest prebuilt binaries from [Releases](https://github.com/ootake0914-dotcom/cyd-3d-maze/releases/tag/v1.0.0):

- `cyd-3d-maze-full-4mb.bin`: Full 4MB flash image. Flash to address `0x0` using `esptool.py` or any ESP Web Flasher tool:
  ```bash
  esptool.py --chip esp32 --port COMx --baud 921600 write_flash 0x0 cyd-3d-maze-full-4mb.bin
  ```

---

## Hardware Requirements

- **Board**: **ESP32-2432S028R** (Cheap Yellow Display / CYD) - **2-USB Port Version** (Micro-USB + Type-C)
- **Display**: 2.8" ST7789 SPI TFT (240x320 resolution)
- **Touch**: XPT2046 Resistive Touch Controller
- **Driver Library**: [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (V1.x)

> [!NOTE]  
> This project is specifically configured for the **2-USB port CYD board** using the **ST7789** display driver and **VSPI** pins.

---

## Features

- **Real-Time Desk Clock HUD**: Top status header showing floor & player stats, and bottom panel displaying a digital clock with interactive touch buttons (`H+`, `M+`, `00s`) to adjust time instantly.
- **3D Raycasting Engine**: High-performance pseudo-3D engine built using LovyanGFX sprite buffering for smooth rendering.
- **Dynamic Biomes & Lighting**: 3 unique dungeon themes (Red Brick Maze, Ice Crystal Cave, Ancient Ruins) with ambient torch flicker, distance shading, and vignette lighting.
- **Autonomous RPG Screensaver**: Autopilot navigation (right-hand rule + A* pathfinding), turn-based battles against 12+ monster types, boss encounters, and hack-and-slash loot drops.
- **3D D20 Dice Mechanics**: Solid 3D D20 dice rendering for treasure chests and tombstone relic recovery.
- **Relic & Grave Recovery System**: Persistent save system via ESP32 NVS (`Preferences`), carrying over legacy equipment to new generations upon death.

---

## Software & Setup

### Required Libraries
Ensure the following libraries are installed in your Arduino IDE / PlatformIO environment:

1. **LovyanGFX** (`#include <LovyanGFX.hpp>`)
2. **XPT2046_Touchscreen** (`#include <XPT2046_Touchscreen.h>`)
3. **Preferences** (ESP32 Built-in)
4. **SPI** (ESP32 Built-in)

### Arduino IDE Settings
- **Board**: `ESP32 Dev Module`
- **Flash Size**: `4MB (32Mb)`
- **Partition Scheme**: `Default 4MB with spiffs` or `Huge APP (3MB No OTA)`
- **PSRAM**: `Disabled`

---

## Touch Controls

- **Bottom Right HUD Buttons**:
  - `H+`: Increment Hours
  - `M+`: Increment Minutes
  - `00s`: Reset Seconds to 00
- **Screen Tap**:
  - Toggle between **3D Maze Desk Clock Mode** and **Loot Collection Inventory Mode**.

---

## License

This project is licensed under the MIT License - feel free to customize and enjoy it on your desk!
