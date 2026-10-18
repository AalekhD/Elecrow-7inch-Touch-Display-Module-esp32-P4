### Touchscreen owner login (Arduino IDE)

This sketch uses the supplied LVGL 8.3.11 library, ESP32_Display_Panel 1.0.4,
ESP32_IO_Expander 1.1.1, and esp-lib-utils 0.2.3 in your Arduino sketchbook's
`libraries` folder. Do not replace LVGL with version 9 for this sketch.
The frequency analyzer additionally requires **arduinoFFT 2.0.4**, installed
through Arduino Library Manager (or `arduino-cli lib install arduinoFFT@2.0.4`).

1. Open `Lesson01-Print_Hello_World.ino` in Arduino IDE.
2. Select **ESP32 P4 Dev Module**, **PSRAM: Enabled**, and **Flash Size: 16MB**.
	Keep the USB and port settings that worked for the original serial sketch.
3. Keep all accompanying headers and `.cpp` files in the sketch folder.
	They configure the EK79007 MIPI-DSI panel, GPIO31 backlight, GT911 touch,
	LVGL integration, and login UI.
4. Upload and open Serial Monitor at **115200 baud**.

Expected serial message: `Touchscreen login ready.` A 3.2-second welcome splash
shows animated level bars and plays a quiet three-note chime. Continue skips
the splash and stops its sound. The first start then shows owner account
creation; subsequent starts show sign-in. There is no Bluetooth or
network dependency and only one owner account. Usernames are case-sensitive,
3-24 characters, and accept letters, digits, dots, dashes, and underscores.
Passwords accept 8-64 printable ASCII characters. Passwords are masked by
default, with an eye button to reveal them. The keyboard checkmark advances to
the next field or submits; its close key hides the keyboard until a field is
tapped again.

Credentials persist in the `device-login` NVS namespace as a single `owner`
record, containing the username, format version, salt, iteration count, and
PBKDF2-HMAC-SHA256 hash (100,000 iterations, 16-byte salt, 32-byte hash).
Plaintext passwords are not saved or logged. Each restart requires login.
Five failed logins cause a 30-second cooldown; this cooldown resets on reboot.
The signed-in launcher displays the username, board features, measured flash
and PSRAM capacity, and two applications: **Record & Play** and
**Frequency Analyzer**. Each app has Apps (back to launcher) and Log out
buttons; leaving an app stops its microphone or playback operation.

### Audio splash and voice echo

The audio wiring follows the vendor's Lesson11 recording/playback example:

| Function | Interface / GPIO |
| --- | --- |
| Microphone | PDM clock 24, data input 26, left channel |
| Speaker | I2S BCLK 22, LRCLK 21, data output 23 |
| Amplifier enable | GPIO30, LOW enables, HIGH mutes |

Recording/playback uses the core's Arduino ESP_I2S at 16 kHz, 16-bit, mono.
The startup chime is generated locally; it is not a spoken greeting.
In **Record & Play**, **Listen & repeat** records four seconds, then automatically
plays back the clip. Stop cancels the current operation. The volume slider
starts at 35 percent. Meter bars show amplitude, not a frequency spectrum;
the splash bars are decorative animation.

Recordings occupy 128,000 bytes of PSRAM and are cleared after playback,
stop, logout, or an audio error. They are never written to flash or sent over
a network. The amplifier is muted during recording to avoid speaker feedback.
The first 125 ms of microphone samples are discarded to avoid startup noise.
Playback gain is bounded and followed by silence before muting the amplifier.
Audio streams in short chunks outside the LVGL lock so the screen stays usable.

If audio initialization or streaming fails, the dashboard reports it and login
remains available; restart to retry an unavailable audio device. Ensure a speaker
is connected to the board's speaker output. Start at low volume.

Audio hardware acceptance checks:

1. Reset and confirm the welcome splash, short chime, and transition to login.
2. Sign in and confirm board features, memory sizes, and both app buttons appear.
	Open Record & Play to access the voice controls.
3. Tap Listen & repeat, speak near the microphone for four seconds, and confirm
	the meter reacts and the speaker repeats the recording.
4. Check volume, Stop during recording/playback, logout, and another recording.
5. Verify stored login credentials still work after restarting the board.

### Live frequency analyzer

