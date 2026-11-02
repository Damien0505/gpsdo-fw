## Alternative firmware for the BH3SAP GPSDO

This is an alternative firmware for the BH3SAP GPSDO sold on various platforms.

![Image of the GPSDO running this firmware](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gpsdo.jpg?raw=true)

### Usage

Power on the device with GPS antenna connected. Wait a long while for the PPB to reach close to zero. When the PPB is stabilized close to zero, the GPSDO is considered locked (padlock icon) and it will automatically save the current PWM value in flash.

This PWM value will then be used on the next boot as a startup value.

The current PWM value can also be manually saved by going to `PWM` menu and press the encoder twice (a message will be shown after the first press).


The [original manual](https://raw.githubusercontent.com/fredzo/gpsdo-fw/b1f1766ef8beb795172a6fa325e783569361913e/doc/gpsdo-documentation.pdf) for the device talks about running the device without a GPS antenna after calibration, but I would advice against that since the oscillator seems sensitive to both ambient temperature, vibrations and orientation. Best results will be had when the GPS antenna is connected at all times.

### Menu system

This alternative firmware has a 2 level menu system. Moving from one menu item to another is done by turning the rotary encoder, and entering a given menu (when applicable) is done by pressing the encoder.

Here is the menu tree :
- `Main Screen`: displays the number of detected satellites, the PPB value and the current time read from GPS frame
- `Date Screen`: displays the number of detected satellites, the PPB value and the current date read from GPS frame
- `Date-time Screen`: displays the number of detected satellites, the PPB value and the current time or date (changes every 5 seconds)
- `Trend Menu`: displays the number of detected satellites, the current PPB value and a graphical representation of the PPB trend over time
  - `Trend Main Screen`: same as above, press the encoder to enter navigation mode (scroll trend data over time by rotating the encoder)
  - `Auto vertical scale`: press to set the auto-vertical-scale status (when set to `ON`, vertical scale will be automatically adjusted to match the displayed trend values)
  - `Auto horizontal scale`: press to set the auto-horizontal-scale status (when set to `ON`, horizontal scale will be automatically adjusted to show available data)
  - `Vertical scale`: shows the current vertical scale (value of the max PPB in the graph), if auto-vertical-scale is off, press the encoder to set the vertical scale value
  - `Horizontal scale`: shows the current horizontal scale (number of seconds represented by a point in the trend graph), if auto-horizontal-scale is off, press the encoder to set the horizontal scale value
  - `Exit`: press to exit the Trend sub-menu
- `PPB Menu`: displays current PPB value
  - `Mean value`: the mean PPB value (running average over 128 seconds)
  - `Instant value`: last calculated PPB value
  - `Frequency`: the measured current MCU frequency (based on the number ot ticks counted between two GPS PPS pulses, should be around 70 000 000 for 70 MHz)
  - `Error`: the last measured frequency error (in Hz)
  - `Correction`: the last correction applied to PWM value
  - `PWM`: the current PWM value
  - `OCXO model`: press to set the OCXO model installed on your GPSDO to ISOTEMP or OX256B (this will adjust warmup time and default PWM value)
  - `Warm-up duration`: press to set the warmup duration is seconds (time to wait after boot to let the OCXO warm-up before starting PWM correction)
  - `Algorithm selection`: press to select the algorithm used to adjust PWM value ; there are 3 available algorithms :
      - `Eric-H` (default): Based on ppm value rather than frequency error (uses 128s rolling average rather than instant values)
      - `Dankar`: Original code from Dankar using square value of instant frequency error as PWM correction
      - `Fredzo`: Same logic as dankar's, but with faster correction when frequency error is >= 2
  - `Correction factor`: press to adjust the responsiveness of the correction algorithm :
      - For `Eric-H` algorithm, the default correction factor is 300, increasing it will slow down the PWM adjustment
      - For `Dankar` and `Fredzo` algorithms, the default correction factor is 10, a value bellow 10 will slow down PWM adjustment and a value above 10 will speed it up
  - `Millis`: the gap in milliseconds between GPS PPS reference and MCU calculated PPS (should be 0)
  - `PWM auto save`: press to set the PWM auto-save status (when set to `ON`, PWM value will automatically be saved the first time PPB mean value reaches 0)
  - `PPS auto resync`: press to set the PWM auto-sync status (when set to `ON`, MCU Controlled PPS output will automatically be resynced to GPS PPS Output the first time PPB mean value reaches 0)
  - `PPB Lock Threshold`: press to set the PPB threshold value above which GPSDO is considered locked
  - `Exit`: press to exit the PPB sub-menu
- `PWM Screen`: the current PWM value, press the encoder twice to save this value to flash memory
- `GPS Menu`: displays the number of detected satellites and the current GPS time
  - `Time`: the current GPS time
  - `Latitude`: the GPS detected latitude (format: ddmm(.)mmmm)
  - `Longitude`: the GPS detected longitude (format: ddmm(.)mmmm)
  - `Latitude decimal`: the GPS detected latitude in decimal format
  - `Longitude decimal`: the GPS detected longitude in decimal format
  - `Locator`: the IARU Locator for the current GPS position
  - `Altitude`: the GPS detected altitude (in meters)
  - `Geoid`: the Geoid-to-ellipsoid separation (in meters)
  - `Sat. #`: the numner of satellites
  - `HDOP`: the current Horizontal Dilution Of Precision value
  - `Baudrate`(__*don't mess with this unless you know what you are doing !*__): set the GPS UART communication baudrate (for GPSDO equipped with ATGM336H GPS modules, changing this will also send a command to change the GPS module baudrate accordingly *BUT* ATGM336H modules installed in the GPSDO have been reported to have a weak battery and don't retain this setting for a very long time... passed this time the module will return to default 9600 bauds, breaking the communication with the bluepill (see [Troubleshooting section](https://github.com/fredzo/gpsdo-fw/blob/main/README.md#no-time-on-the-display)))
  - `Comm`: set the baudrate of the external serial link on PA2/PA3, taking effect at next boot (see [GPS UART Passthrough](#gps-uart-passthrough)). It defaults to 115200, whatever the GPS baudrate
  - `Time Zone offset`: set the number of hours (-14/+14) to shift the displayed time from UTC to match local time
  - `Date Format`: set the date format (either `dd/mm/yy` (default value), `mm/dd/yy`, `yy/mm/dd`, `dd.mm.yy` or `yy-mm-dd`)
  - `Model`: displays the detected GPS module model, press to manually set the GPS module model
  - `Frame`: displays to first characters of the last frame received from the GPS module
  - `Exit`: press to exit the GPS sub-menu
- `Uptime Screen` : displays the number of seconds elapsed since last boot
- `GGA Frames Screen`: the number of GGA frames received from the GPS module since last boot
- `Contrast Screen` : press the encoder to change the contrast value by turning the rotary encoder ; press again to exit (when editing contrast value, `?` is displayed after contrast)
- `PPS Menu`: This menu is dedicated to the [MCU controlled PPS output](https://github.com/fredzo/gpsdo-fw?tab=readme-ov-file#mcu-controlled-pps-output) ; it shows the number of times the MCU PPS Output has been synced (top left) and the shift with GPS PPS output in clock cycles (bottom)
  - `Shift`: the shift between MCU PPS output and GPS PPS output in clock cycles
  - `Shift milliseconds`: the shift between MCU PPS output and GPS PPS output in milliseconds
  - `Sync Count`: the number of times the MCU PPS output has been re-synced to the GPS PPS output
  - `Sync.`: press to set the synchronization activation status (when set to `ON` the MCU PPS Output will be resynced if it deviates from the GPS PPS output of more than `threshold` clock cycles during more than `delay` seconds)
  - `Delay`: press to set the MCU PPS output synchronisation delay (in seconds)
  - `Threshold`: press to set the MCU PPS output synchronisation threshold (in clock cycles)
  - `Force Sync`: press to force the MCU Controlled PPS output to be synched with the GPS PPS output
  - `Exit`: press to exit the PPS sub-menu
- `Version Screen` : shows the current firmware version

#### Main screen
![Main Screen](https://github.com/fredzo/gpsdo-fw/blob/main/doc/main-screen.jpg?raw=true)
The top left corner of the `Main Scren` contains an indicator for PPS pulses. Next to that is the current number of satellites used by the GPS module. To the right of that is the current measured PPB error.
Bottom line is the current UTC time from GPS module.

#### Trend screen
![Trend Screen](https://github.com/fredzo/gpsdo-fw/blob/main/doc/trend-screen.jpg?raw=true)
The top left corner of the `Trend Scren` contains an indicator for PPS pulses (using default characters from the LCD driver, custom characters beeing used for graphical trend display).
Next to that is the current number of satellites used by the GPS module. To the right of that is the current measured PPB error.
Bottom line is a graphical representation of the PPB trend over time.

Trend menu gives access to trend navigation, and scale settings:
![Trend Menu](https://github.com/fredzo/gpsdo-fw/blob/main/doc/trend-menu.png?raw=true)

#### Boot Screen
After boot, the GPSDO will automatically display the last used screen between `Main Screen`, `Date Screen`, `Date Time Screen` and `Trend Screen`.

#### GPSDO Lock
GPSDO is considered locked when the mean PPB value (running average over 128 seconds) is above the `PPB Lock Threshold` setting in `PPB` menu.

The GPSDO locked status can be monitored with the padlock icon on the main screen:
![GPSDO Lock](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gpsdo-lock.png?raw=true)

#### PPB Menu
![PPB Menu](https://github.com/fredzo/gpsdo-fw/blob/main/doc/ppb-menu.png?raw=true)

#### GPS Menu
![GPS Menu](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gps-menu.png?raw=true)


### Flashing the firmware

To flash this alternative firmware in your GPSDO you will need :
- A [ST-Link V2 dongle](https://github.com/fredzo/gpsdo-fw/blob/main/doc/st-link-v2.png?raw=true)
- The [STM32CubeProgrammer software](https://www.st.com/en/development-tools/stm32cubeprog.html).

First you need to open it to access the bluepill board inside it.

To do so, you only need to remove the 4 screews at the top left and right sides of the front and bacck pannels of your GPSDO:
![Open Case](https://github.com/fredzo/gpsdo-fw/blob/main/doc/open-case.jpg?raw=true)

You now have access to the bluepill board, but you need to bend the 4 pins of the programmation header to the top so that you can plug Dupont wires to that header:
![Bend pins](https://github.com/fredzo/gpsdo-fw/blob/main/doc/st-link-connection.jpg?raw=true)

You can now launch the [STM32CubeProgrammer software](https://www.st.com/en/development-tools/stm32cubeprog.html) click `Connect`, click `Open File`, select the `gpsdo.bin` file downloaded in the [Release section](https://github.com/fredzo/gpsdo-fw/releases) and hit `Download` in STM32CubeProgrammer:
![Bend pins](https://github.com/fredzo/gpsdo-fw/blob/main/doc/stm32-cube-programmer.png?raw=true)

For video instructions, you can check [Tony Albus's BH3SAP GPSDO review at 7'20''](https://www.youtube.com/watch?v=FxD5QzaOiZ4&t=440s): 
[![Tony Albus's BH3SAP GPSDO review](https://img.youtube.com/vi/FxD5QzaOiZ4/0.jpg)](https://www.youtube.com/watch?v=FxD5QzaOiZ4&t=440s)

### Troubleshooting

#### No time on the display
If the current time is not displayed on the main screen, it most likely is because the UART communication between the bluepill board and the GPS module is broken.

To fix that, go the the GPS menu and set the baudrate to the default 9600 value.

It's generally not a good idea to change the baudrate of the GPS module since ATGM336H modules installed in the GPSDO have been reported to have a weak battery. They don't retain the baudrate setting for more than 10 to 20 minutes. Passed this time the module will return to default 9600 bauds and break the communication with the bluepill.

#### Flickering screen / impossible to get the GPSDO to lock
Later versions of the GPSDO come with a different OCXO than the original ISOTEMP model. This new OCXO, a Bowei OX256B-T-LU-V-10M, has a weaker output signal than the ISOTEMP. This weaker signal is not strong enough to drive the bluepill OSCin input through the onboard quartz like the ISOTEMP does.

The good news is that it should not need to do so if the wiring of the GPSDO had been done correctly!...

So far, all produced GPSDO units have been reported to have a wrong connection bellow the bluepill board, sending the OCXO output to PIN6 (OSCout) rather than PIN5 (OSCin). This can easily be fixed by resoldering the red wire to the other pin of the quartz (on C13 side) like shown bellow.

Wrong pin:
![GPS Passthrough](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gpsdo-wrong-pin.jpg?raw=true)


Fix:
![GPS Passthrough](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gpsdo-fix-pin.jpg?raw=true)

This wrong wiring does not seem to have an impact on GPSDOs with ISOTEMP OCXO installed, that might explain why it has not been detected at design time.

### Hardware Extensions

#### MCU controlled PPS Output
An MCU controlled PPS Output has been added on pin PB1. It will output a 100 ms pulse every second, based on the clock of the Bluepill board.
This output, compared to the PPS output of the original design, is available after boot, even if the GPS module is not yet locked or if the GPS antenna is not connected.

If `Sync` mode is on, the MCU controlled PPS output will be synchronized to the GPS PPS output as soon as the GPS module gets a fix. The deviation between the two PPS outputs will then be monitored and the MCU controlled PPS output will be resynced to the GPS PPS output as soon as it deviates from more than `threshold` clock cycles during more than `delay` seconds.

If the `PPS auto resync` is set to `ON` in the `PPB` menu, the MCU controlled PPS Output will also be automatically resynced to the GPS PPS Output the first time PPB mean value reaches 0.

The dedicated `PPS` menu allows monitoring the deviation between the MCU controlled PPS output and the GPS PPS output and setting the synchronization parameters:
  - The `Shift` entry shows the shift between MCU PPS output and GPS PPS output in clock cycles
  - The `Shift milliseconds` entry shows the shift between MCU PPS output and GPS PPS output in milliseconds
  - The `Sync Count` entry shows the number of times the MCU PPS output has been re-synced to the GPS PPS output
  - The `Sync.` entry sets the synchronization activation status (when set to `ON` the MCU PPS Output will be resynced if it deviates from the GPS PPS output of more than `threshold` clock cycles during more than `delay` seconds)
  - The `Delay` entry sets the MCU PPS output synchronisation delay (in seconds)
  - The `Threshold` entry sets the MCU PPS output synchronisation threshold (in clock cycles)
  - The `Force Sync` entry can be used to force the MCU Controlled PPS output to be synched with the GPS PPS output

![PPS Out](https://github.com/fredzo/gpsdo-fw/blob/main/doc/pps-output.jpg?raw=true)

#### GPS UART Passthrough

A GPS UART passthrough has been added on UART2: pins PA2 (TX) and PA3 (RX) can be used to communicate with the GPS module. This is bidirectional, so the GPS can be used by a computer or configured via manufacturer software.

This link now has its own baudrate, 115200 by default, set in the `GPS > Comm` menu: use this for your serial adapter, and not the GPS baudrate. For heavy traffic such as u-center, set it to the same value as the GPS baudrate and reboot. It also carries monitoring and control sentences, see [the protocol](doc/PROTOCOL.md).

![GPS Passthrough](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gps-passthrough.jpg?raw=true)

These two pins and ground can be routed to an external header on the backside of the device, and then plugging in a serial to USB converter when needed.

#### GPS Lock and GPSDO Lock Outputs

Pins `PA0` and `PA1` can be used to drive leds showing GPS Lock and GPSDO Lock status.

`PA0` will be pulled low as soon as the GPS module is locked and `PA1` will be pulled low when PPB mean value goes above the set threshold.

![GPS Lock and GPSDO Lock outputs](https://github.com/fredzo/gpsdo-fw/blob/main/doc/gps-gpsdo-lock-output.png?raw=true)

### Theory of operation

Most of this is based on reverse engineering the circuit, and which pins certain things were connected to. The base of the device is a [Bluepill board](https://os.mbed.com/users/hudakz/code/STM32F103C8T6_Hello/) with a STM32F103C8T6 MCU. The oscillator is an Isotemp [143-141 OCXO](https://github.com/fredzo/gpsdo-fw/blob/main/doc/OCXO-143-Series.pdf?raw=true) which can be trimmed with a DC voltage. The output of the oscillator is buffered by a OPA692 and has an output resistor of 51 Ohms. The GPS differs between models, some have an [ATGM336H](https://github.com/fredzo/gpsdo-fw/blob/main/doc/2501061039_ZHONGKEWEI-ATGM336H-5N31_C90770.pdf?raw=true) and others a u-blox Neo-6M (perhaps not authentic) ; both are communicating with Bluepill board trhough UART3 in [NMEA protocol](https://github.com/fredzo/gpsdo-fw/blob/main/doc/NMEA_Reference_Manual-Rev2.1-Dec07.pdf?raw=true). The 1PPS output from the GPS is not buffered, so it has very high output impedande.

* The OCXO is connected to the OSC_IN on the Bluepill board. The MCU is configured to be driven by an oscillator, so the crystal mounted on the Bluepill does nothing.
* The PPS is connected to the CH1 input on TIM1.
* The VCO is controlled via PWM from CH2 of TIM1. The PWM signal is sent to a couple of low pass filters, giving a DC voltage.

The MCU will PLL the clock up to 70MHz and then TIM1 is setup to count the internal 70MHz clock, while being gated by the PPS pulse from the GPS module. This means that it continually counts how many cycles on the clock passes between each PPS pulse. This is then used to adjust the VCO.

The VCO is simply adjusted by the error detected between two pulses. If 70000001 clocks are counted, the VCO voltage will drop a bit and so on. This is really simple, but due to the small adjustments, it will average out over time and it should work out since the counter is always running. If we are running at 70 000 000.01 we will get one more clock every 100 seconds, which will then cause a small adjustment (smallest adjustment possible).

It's fairly slow to reach a steady state, and it can probably easily be sped up with a better algorithm.

The displayed PPB error is a long running average. It counts the number of clock ticks over 128 seconds and compares it to the expected (128*SYSCLK)

### Building

#### Linux / OSX

Clone the repo, update submodules and do the cmake. (Or just download a release) You should not need any other dependencies than arm-none-eabi-gcc. The bluepill can be flashed in multiple ways, check the documentation for it for information. Included is a openocd configuration for connecting to the device via SWD using a JLink adapter.

#### Windows

Developing / building on Windows can be achieved with Visual Studio Code and MSYS2:
* [Download and install VSCode](https://code.visualstudio.com/download)
* [Download and install MSYS2](https://www.msys2.org/)
* In a MSYS2 shell, run `pacman -S mingw-w64-x86_64-arm-none-eabi-gcc` to install arm-none-eabi-gcc toolchain and `pacman -S mingw-w64-x86_64-ninja` to install Ninja build system
* Add `C:\msys64\mingw64\bin` to your Windows PATH
* Clone the repo, update submodules, and open the project in VSCode
* Install CMake VSCode extension
* Use CMake pane in VSCode to build the project or use `ninja` in a command line
* Run `arm-none-eabi-objcopy -O binary build/Release/gpsdo.elf build/Release/gpsdo.bin` in VSCode terminal to convert elf file in bin file

### USB

It would be nice to have NMEA output over USB, and the Bluepill dev board in the GPSDO does have a USB connector. It's however difficult to use since it requires a PLLCLK of 48MHz. But since we use 10MHz as input instead of 8MHz this can't be achieved. It should be possible to run the HSI to the PLL and then run the USB off of that. Then run the HSE directly to the peripherals. But then the timers would be running at 10MHz and that would cause the PWM to be slower, and the measurements to have lower resolution.

### GPS passthrough

Instead of using the USB, I have added GPS passthrough on an unused UART. Pins PA2 (TX) and PA3 (RX) can be used to communicate with the GPS module. This is bidirectional, so the GPS can be used by a computer or configured via manufacturer software.

I ended up routing these two pins and ground to an external header on the backside of the device, and then plugging in a serial to USB converter when needed.
