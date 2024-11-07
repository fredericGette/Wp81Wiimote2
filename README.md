# Wp81Wiimote2

Other demonstration of the usage of a Wiimote with Windows Phone 8.1  
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
- Stop the communication between the Windows Bluetooth stack and the Qualcom Bluetooth stack.  
- Reset the Bluetooth controller.  
- Start *Inquiry* in a loop to detect a Bluetooth device (you have to press simultaneously the buttons 1+2 of the Wiimote). It exits the loop as soon as a device is detected.  
- Establish a Bluetooth connection with the Bluetooth device.
- Open and configure a *HID control* channel with the Bluetooth device.
- Open and configure a *HID interrupt* channel with the Bluetooth device.
- Set the first LED of the Wiimote

To stop the executable, press Ctrl+C, then disable Bluetooth on the Windows Phone.  

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