Open Frequency Analyzer, tap Start, and play music near the microphone.
The five bars show the strongest separated frequency peaks, ordered by strength
(strongest first), rather than five fixed bass/treble bands. Each bar has its
current frequency in Hz and peak amplitude in dBFS. The shared scale is -80 to
0 dBFS; the bottom row shows total AC signal RMS level and a clipping warning.
The speaker stays muted during analysis; this app does not generate tones.

The detector uses ArduinoFFT, a 2,048-sample Hann-windowed FFT, DC removal,
window-gain normalization, and interpolated local maxima. At 16 kHz sampling,
each frame represents 128 ms and bins are about 7.8 Hz apart. Peak candidates
are limited to roughly 60 Hz through 7 kHz and separated by at least four bins
to avoid displaying the same tone's neighboring bins as different peaks.
Interpolation estimates frequency between bins but does not make nearby tones
separately resolvable. Harmonics can appear alongside a music note's fundamental.

Peaks below -70 dBFS or more than 28 dB below the strongest peak are omitted;
frames with RMS level below -75 dBFS show no peaks. Therefore fewer than five
bars may be populated in quiet or simple sounds. The graph is live only, with
no freeze or peak hold. Stop clears the graph. dBFS is relative to the digital
sample range, not calibrated acoustic dB SPL.

Recording and analysis share one microphone owner and cannot run concurrently.
Spectrum frames are held in RAM and copied under a short lock; no microphone
audio is saved to flash or sent to a network. Back and logout clear the results.

Analyzer hardware acceptance checks:

1. Sign in, open Frequency Analyzer, and tap Start. Play music and confirm Hz
	labels, bar heights, and signal level update continuously.
2. Optionally play a clean 440 Hz or 1 kHz test tone and verify a nearby peak.
	Room acoustics and the microphone can add other peaks or harmonics.
3. Stop and confirm the graph clears; restart and confirm it responds again.
4. Return to Apps, use Record & Play, then return to the analyzer. Confirm both
	apps remain usable and no sound leaks from the speaker during analysis.
5. Log out during analysis and verify microphone activity stops.

The host regression test exercises the same detector and ArduinoFFT source
with silence, DC, a known sine amplitude, six simultaneous tones (top five),
band-edge tones, out-of-band sound, clipping, and reset after silence. Run with
MinGW g++ from the sketch folder in PowerShell:

```powershell
$fft = "$env:USERPROFILE\Documents\Arduino\libraries\arduinoFFT\src"
$test = Join-Path $env:TEMP 'esp32p4-spectrum-test.exe'
g++ -std=c++17 -O2 -Wall -Wextra -I. "-I$fft" tests/spectrum_analyzer_test.cpp spectrum_analyzer.cpp "$fft/arduinoFFT.cpp" -o $test
if ($LASTEXITCODE -eq 0) { & $test }
```

