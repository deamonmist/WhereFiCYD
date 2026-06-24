WhereFi CYD
by deamonmist

#Overview
The purpose of this hand-held device is to be able to locate a WiFi, BLE, or Classic Bluetooth signal in physical space. It is built around the 3.5” CYD, scans for nearby networks and devices, view signal strength, security type, and MAC addresses. You can click on a specific to track it in real time using an animated signal visualizer.

#Features
•	Three radio modes — WiFi (channels 1–13), BLE, and Classic Bluetooth, switchable at any time via tab buttons
•	10-second boot scan with animated progress bar and live device count
•	Top 5 list showing device name, MAC address, signal strength, channel, and security type (OPEN / WEP / WPA / WPA2 / WPA3)
•	Signal tracker with a gaussian bell curve visualizer that responds to signal strength and direction
•	RSSI history graph showing signal strength over time
•	Battery percentage indicator with color-coded icon (green / yellow / red)
•	Debug touch overlay — long press anywhere to show touch zone mapping
•	Resistive touchscreen navigation throughout

#Hardware
This project is designed to run entirely off of a single piece of hardware: the so called CYD, Cheap Yellow Display. This uses the 3.5” model which sports an ESP32-WROOM-32E, which features WiFI and BT/BLE.
Please note: Not all CYDs are the same. Different sized screens often have different ESP32 chipsets. Please refer to the Pin Map below which is used by the python code to function. Different CYD’s may require adjustments accordingly.
Here are the details on the one I used:

##Component	Details
• Board	ESP32-32E 3.5″ LCD Display (320×480, Resistance Touch)
• Display driver	ST7796
• Touch controller	XPT2046
• ESP32 module	ESP32-WROOM-32E (WiFi + BT/BLE)
• Battery	LiPo via JST connector on board
• Charging	Recommended: HW-373 or TP4056 module (not included on board)

#Pin Map
These are hardcoded to match the ESP32-32E board silkscreen:
Function..........GPIO
TFT MOSI..........GPIO 13
TFT MISO..........GPIO 12
TFT SCK...........GPIO 14
TFT CS............GPIO 15
TFT DC............GPIO 2
TFT Backlight.....GPIO 27
Touch CS..........GPIO 33
Touch IRQ.........GPIO 36
Battery ADC.......GPIO 34

#Project Structure
wifi-scanner/
├── platformio.ini       # Build config, display driver, pin definitions
├── partitions.csv       # Custom partition table (required for BT + BLE + WiFi)
└── src/
    └── main.cpp         # Full application source

#Installation
##Requirements
•	VS Code with the PlatformIO IDE extension
•	Git for Windows (required for library install)
•	A USB-C data cable (charge-only cables will not work)

##Steps
•	Clone or download this repository and open the folder in VS Code. PlatformIO should detect platformio.ini and activate automatically.
•	On first build, PlatformIO will download the required libraries automatically and they include:
o	bodmer/TFT_eSPI @ ^2.5.43
o	XPT2046_Touchscreen
o	ESP32 BLE Arduino (bundled with framework)
o	BluetoothSerial (bundled with framework)
•	Connect your CYD via USB-C. Open Device Manager and confirm it appears under Ports (COM & LPT). If not, you may need a CP210x or CH340 driver depending on your board’s USB chip.
•	In the PlatformIO sidebar (alien head icon), click Build and wait for SUCCESS. Then click Upload.
•	Open the Serial Monitor at 115200 baud to confirm boot. The display should light up and begin scanning immediately.

##Grey or Blank Screen
The pin configuration in platformio.ini is set for the specific ESP32-32E board listed above. If you have a different CYD variant, you may need to adjust the following build flags:

-DTFT_MOSI=13
-DTFT_MISO=12
-DTFT_SCLK=14
-DTFT_CS=15
-DTFT_DC=2
-DTFT_BL=27

Common alternative CYD variants use MOSI=23, MISO=19, SCK=18 — check your board’s silkscreen.

#Usage
##Boot
The device scans all available channels for 8–10 seconds on startup, then displays the top 5 results ranked by signal strength.

##Switching Modes
Tap the WiFi, BLE, or Classic buttons in the header to switch radio modes at any time. A fresh scan runs automatically when switching.

##Tracking a Device
Tap any item in the list to open the signal tracker. The bell curve visualization responds to signal strength — taller and centered means stronger signal pointed toward the source. Walk around slowly to find the direction of strongest signal. A [ CENTERED ] label appears when the bell is both tall and centered.

##Navigating Back
Tap the Back button in the tracker header to return to the list.

##Debug Overlay
Long press anywhere on screen for approximately 1.2 seconds to toggle a touch zone overlay. Useful for diagnosing tap accuracy on different screen variants.
Battery
The battery percentage in the top-right reads from GPIO 34 via the board’s built-in voltage divider. The board’s JST connector accepts a single-cell LiPo. Charging requires an external module such as a TP4056 or HW-373 wired between the charger input and the LiPo — the board itself has no charging circuit.

#Notes on Bluetooth
##BLE
BLE scans passively and picks up any device that is advertising — phones, fitness trackers, smart home devices, AirTags, and similar. Most modern devices advertise continuously, so BLE results tend to be rich and lively.

##Classic Bluetooth
Classic Bluetooth only discovers devices that are explicitly in discoverable or pairing mode. Most phones and speakers are not discoverable by default. Put the target device into pairing mode before scanning for best results.

##License
GNU General Public License v3.0

#About
Built by deamonmist as part of his maker and tech channel. Learning in public, one project at a time.
YouTube: youtube.com/@deamonmist
GitHub: github.com/Deamonmist

#Assembly
##Parts:

•	1x 3.5” CYD
•	1x 3000mAh LiPo Battery
•	12x PM3*20 Plastic Screws
•	4x M3*10 Hexagonal Nuts
•	12x M3 Hexagonal Nuts
•	1x 3D Printed SwitchPlate.stl
•	1x 3D Printed BottomPlate.stl
•	1x 3D Printed BatteryPlate.stl

##Installing a Switch
A switch is optional. Installing it along the positive cable between the battery and the power into the board will give you a useful on/off switch.

##Charging the Battery
As it stands, the battery must be removed and charged separately. There are many solutions to this; but because this is designed to be a one board project, it is not included in this iteration.
 
#Assembly
Download WhereFi_CYD_ReadMe.docx for picture details.
 
#Note on Use of AI
I designed the 3D print files from scratch. 3D modeling is something I’ve done for a long time, and I use no AI to assist me when I work on this part of my projects. 
While I have studied and practices coding on and off for most of my life, I am mediocre at best. I relied heavily on an LLM to complete the code. If you have any real skill, this will be obvious to you. I don’t claim to have made it from scratch, but I am pleased with the outcome.

