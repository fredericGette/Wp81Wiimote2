# Wp81Wiimote2

An other demonstration of the usage of a Wiimote with Windows Phone 8.1  
This time, the communication is done at the HCI level.  

See also my previous projects on the same topic:  
- [Wp81Wiimote](https://github.com/fredericGette/Wp81Wiimote)
- [Wp81WiimoteDriver](https://github.com/fredericGette/Wp81WiimoteDriver)

> [!WARNING]
> Work In Progress.

> [!WARNING]
> Currently only compatible with Nokia Lumia 520.  

## Usage

TODO  

Currently, when you start the executable it does the following steps:  
- Stop the communication between the Windows Bluetooth stack and the Qualcomm Bluetooth stack.  
- Reset the Bluetooth controller.  
- Start *Inquiry* in a loop to detect 4 Bluetooth devices (you have to press simultaneously the buttons 1 and 2 of the Wiimote). It exits the loop as soon as 4 devices are detected.  
- Establish a Bluetooth connection with the Bluetooth devices.
- Open and configure a *HID control* channel with the Bluetooth devices.
- Open and configure a *HID interrupt* channel with the Bluetooth devices.
- Set the LED of the Wiimotes (the LED# corresponds to the order of the detection of the Wiimote).
- Read the inputs of the Wiimotes (buttons and accelerometer)

To stop the executable, press Ctrl+C, then disable Bluetooth on the Windows Phone.  

[Example](Capture01.PNG)
The program displays the inputs received from the Wiimotes.  
A new line is displayed each time a button is pressed or released.  
The format of a line is the following:  
```
MMMM msg/s 1:<>v^+21BA-H ±XXX ±YYY ±ZZZ 2:<>v^+21BA-H ±XXX ±YYY ±ZZZ 3:<>v^+21BA-H ±XXX ±YYY ±ZZZ 4:<>v^+21BA-H ±XXX ±YYY ±ZZZ 
```
Where:
- `MMMM msg/s` is the number of ACL messages received by second.
- `<` is D-Pad left.
- `>` is D-Pad right.
- `v` is D-Pad down.
- `^` is D-Pad up.
- `+` is Plus button.
- `2` is Two button.
- `1` is One button.
- `B` is B button.
- `A` is A button.
- `-` is Minus button.
- `H` is Home button.
- XXX is the value of the acceleration in the X axis.
- YYY is the value of the acceleration in the Y axis.
- ZZZ is the value of the acceleration in the Z axis.
  
## Deployment

- [Install a telnet server on the phone](https://github.com/fredericGette/wp81documentation/tree/main/telnetOverUsb#readme), in order to run the application.  
- Manually copy the executable from the root of this GitHub repository to the shared folder of the phone.
> [!NOTE]
> When you connect your phone with a USB cable, this folder is visible in the Explorer of your computer. And in the phone, this folder is mounted in `C:\Data\USERS\Public\Documents`  

### Installation of the kernel drivers "wp81controldevice.sys" and "wp81hcifilter.sys"

> [!NOTE]
> They are currently exactly the same drivers than those used by [wp81btmon](https://github.com/fredericGette/wp81btmon).  

#### Legacy driver "wp81controldevice.sys"

This driver allows communication between the *wp8wiimote2* executable and the filter driver.

- Manually copy the .sys from the driver folder of this GitHub repository to the shared folder of the phone.
- Install the driver:
```
sc create wp81controldevice type= kernel binPath= C:\Data\USERS\Public\Documents\wp81controldevice.sys
```
- Start the driver:
```
sc start wp81controldevice
```

> [!NOTE]
> You have to start the control driver after every reboot of the phone.  

#### Filter driver "wp81hcifilter.sys"

This driver filters the IOCtls exchanged between the Windows Bluetooth stack and the Qualcomm Bluetooth stack.

- Manually copy the .sys from the driver folder of this GitHub repository to the shared folder of the phone.
- Install the driver:
```
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter"
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter" /V Description /T REG_SZ /D "WP81 HCI Filter driver"
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter" /V DisplayName /T REG_SZ /D "wp81HCIFilter"
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter" /V ErrorControl /T REG_DWORD /D 1
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter" /V Start /T REG_DWORD /D 3
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter" /V Type /T REG_DWORD /D 1
reg ADD "HKLM\SYSTEM\CurrentControlSet\Services\wp81hcifilter" /V ImagePath /T REG_EXPAND_SZ  /D  "\??\C:\Data\USERS\Public\Documents\wp81hcifilter.sys"
reg ADD "HKLM\System\CurrentControlSet\Enum\SystemBusQc\SMD_BT\4&315a27b&0&4097" /V LowerFilters /T REG_MULTI_SZ /D "wp81hcifilter"
```
- Start the driver:
```
powertool -reboot
```

> [!NOTE]
> The filter driver automatically starts when the Bluetooth stack boots (ie when the phone boots and Bluetooth is enabled).

> [!NOTE]
> You can inspect the logs of the drivers using [wp81debug](https://github.com/fredericGette/wp81debug)  
> `wp81debug.exe dbgprint | findstr /C:"Control!" /C:"HCI!"`
> And parse them using [hciexplorer](https://github.com/fredericGette/hciexplorer) in *command line* version.

## References

Wiimote specifications are available at [Wiimote - WiiBrew](https://wiibrew.org/wiki/Wiimote)
