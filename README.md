# ArduinoGotchi - M5StickC Plus2 Port

This is a port of the ArduinoGotchi (Tamagotchi P1 emulator) to the M5StickC Plus2 device.

## Features

- **Full Tamagotchi P1 emulation** using TamaLib core
- **M5StickC Plus2 native display** (240x135 TFT LCD with 2x scaling)
- **Built-in button controls**: BtnA (Left), BtnB (Middle), BtnPWR (Right)
- **Built-in speaker** support for Tamagotchi sounds
- **Auto-save functionality** with EEPROM persistence
- **Deep sleep support** for power management
- **Serial debugging** via USB

## Hardware Requirements

- M5StickC Plus2 device
- USB-C cable for programming and power

## Button Mapping

| M5StickC Button | Tamagotchi Function |
|----------------|-------------------|
| BtnA (Side)    | Left Button       |
| BtnB (Front)   | Middle Button     |
| BtnPWR (Power) | Right Button      |

## Build Instructions

1. Install PlatformIO in VS Code
2. Open this project folder
3. Select the `m5stickc-plus2` environment
4. Build and upload:
   ```bash
   pio run -e m5stickc-plus2 --target upload
   ```

## Usage

- **First time setup**: Press the middle button (BtnB) to configure the clock
- **Long press middle button (5s)**: Reset to egg state
- **Long press left button (5s)**: Enter deep sleep mode (10 minutes)
- **Auto-save**: Game state is saved automatically every 2 minutes

## Display

The original 128x64 Tamagotchi display is scaled 2x to fit the 240x135 M5StickC Plus2 screen with proper centering.

## Power Management

- Deep sleep mode preserves game state and extends battery life
- Wake from deep sleep automatically continues the game
- USB-C charging while playing

## Memory Usage

- **RAM**: 8.3% (27,060 bytes)
- **Flash**: 38.9% (509,461 bytes)

## Differences from Original

This M5StickC Plus2 port includes several optimizations:

- Native M5StickC Plus2 display driver (no external OLED needed)
- Built-in button and speaker integration
- Proper deep sleep implementation for M5StickC hardware
- Optimized graphics scaling and rendering

## Troubleshooting

- If the display appears corrupted, try resetting the device
- Ensure the M5StickC Plus2 is properly connected via USB-C
- Check serial monitor for debugging information

## Contributing

Feel free to submit issues or improvements for the M5StickC Plus2 port!