This is an application-level local login, not a hardware security boundary.
Flash encryption and secure boot are not configured; a person with physical
access can erase or replace firmware/account storage. There is no password
recovery screen. To deliberately reset a forgotten account, run the following
once in a temporary maintenance sketch (it deletes this app's account only),
then upload the login sketch again:

```cpp
#include <Preferences.h>
void setup() {
  Preferences preferences;
  if (preferences.begin("device-login", false)) {
	 preferences.clear();
	 preferences.end();
  }
}
void loop() {}
```

Hardware acceptance checks:

1. Tap each field and enter text using the keyboard; confirm touch positions
	match the screen. Check the eye, keyboard close, and checkmark actions.
2. Confirm empty/short credentials and mismatched passwords show errors.
3. Create an owner; confirm the welcome screen shows the chosen username.
4. Log out. Confirm wrong credentials are rejected, and five failed attempts
	temporarily disable sign-in. After 30 seconds, sign in correctly.
5. Restart the board. Confirm the account persists but the sign-in screen
	returns. A normal upload must not erase flash if the account is to persist.

The LVGL port refreshes the screen in its own task. Password hashing and NVS
writes execute from the Arduino loop outside the LVGL lock so rendering can
continue. Startup errors are printed to Serial Monitor and halt initialization.

### 1, Product picture

<img width="600" height="600" alt="image" src="https://github.com/user-attachments/assets/96031111-def0-44db-aa04-17fdfcc1354b" />



### 2, Product version number

|      | Hardware | Software | Remark |
| ---- | -------- | -------- | ------ |
| 1    | V1.0     | V1.0     | old |
| 1    | V1.1     | V1.1     | old |
| 1    | V1.2     | V1.2     | latest |


**Version 1.1 Update**

The C6 module version 1.1 adds support for 4-wire SDIO communication. Additionally, the arrangement of the SDIO pins has been adjusted, with the pin mappings of the data lines D0–D3 changing from **IO14, IO15, IO16, IO17** to **IO17, IO16, IO15, IO14**. 

**Version 1.2 Update** 

Based on the V1.1 version, V1.2 further optimized the communication stability of the LoRa wireless module. At the same time, the signal pins of the wireless module socket were re-allocated: the original **IO53 and IO54** were adjusted to **IO27 and IO28** respectively, and correspondingly, **IO27 and IO28** were adjusted to **IO53 and IO54** respectively.

### 3, product information

| **Main Chip-ESP32-P4NRW32**                  |                                                              |
| -------------------------------------------- | ------------------------------------------------------------ |
| CPU/SoC                                      | **ESP32-P4**RISC-V 32-bit dual-core processor for HP systems, running at up to 400 MHz;RISC-V 32-bit single-core processor for LP systems, running at up to 40 MHz |
| System Memory                                | 768 KB L2MEM (HP) 32 KB SRAM (LP) 8 KB TCM 32 MB PSRAM       |
| Memory                                       | 128 KB ROM (HP) 16 KB ROM (LP) 16 MB Flash                   |
| Development Environment                      | ESP-IDF、Arduino IDE                                   |
| **Screen**                                   |                                                              |
| Size                                         | 7.0 inch                                                     |
| Resolution                                   | 1024*600                                                     |
| Display Panel                                | IPS Panel                                                    |
| Touch Panel                                  | Capacitive Touch, Single/5-point Touch                       |
| Viewing Angle                                | 178°                                                         |
| Brightness                                   | 400 cd/m²(Typ.)                                              |
| Color Depth                                  | 16.7M (8-bit)                                                |
| **Wireless Communication - Onboard Antenna** |                                                              |
| WiFi                                         | Support 2.4GHz(Wi-Fi6), 802.11a/b/g/n                        |
| Bluetooth                                    | Support Bluetooth 5.3 and BLE                                |
| Other                                        | Zigbee, LoRa, nRF2401, Matter, Thread (**Optional**)         |
| **Interface/Function**                       |                                                              |
| Interface                                    | USB2.0, UART, I2C, GPIO female headers, SD card holder, battery socket, speaker jack, camera header, module female headers, etc. |
| Function                                     | Audio amplifier, battery charge management, USB to UART, dual microphones, dual speakers etc. |
| **Button/LED Indicator**                     |                                                              |
| Reset Button                                 | Yes, press to reset the device                               |
| Boot Button                                  | Yes, press and hold the power button to burn the program     |
| Power Button                                 | Switch On/Off                                                |
| PWR                                          | Device power on/off indication                               |
| CHG                                          | Lithium battery charging status, Low battery state           |
| **Other**                                    |                                                              |
| Installation method                          | All around mounting holes(M3 3.2mm), embedded, shell assembly |
| Operating temperature                        | -20~70 °C                                                    |
| Storage temperature                          | -30~80 °C                                                    |
| Power Input                                  | 5V/2A, USB or UART terminal                                  |
| Active Area                                  | 155mm*87mm                                                   |
| Dimensions                                   | 180*105mm                                                    |



### Functional description of the product's internal interfaces:

| Pin Name | Description                                                  | Connector Type |
| :------- | :----------------------------------------------------------- | :------------- |
| SPK      | Output audio signals to connect to speakers. The main board comes with a power amplifier chip circuit. | PH2.0-2P       |
| PWR      | Power LED.                                                   |                |
| RST      | Reset button. Press it to reset the system.                  |                |
| boot     |                                                              |                |
| UART1    | Builds communication between Logic modules, including the serial communication module and the print module. | HY2.0-4P       |
| I2C      | Builds communication between Logic modules, including the serial communication module and the print module. | HY2.0-4P       |
| UART3-IN | Input power supply and serial communication functionality    | XH2.54-4P      |
| BAT      | Connect the lithium battery. (with battery charging circuit) | PH2.0-2P       |





### 4, Use the driver module

| Name | dependency library |
| ---- | ------------------ |
| LVGL | lvgl/lvgl@9.2   |

### 5,Quick Start




##### ESP-IDF starts

1.Right-click on an empty space in the project folder and select "Open with VS Code" to open the project.
![4](https://github.com/user-attachments/assets/a842ad62-ed8b-49c0-bfda-ee39102da467)



2.In the IDF plug-in, select the port, then compile and flash

<img width="1363" height="721" alt="image" src="https://github.com/user-attachments/assets/784b78c0-65a8-4a3e-b98d-73f2f5d04e35" />



### 6,Folder structure.
|--3D file： Contains 3D model files (.stp) for the hardware. These files can be used for visualization, enclosure design, or integration into CAD software.

|--Datasheet: Includes datasheets for components used in the project, providing detailed specifications, electrical characteristics, and pin configurations.

|--Eagle_SCH&PCB: Contains **Eagle CAD** schematic (`.sch`) and PCB layout (`.brd`) files. These are used for circuit design and PCB manufacturing.

|--example: Provides example code and projects to demonstrate how to use the hardware and libraries. These examples help users get started quickly.

|--factory_firmware: Stores pre-compiled factory firmware that can be directly flashed onto the device. This ensures the device runs the default functionality.

|--factory_sourcecode:  Contains the source code for the factory firmware, allowing users to modify and rebuild the firmware as needed.

|--libraries: Includes necessary libraries required for compiling and running the project. These libraries provide drivers and additional functionalities for the hardware.


### 7,Pin definition

#### ESP32-P4 7 inch and IPS Display Wiring Pins:
**DSI = Display Serial Interface**is a high-speed, low-power display interface standard defined by the MIPI Alliance, most commonly used in smartphones, tablets, Raspberry Pi devices, and embedded Linux systems.

<img width="723" height="693" alt="image" src="https://github.com/user-attachments/assets/f74e408c-d66c-44f9-a88b-de0172dc0e28" />


DSI Pin connection

DSI_DATAN0--IO40

DSI_DATAN0--IO39

DSI_DATAN0--IO36

DSI_DATAN0--IO35

DSI_CLKN--IO37

DSI_CLKP--IO38

DSI_REXT--IO34

#### ESP32-P4 and Touch Driver Wiring：

i2c address: 0x5D/0x14.(The INT pin level during reset of the GT911 touch chip determines the device address.)

INT Low Level(0x5D);

INT High Level(0x14).

<img width="641" height="571" alt="image" src="https://github.com/user-attachments/assets/f5a27de9-679b-4199-b611-78e549d06f64" />


Pin connection

I2C1_SCL(IO46)

I2C1_SDA(IO45)

INT_TP(IO42)

RESET_TP(IO40)

#### ESP32-P4 and wireless module wiring pins：

Output voltage: 3.3V Output current: 1A max. Use: The power supply communicates with the wireless module.

<img width="1008" height="553" alt="image" src="https://github.com/user-attachments/assets/afea7188-35c8-4ef2-92ff-d0387e378274" />


Pin connection


#define RADIO_GPIO_CLK 8

#define RADIO_GPIO_MISO 7

#define RADIO_GPIO_MOSI 6

#ifdef CONFIG_BSP_SX1262_ENABLED

#define SX1262_GPIO_BUSY 9

#define SX1262_GPIO_IRQ 53

#define SX1262_GPIO_NRST 54

#define SX1262_GPIO_NSS 10

#ifdef CONFIG_BSP_NRF2401_ENABLED

#define NRF24_GPIO_IRQ 9

#define NRF24_GPIO_CE 53

#define NRF24_GPIO_CS 54

ESP32-P4 and Audio out：

<img width="913" height="789" alt="image" src="https://github.com/user-attachments/assets/7f9e0056-6d34-4d90-a899-7a642a0af6e1" />


Pin connection


#define AUDIO_GPIO_LRCLK    21   // GPIO pin number for LRCLK (Left-Right Clock)

#define AUDIO_GPIO_BCLK     22   // GPIO pin number for BCLK (Bit Clock)

#define AUDIO_GPIO_SDATA    23   // GPIO pin number for SDATA (Serial Data)

#define AUDIO_GPIO_CTRL     30   // GPIO pin number for audio amplifier control

