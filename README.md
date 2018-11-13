<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->
**Table of Contents**

- [fpga-cfg FPGA Manager interface driver](#fpga-cfg-fpga-manager-interface-driver)
- [Userspace interface for loading FPGA devices](#userspace-interface-for-loading-fpga-devices)
    - [SPI configuration interface](#spi-configuration-interface)
    - [FPP configuration interface](#fpp-configuration-interface)
    - [Configuration interface files](#configuration-interface-files)
    - [Configuration description](#configuration-description)
    - [Example for configuration via FPP](#example-for-configuration-via-fpp)
    - [Example for Partial Reconfiguration (PR)](#example-for-partial-reconfiguration-pr)
    - [Examples for configuration via SPI and SPI/CvP](#examples-for-configuration-via-spi-and-spicvp)
      - [SPI](#spi)
      - [SPI/CvP](#spicvp)
- [FPGA Devices, FPGA Configuration Adapter Hardware and Drivers](#fpga-devices-fpga-configuration-adapter-hardware-and-drivers)
  - [Required low-level FPGA manager and platform drivers](#required-low-level-fpga-manager-and-platform-drivers)
    - [Programming the FT232H Adapter EEPROM with custom USB VID/PID](#programming-the-ft232h-adapter-eeprom-with-custom-usb-vidpid)
    - [Kernel Config options for enabling the drivers](#kernel-config-options-for-enabling-the-drivers)
    - [Driver mainlining status](#driver-mainlining-status)
    - [FT232H based FPGA configuration adapter drivers for mainlining](#ft232h-based-fpga-configuration-adapter-drivers-for-mainlining)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# fpga-cfg FPGA Manager interface driver

The *fpga-cfg* driver provides sysfs based configuration interface for loading various FPGA devices. The driver uses available low-level Linux FPGA managers.

# Userspace interface for loading FPGA devices

You can see all available FPGA managers in /sys/class/fpga_manager directory. With multible FPGA managers detected you should see multiple FPGA managers in /sys/class/fpga_manager/fpga\*/ directories, e.g.:

```
# head /sys/class/fpga_manager/fpga*/name
==> /sys/class/fpga_manager/fpga0/name <==
ftdi-fpp-fpga-mgr single 1-4.1:1.0

==> /sys/class/fpga_manager/fpga1/name <==
altera-ps-spi spi0.1
```

The *fpga-cfg* driver uses available Linux FPGA managers and creates configuration interfaces for each of them. The configuration interfaces are located in subdirectories under /sys/kernel/debug/fpga_cfg/\*.  

### SPI configuration interface
For SPI configuration interfaces the SPI slave name of the PS-SPI device is encoded in the FPGA manager name (trailing string "spi0.1" in example above). The created directory name for the configuration interface has "spi\_" prefix and ends with the trailing string from SPI FPGA manager name, so it looks like "spi_spiN.M", where N is the SPI bus number and M is the chip select number assigned to the PS-SPI slave. E.g. *spi0.1* is the slave name and *spi_spi0.1* is the directory for configuration interface:

```
# head /sys/class/fpga_manager/fpga1/name
altera-ps-spi spi0.1
```

For configuration via SPI/USB-SPI following sysfs interface files exist:
```
# tree -l /sys/kernel/debug/fpga_cfg
/sys/kernel/debug/fpga_cfg
`-- spi_spi0.1
    |-- cvp -> /sys/devices/platform/fpga-cfg.0/spi_spi0.1/cvp
    |   |-- image
    |   `-- meta
    |-- debug -> /sys/devices/platform/fpga-cfg.0/spi_spi0.1/debug
    |-- history
    |-- load -> /sys/devices/platform/fpga-cfg.0/spi_spi0.1/load
    |-- ready -> /sys/devices/platform/fpga-cfg.0/spi_spi0.1/ready
    |-- spi -> /sys/devices/platform/fpga-cfg.0/spi_spi0.1/spi
    |   |-- image
    |   `-- meta
    `-- status -> /sys/devices/platform/fpga-cfg.0/spi_spi0.1/status

3 directories, 9 files
```

### FPP configuration interface
For FPP interfaces the configuration interface directory in named differently, it contains "fpp\_" prefix and the board location string (which is encoded in the FPGA manager name).

```
# head /sys/class/fpga_manager/fpga0/name
ftdi-fpp-fpga-mgr single 1-4.1:1.0
```
The name of each FPP FPGA manager contains the trailing unique string ("1-4.1:1.0" in example above) identifying the USB Bus/Port interface the FPP FPGA interface is connected to. Additionally, the name contains location specifier string "single", "left" or "right". For RevA. PRAX boards this string is always "single". For RevB. PRAX boards this string is either "left" or "right", depending on board detection via CPLD.
Thus, the name for configuration interface directory for RevA. boards is "fpp_single.N", where N is the assigned board number unique for each connected board.  

For configuration via FPP on PRAX RevA. board the sysfs interface files look like:

```
# tree -l /sys/kernel/debug/fpga_cfg
/sys/kernel/debug/fpga_cfg
└── fpp_single.0
    ├── cvp -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/cvp
    │   ├── image
    │   └── meta
    ├── debug -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/debug
    ├── fpp -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/fpp
    │   ├── image
    │   └── meta
    ├── history
    ├── load -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/load
    ├── pr -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/pr
    │   ├── image0
    │   └── meta0
    ├── ready -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/ready
    └── status -> /sys/devices/platform/fpga-cfg.0/fpp_single.0/status

4 directories, 11 files
```

### Configuration interface files
| File | Meaning |
| :--- | :--- |
|*debug* | file for enabling more debug info in dmesg log. Write: 1 - enable, 0 - disable|
|*history* | file for reading FPGA configuration history|
|*load* | interface for writing a FPGA configuration description|
|*ready* | interface for waiting for Partial-Reconfiguration completion. Use epoll_wait() and pread(). 1 - success, 0 - error|
|*status* | interface for waiting for FPP/SPI/CvP configuration completion. Use epoll_wait() and pread(). 1 - success, 0 - error|
|*cvp/[image, meta]* | files for reading last CvP configuration image/meta-data|
|*fpp/[image, meta]* | files for reading last FPP FPGA configuration image/meta-data|
|*pr/[imageN, metaN]* | files for reading last FPGA Partial Reconfiguration image/meta-data|
|*spi/[image, meta]* | files for reading last SPI FPGA configuration image/meta-data|

See configuration status polling example with usage of epoll_wait()/pread() [here.](examples/fpga-cfg-epoll.cpp)

### Configuration description
 To configure an FPGA the user writes configuration description to the *load* file. A configuration description is a set of key-value pairs surrounded by curly braces, e.g.:

\{  
&nbsp;&nbsp;&nbsp;&nbsp;fpga-pcie-bus-nr = "06:00.0";  
&nbsp;&nbsp;&nbsp;&nbsp;fpga-type	= "Stratix-V";  
&nbsp;&nbsp;&nbsp;&nbsp;spi-lsb-first   = "1";  
&nbsp;&nbsp;&nbsp;&nbsp;spi-image	= "/lib/firmware/apc_spi.bit";  
&nbsp;&nbsp;&nbsp;&nbsp;cvp-image	= "/lib/firmware/apc_cvp.rbf";  
\}  

Depending on the configuration type (SPI, FPP, optional CvP or PR) different key-value pairs are required.
Following key options are defined (parsed in the driver):

| Key | Value |
| :--- | :--- |
| *fpga-type*        | a string describing this FPGA. This string will appear in the header of "history" file containing the configuration history |
| *fpga-pcie-bus-nr* | a string encoding the PCIe BUS:DEVICE.FUNCTION number of the FPGA device for CvP/PR configurations |
| *fpp-usb-dev-id* | Linux USB Bus/Port interface string of the associated FPP FPGA manager. e.g.: *fpp-usb-dev-id = "1-4.1:1.0"*|
| *spi-lsb-first* | When set to "1", it specifies that before sending the data to FPGA the bit order of the SPI bitstream file should be reversed. If this option is missing or if it is set to "0", the bit order won't be changed |
| *spi-image* | specifies full path to a bitstream file used for initial FPGA configuration. The image files should be placed in /lib/firmware directory or in subdirectories under /lib/firmware. **Image locations outside of the /lib/firmware directory are not supported**|
| *spi-image-meta* | full path to a file containing the meta information for a SPI FPGA image (only used for logging in configuration history)|
| *cvp-image* | full path to an RBF file used to load the FPGA via PCIe (CvP configuration)|
| *cvp-image-meta* | specifies full path to a file containing the meta information for a CvP FPGA image (only used for logging in configuration history)|
| *fpp-image* | full path to a bitstream file used for FPGA configuration via FPP |
| *fpp-image-meta* | full path to a file containing the meta information for an FPP FPGA image (only used for logging in configuration history)|
| *part-reconf-image* | full path to an RBF file used for partial reconfiguration|
| *part-reconf-image-meta* | full path to a file containing the meta information for partial reconfiguration (only used for logging in configuration history)|
| *mfd-driver* | a string specifying the FPGA MFD driver to be bound to the PCIe FPGA device after an FPP or CvP configuration|
| *mfd-driver-param* | a string containing module parameters for loading the driver specified by *mfd-driver* option. This module parameter string must contain all module parameters separated by comma, e.g.: *mfd-driver-param = "mfd_bar_nr=7,mfd_bar_offs=0x12000057,i2c_irq_nr=10"*|

### Example for configuration via FPP
After connecting a PRAX RevA. board the needed drivers should be loaded automatically. Check them by running lsmod:
```
# lsmod
Module                  Size  Used by
ftdi_fifo_fpp          16384  1
ft232h_intf            24576  2
altera_cvp             16384  0
fpga_cfg               36864  0
```

In the kernel log you can see driver output like:
```
...
[57243.321895] usb 2-1.2: new high-speed USB device number 10 using ehci-pci
[57243.403698] usb 2-1.2: New USB device found, idVendor=0403, idProduct=7148
[57243.403707] usb 2-1.2: New USB device strings: Mfr=1, Product=2, SerialNumber=3
[57243.403712] usb 2-1.2: Product: FT232H
[57243.403716] usb 2-1.2: Manufacturer: FTDI
[57243.403720] usb 2-1.2: SerialNumber: 1002
[57243.426358] EEPROM: 00000000: 11 00 03 04 48 71 00 09 e0 00 08 00 33 00 a0 0a  ....Hq......3...
[57243.426364] EEPROM: 00000010: aa 0e b8 0a 00 00 00 00 00 00 00 00 88 00 56 00  ..............V.
[57243.426367] EEPROM: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426370] EEPROM: 00000030: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426372] EEPROM: 00000040: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426375] EEPROM: 00000050: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426378] EEPROM: 00000060: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426381] EEPROM: 00000070: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426383] EEPROM: 00000080: 00 00 00 00 00 00 00 00 00 00 48 00 00 00 00 00  ..........H.....
[57243.426386] EEPROM: 00000090: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426389] EEPROM: 000000a0: 0a 03 46 00 54 00 44 00 49 00 0e 03 46 00 54 00  ..F.T.D.I...F.T.
[57243.426392] EEPROM: 000000b0: 32 00 33 00 32 00 48 00 0a 03 31 00 30 00 30 00  2.3.2.H...1.0.0.
[57243.426395] EEPROM: 000000c0: 32 00 02 03 00 00 00 00 00 00 00 00 00 00 00 00  2...............
[57243.426397] EEPROM: 000000d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426400] EEPROM: 000000e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[57243.426403] EEPROM: 000000f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 22 40  .............."@
[57243.426687] ft232h-intf 2-1.2:1.0: using 2 CBUS pins
[57243.743075] ftdi-fifo-fpp-mgr ftdi-fifo-fpp-mgr.0: Board Rev 1, Addr Sel 0
[57243.743276] fpga_manager fpga0: ftdi-fpp-fpga-mgr single 2-1.2:1.0 registered
[57243.743286] LOOKING for [ ftdi-fpp-fpga-mgr ] in 'ftdi-fpp-fpga-mgr single 2-1.2:1.0'
[57243.743518] fpga-cfg fpga-cfg.0: probing, fpga mgr in pdata ftdi-fpp-fpga-mgr single 2-1.2:1.0
[57243.743573] fpga-cfg fpga-cfg.0: FPP board address: 'single'
[57243.743582] fpga-cfg fpga-cfg.0: FPP manager usb id: '2-1.2:1.0'
[57243.743734] fpga-cfg fpga-cfg.0: Using FPGA manager 'ftdi-fpp-fpga-mgr single 2-1.2:1.0'
```
Now write the FPP configuration description file:

```
$ cat /lib/firmware/config-desc-fpp
{
	fpga-type	= "Arria-10";
	fpga-pcie-bus-nr = "9:00.0";
	fpp-usb-dev-id	= "2-1.2:1.0";
	fpp-image	= "/lib/firmware/PRAX_fpp_x8.rbf";
	fpp-image-meta	= "/lib/firmware/PRAX_fpp_x8-meta.xml";
	mfd-driver	= "fpga_mfd";
}

$ sudo dd bs=16k if=/lib/firmware/config-desc-fpp of=/sys/kernel/debug/fpga_cfg/fpp_single.0/load
0+1 records in
0+1 records out
218 bytes copied, 3.00228 s, 0.1 kB/s

$ sudo dd bs=16k if=/lib/firmware/config-desc-fpp of=/sys/kernel/debug/fpga_cfg/fpp_single.0/load
0+1 records in
0+1 records out
218 bytes copied, 3.03923 s, 0.1 kB/s
```

An application can open the sysfs *load* file and write() the FPGA configuration description to the file descriptor. write() syscall terminates without errors when the FPGA configuration succeeds. If something goes wrong, the write() syscall returns a negative error code, e.g. -EINVAL if the description is invalid, or -ENODEV if the PCIe bus number for FPGA device is wrong. The 'dd' command in example above terminated without errors, both FPGA configurations succeeded. In case of successful configuration you will read "1" as status value via sysfs *status* or *ready* interface. The *history* file contains the log with all successful configurations, e.g.:

```
$ sudo cat /sys/kernel/debug/fpga_cfg/fpp_single.0/status
1

$ sudo tail -f /sys/kernel/debug/fpga_cfg/fpp_single.0/history
=== Config Log for Arria-10 device @ 9:00.0 ===
[111220.144575] load 1: /lib/firmware/PRAX_fpp_x8.rbf	meta: /lib/firmware/PRAX_fpp_x8-meta.xml
[111228.887850] load 2: /lib/firmware/PRAX_fpp_x8.rbf	meta: /lib/firmware/PRAX_fpp_x8-meta.xml
```

**For Partial-Reconfigurations the FPGA configuration status is returned in *ready* file (not in *status* file).** An application can poll() on the *status* or *ready* file descriptor to wait for completion of the FPGA configuration. An example using epoll_wait()/pread() is [here.](examples/fpga-cfg-epoll.cpp)

### Example for Partial Reconfiguration (PR)
When Arria-10 FPGA was configured with an image containing Altera Partial Reconfiguration core (PR IP), it is possible to register an FPGA manager for partial reconfiguration. This must be done in the PCIe driver for Altera PCIe FPGA device by calling the *alt_pr_register()* function while driver probing. When removing the PCIe FPGA driver the manager must be unregistered by calling *alt_pr_unregister()* function. The *fpga-mfd* driver was already extended with an option to register/unregister the partial reconfiguration FPGA manager. The driver registers this manager by default (pr_enable=1 module option). The FPGA image should provide the PR IP register access via BAR 4, offset 0xCFB0 (see PR_IP_BAR_NR and PR_IP_REG_OFFS macros in kernel-modules/include/linux/fpga-mfd.h). When these prerequisites are met, load the *fpga-mfd* driver and try the partial reconfiguration as follows:

```
# cat /lib/firmware/config-desc-arria10-pr-a
{
	fpga-type		= "Arria-10";
	fpga-pcie-bus-nr	= "42:00.0";
	part-reconf-image	= "/lib/firmware/impl_a.pr_region.rbf";
	part-reconf-image-meta	= "/lib/firmware/pr-a-meta.xml";
}

# dd bs=16k if=/lib/firmware/config-desc-arria10-pr-a of=/sys/kernel/debug/fpga_cfg/fpp_single.0/load
0+1 records in
0+1 records out

# cat /sys/kernel/debug/fpga_cfg/fpp_single.0/ready
1

# tail -f /sys/kernel/debug/fpga_cfg/fpp_single.0/history
=== Config Log for Arria-10 device @ 42:00.0 ===
[10940.375177] load 1: /lib/firmware/output_file_20170317.periph.core.rbf     meta: /lib/firmware/cvp-meta.xml
[10951.338801] load 2: /lib/firmware/impl_a.pr_region.rbf     meta: /lib/firmware/pr-a-meta.xml
```

### Examples for configuration via SPI and SPI/CvP
#### SPI
This example shows a single Xilinx Spartan-6 configuration over SPI. When all required modules are loaded, we have two configuration interfaces: *spi_spi0.1* and *spi_spi1.2*.
The first is for Altera Stratix-V FPGA, the second is for Xilinx Spartan-6 (you can read the FPGA manager names under */sys/class/fpga_manager/fpga\*/name*):

```
# lsmod
Module                  Size  Used by
altera_cvp             16384  0
xilinx_spi             16384  1
altera_ps_spi          16384  1
fpga_cfg               32768  0

# ls /sys/kernel/debug/fpga_cfg
spi_spi0.1/ spi_spi1.2/

# head /sys/class/fpga_manager/fpga*/name
==> /sys/class/fpga_manager/fpga0/name <==
altera-ps-spi spi0.1

==> /sys/class/fpga_manager/fpga1/name <==
xlnx-slave-spi spi1.2
```

Now write the configuration description to the matching interface file:
```
# cat /lib/firmware/config-desc-xlnx-spi
{
	fpga-type	= "Xilinx-S6";
	spi-image	= "/lib/firmware/afe_spi.bit";
	spi-image-meta	= "/lib/firmware/afe_spi-meta.xml";
}

# dd bs=16k if=/lib/firmware/config-desc-xlnx-spi of=/sys/kernel/debug/fpga_cfg/spi_spi1.2/load
0+1 records in
0+1 records out

# cat /sys/kernel/debug/fpga_cfg/spi_spi1.2/status
1

# cat /sys/kernel/debug/fpga_cfg/spi_spi1.2/history
=== Config Log for Xilinx-S6 device @  ===
[  556.385423] load 1: /lib/firmware/afe_spi.bit	meta: /lib/firmware/afe_spi-meta.xml

```

#### SPI/CvP
This example shows a two-stage configuration over SPI and CvP with subsequent
loading of specified FPGA driver module with custom module options:

```
# cat /lib/firmware/config-desc-spi-cvp
{
	fpga-pcie-bus-nr = "06:00.0";
	fpga-type	= "Stratix-V";
	spi-lsb-first	= "1";
	spi-image	= "/lib/firmware/apc_spi.bit";
	cvp-image	= "/lib/firmware/apc_cvp.rbf";
	spi-image-meta	= "/lib/firmware/spi-meta.xml";
	cvp-image-meta	= "/lib/firmware/cvp-meta.xml";
	mfd-driver	= "fpga_mfd";
	mfd-driver-param = "mfd_bar_nr=7,mfd_bar_offs=0x0,i2c_enable=0,msgdma_enable=0,msgdma2_enable=0,altera_10g_enable=0";
}

# dd bs=16k if=/lib/firmware/config-desc-spi-cvp of=/sys/kernel/debug/fpga_cfg/spi_spi0.1/load
0+1 records in
0+1 records out

# cat /sys/kernel/debug/fpga_cfg/spi_spi0.1/status
1

# tail -f /sys/kernel/debug/fpga_cfg/spi_spi0.1/history
=== Config Log for Stratix-V device @ 06:00.0 ===
[ 1861.203664] load 1: /lib/firmware/apc_spi.bit	meta: /lib/firmware/spi-meta.xml
[ 1862.669482] load 2: /lib/firmware/apc_cvp.rbf	meta: /lib/firmware/cvp-meta.xml
```

Here is the corresponding dmesg Log:

```
...
[ 1861.159575] fpga_manager fpga0: writing apc_spi.bit to altera-ps-spi spi0.1
[ 1861.221063] pciehp 0000:03:03.0:pcie204: Slot(3): Link Down
[ 1861.226667] pciehp 0000:03:03.0:pcie204: Slot(3): Link Up
[ 1861.226687] pci 0000:06:00.0: PME# disabled
[ 1861.359018] pci 0000:06:00.0: [1172:e001] type 00 class 0xff0000
[ 1861.359075] pci 0000:06:00.0: reg 0x10: [mem 0x00000000-0x0001ffff]
[ 1861.359099] pci 0000:06:00.0: reg 0x18: [mem 0x00000000-0x0fffffff]
[ 1861.359130] pci 0000:06:00.0: reg 0x20: [mem 0x00000000-0xffffffff 64bit pref]
[ 1861.359170] pci 0000:06:00.0: calling pci_fixup_ide_bases+0x0/0x78
[ 1861.359472] pci 0000:06:00.0: BAR 4: assigned [mem 0x1000000000-0x10ffffffff 64bit pref]
[ 1861.367664] pci 0000:06:00.0: BAR 2: assigned [mem 0xb0000000-0xbfffffff]
[ 1861.374784] pci 0000:06:00.0: BAR 0: assigned [mem 0xa0100000-0xa011ffff]
[ 1861.381739] pcieport 0000:03:03.0: PCI bridge to [bus 06]
[ 1861.387160] pcieport 0000:03:03.0:   bridge window [mem 0xa0100000-0xc01fffff]
[ 1861.394543] pcieport 0000:03:03.0:   bridge window [mem 0x1000000000-0x10ffffffff 64bit pref]
[ 1861.403395] altera-cvp 0000:06:00.0: assign IRQ: got 47
[ 1861.403530] fpga_manager fpga1: Altera CvP FPGA Manager @0000:06:00.0 registered
[ 1861.411166] fpga_manager fpga1: writing apc_cvp.rbf to Altera CvP FPGA Manager @0000:06:00.0
[ 1862.669535] fpga_manager fpga1: fpga_mgr_unregister Altera CvP FPGA Manager @0000:06:00.0
[ 1862.693836] fpga_mfd 0000:06:00.0: assign IRQ: got 47
[ 1862.693939] fpga_mfd 0000:06:00.0: enabling device (0400 -> 0402)
[ 1862.700471] fpga_mfd 0000:06:00.0: enabling bus mastering
[ 1862.700569] fpga_mfd 0000:06:00.0: PCI Express bandwidth of 16GT/s available
[ 1862.707789] fpga_mfd 0000:06:00.0: (Speed:5.0GT/s, Width: x4)
[ 1862.713814] fpga_mfd 0000:06:00.0: successfully probed FPGA #0 using 1 MSI-X vectors
```

# FPGA Devices, FPGA Configuration Adapter Hardware and Drivers
Currently we use two FT232H based FPGA configuration adapter types. The first adapter type (USB-SPI) utilizes FT232H in MPSEE mode to connect ADBUS SPI/GPIO pins to Stratix-V PS-SPI interface. Another adapter type (USB-FIFO-FPP) connects FT232H ADBUS (in FT245 FIFO mode) and two ACBUS GPIOs to the CPLD, the CPLD is connected to the Arria-10 FPP interface. Both FPGAs are connected to the host via PCIe.

## Required low-level FPGA manager and platform drivers
Figure 1 shows a summary with simplified device connection diagram and low-level and configuration interface drivers relationship. The *fpga-cfg* driver provides sysfs based configuration interface for custom FTDI FT232H based adapters for initial configuration of Stratix-V and Altera-10 FPGAs via PS-SPI or FPP interfaces.

```text
                +-------------+
                |             |
                |  STRATIX V  |PS-SPI         FT245 FIFO & GPIO
                |             +-----+    +-------------------+
                |  on Board 1 |     +    +                   |
                |             |                         +----+---+
                |  PCIe       |   ADBUS&ACBUS           |  CPLD  |
                +---+---------+ Connection Options      +----+---+
                    ^          (MPSSE or FIFO&GPIO)          |
                    +                  +              +------+-------+
               altera-cvp  +-----------+----------+   |     FPP      |
                           |        FT232H        |   |              |
                           |     0x0403:0x7148    |   |   ARRIA 10   |
                           |     0x0403:0x7149    |   |              |
                           +----------+-----------+   |  on Board 2  |
                                      |               |              |
                          +-----------+------------+  |        PCIe  |
                  creates | ft232h-intf (USB misc) |  +----------+---+
                 platform |     bulk/ctrl xfer     |             ^
                  devices |ACBUS GPIO Ctrl (0x7148)|             |
                   below  |MPSSE GPIO Ctrl (0x7149)|             |
                          +-------+-------+--------+             |
                                  |       |                      |
                     for     +----+       +------+    for        |
                  PID 0x7149 |                   | PID 0x7148    |
                   +---------+--------+  +-------+---------+     |
                   |  ftdi-mpsse-spi  |  |                 |     |
                   | altera-ps-spi in |  |ftdi-fifo-fpp-mgr|     |
                   |   spi_board_info |  |                 |     |
                   +---------+--------+  +--------+--------+     |
                             ^                    ^              |
                  Drivers:   |                    |              |
                             +                    |              |
                MPSSE SPI master(spi-ftdi-mpsse)  |              +--------------+
                             ^                    |              |              |
                             |                    +              +              +
                       altera-ps-spi        ftdi-fifo-fpp    altera-cvp    altera-pr-ip
                        FPGA Manager         FPGA Manager   FPGA Manager   FPGA Manager
                             ^                    ^              ^              ^
                             |                    |              |              |
                             +---------------+    +    +---------+              |
                                          fpga-cfg interface                    |
                                        driver (uses fpga-mgr) +----------------+

               Figure 1: FT232H, FPGA Devices and Drivers Relationship
```


The *fpga-cfg* driver uses various low-level drivers for Linux FPGA Manager framework (fpga-mgr). The required low-level fpga-mgr drivers can be found [here](http://git.denx.de/?p=linux-denx/linux-denx-agust.git;a=tree;f=drivers/fpga;h=d14d8f8dd663845b5fbe9b4ae8be69156f9e6054;hb=refs/heads/fpga "low-level fpga-mgr drivers"). We use following FPGA manager drivers: altera-cvp.c (CvP configuration), altera-pr-ip-core.c (Partial Reconfiguration), altera-ps-spi.c (PS-SPI configuration), ftdi-fifo-fpp.c (FPP configuration).

Additional changes [1](http://git.denx.de/?p=linux-denx/linux-denx-agust.git;a=commit;h=b67a7990b487b458dd223046269bf51309387454 "patch1") to the FPGA manager framework are required for *fpga-cfg* driver to build and work as expected.  
To enable auto-loading *fpga-cfg* module the kernel should contain following patch: [2](http://git.denx.de/?p=linux-denx/linux-denx-agust.git;a=commit;h=2758da9cc84bb8e80be914850609d6ee657000f1 "patch2").

Some remarks about the drivers in above diagram

 * the *ft232h-intf* driver binds to FT232H USB devices and creates either a FIFO-FPP (*ftdi-fifo-fpp-mgr*) or a MPSSE (*ftdi-mpsse-spi*) platform device for each FT232H device, depending on the USB PID in the FTDI EEPROM on the FT232H device. *ft232h-intf* driver provides common functions used in drivers for created platform devices. It also registers GPIO controllers for either ACBUS-GPIO or MPSSE-GPIO pin control

 * *ftdi-fifo-fpp* FPGA Manager driver attaches to *ftdi-fifo-fpp-mgr* platform devices and registers one FPP FPGA manager for each platform device. *ftdi-fifo-fpp* driver requires some control/status GPIOs. On the FPP adapter hardware these GPIOs are some ACBUS pins. The GPIO lookup tables for needed ACBUS pins must be setup in the *ft232h-intf* driver before *ftdi-fifo-fpp-mgr* platform device registration, so the
*ftdi-fifo-fpp* driver can find appropriate GPIO descriptors for ACBUS pins when probing

 * *spi-ftdi-mpsse* SPI master platform driver connects to the *ftdi-mpsse-spi* platform devices and registers an SPI master controller for FT232H USB-SPI bus. In our adapter hardware this bus has only one SPI slave device - Altera PS-SPI proxy. Thus, the *spi-ftdi-mpsse* platform driver instantiates one new *altera-ps-spi* SPI slave device using *struct spi_board_info*, so that the *altera-ps-spi* driver can probe and connect to it. *altera-ps-spi* driver requires some control/status GPIOs. On the adapter hardware these GPIOs are some ADBUS pins controlled in MPSSE mode. Therefore, the driver must additionally register appropriate GPIO controller and install suitable GPIO lookup tables for *altera-ps-spi* SPI slave device. The *spi-ftdi-mpsse* driver gets spi_board_info and GPIO setup information via platform data for *spi-ftdi-mpsse* device

 * *fpga-cfg* driver takes configuration description via sysfs interface and performs initial FPGA configuration with a periphery image. Afterwards the FPGA PCIe device appears on the PCI bus, *altera-cvp* driver is loaded and CvP FPGA manager is registered with the FPGA manager framework. Subsequently the *fpga-cfg* driver uses the CvP FPGA manager to configure the FPGA with the core image and unbinds the *altera-cvp* driver after successful CvP configuration (so that a device-specific driver can be attached to the configured PCIe FPGA device). Then, the FPGA device-specific driver (specified by *mfd-driver* option in configuration description) is bound to the PCIe FPGA device, e.g. *fpga-mfd* driver. When partial FPGA re-configuration is required, this driver can register an FPGA manager for partial re-configuration using the alt_pr_register() function (exported by *altera-pr-ip-core* driver)

### Programming the FT232H Adapter EEPROM with custom USB VID/PID
The FTDI EEPROMs on FT232H based adapters for FPGA configuration must be updated with our custom USB PIDs, otherwise the drivers for FPP or USB-SPI configuration won't work. For programming the FTDI EEPROM we use custom *loadFpp* tool with options "--pid-fpp" or "--pid-spi".

To program the USB VID/PID 0x0403:0x7149 for USB-SPI adapter ensure that an USB-SPI adapter is connected and run:

```
$ sudo loadFpp --pid-spi --write-config"
```

To program the USB VID/PID 0x0403:0x7148 for FPP adapters ensure that all FPP adapters are connected and run:
```
$ sudo loadFpp --print-devices
```
When multiple FT232H devices for FPP are used you must use "--devNr" option to select the needed device, e.g.:
```
$ sudo loadFpp --devNr 0 --pid-fpp --write-config"
$ sudo loadFpp --devNr 1 --pid-fpp --write-config"
```

### Kernel Config options for enabling the drivers
| Driver | Kconfig option |
| :--- | :--- |
|*altera-cvp* | CONFIG_FPGA_MGR_ALTERA_CVP|
|*altera-pr-ip-core*  | CONFIG_ALTERA_PR_IP_CORE|
|*altera-ps-spi* | CONFIG_FPGA_MGR_ALTERA_PS_SPI|
|*ft232h-intf* | CONFIG_USB_FT232H_INTF|
|*ftdi-fifo-fpp* |  CONFIG_FPGA_MGR_FTDI_FIFO_FPP|
|*ftdi-mpsse-spi* | CONFIG_SPI_FTDI_MPSSE|
|*xlnx-slave-spi* | CONFIG_FPGA_MGR_XILINX_SPI|

### Driver mainlining status
| In Mainline | Not in Mainline yet |
| :--- | :--- |
|*altera-cvp*|*ft232h-intf*|
|*altera-pr-ip-core*|*spi-ftdi-mpsse*|
|*altera-ps-spi*|*ftdi-fifo-fpp*|
|*xlnx-slave-spi*|*fpga-cfg* (can this driver be acepted in mainline tree?) |

### FT232H based FPGA configuration adapter drivers for mainlining

The low-level FPGA manager drivers for our FT232H based adapter variants have been reworked (since v4.15 kernel) to prepare them for inclusion in mainline kernel tree. In reworked drivers we use different custom PIDs for our two adapter types (USB-SPI, USB-FIFO-FPP). USB PID 0x7148 is reserved for FIFO-FPP adapter, USB PID 0x7149 is reserved for USB-SPI adapter. The common USB transfer related functions (bulk, control and FTDI mode setting code) are in the FT232H interface driver *ft232h-intf* under drivers/usb/misc/ft232h-intf.c. When probing for USB VID/PID 0x0403:0x7148, this driver registers ACBUS GPIO controller, GPIO lookup tables for FIFO FPP device and creates a platform device *ftdi-fifo-fpp-mgr* for attaching the low-level FPGA manager driver for FIFO FPP interface. The attached FPGA manager driver *ftdi-fifo-fpp* resides in drivers/fpga/ftdi-fifo-fpp.c. When probing for USB VID/PID 0x0403:0x7149, the *ft232h-intf* driver registers MPSSE GPIO controller, GPIO lookup tables for *altera-ps-spi* control/status GPIOs and creates platform device for attaching MPSSE SPI master controller driver. The SPI master controller platform driver registers MPSSE SPI bus with SPI slave device from *spi_board_info* struct in its platform data (in our case PS-SPI slave device for attaching *altera-ps-spi* driver). The location of this custom FTDI SPI master controller driver is drivers/spi/spi-ftdi-mpsse.c.

