# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an hardware module emulating the Sharp CE-140F disk drive with an SD-Card, using an STM32 microcontroller to 
interface with a Sharp Pocket Computer (PC) through its proprietary 11-pin interface.

What I think the Sharp-PC was missing the most (like any retro computers from the 80's) is a quick and convenient way to store and retrieve programs to and from its memory. Entering a BASIC listing by hand has always been the most frustrating task, in the early days of home computing. Yes, a software-emulated cassette interface does exist already, but disk storage is way much better! 

The Sharp CE140-F Disk Driver was a rare and expensive extension, compatible with several of the Sharp-PC models:

PC-1280, PC-1280(Jap), PC-1285, PC-1403, PC-1403H, PC-1425, PC-1425(Jap), PC-1460, PC-1460(Jap), PC-1470U, PC-1475, PC-1475(Jap), EL-5500III, PC-E500, PC-E500(Jap), PC-E500S, PC-E550, PC-E650, PC-U6000. As well as the PC-1360, PC-1360K, PC-1365 and PC-1365K after 1986. 

See also: [pocket.free.fr](http://pocket.free.fr/html/sharp/ce-140f_e.html?fbclid=IwAR3U31Bk95W-eKc_V7EStOoxpfgk6qsZ3UOhHXM6cb7czv4wsmH_SPU4H-8)

This interface is a cheap alternative to effectively have file management on the Sharp PC. Here's a link to a demo on the early prototype, using the LOAD and SAVE commands for example:

[LOAD or SAVE demo](https://youtu.be/YH5hvxJgKFs)

I started with this prototype (Apr '22) on the Nucleo L053R8 board:

<img src="https://user-images.githubusercontent.com/659557/180180992-6d9be30f-607c-4927-bcbf-eb3c7a3ea95e.jpg" width=100% height=100%>

which I later also moved to a proto-board version:

<img src="https://user-images.githubusercontent.com/659557/197160147-e2b9a441-d164-4adc-97ca-d35f92db984d.jpg" width=100% height=100%>

Then, I abandoned the L053R8, in favour of a more compact (and powerful) L432KC board. The prototype is like below:

<img src="https://user-images.githubusercontent.com/659557/202128859-ecb3f3f4-8933-494c-a026-fa893b7ce3a1.jpg" width=100% height=100%>

## The PCB version...
An Indiegogo campaign is still in progress, funding the manufacturing of a PCB board to hold the Nucleo, SD card, and interface circuitry:

[Sharp_ce140f_emul on Indiegogo](https://www.indiegogo.com/projects/sharp-ce-140f-disk-drive-emulator/x/32084495#/)

Early samples of the assembled PCB boards are looking like this:

![20230228_182704](https://user-images.githubusercontent.com/659557/222767238-69398b13-cc70-4f3a-9172-2753334ed6cd.jpg)

while testing is ongoing...

## Hardware interface notes!

Interface schematics (find complete KiCad project in the repo):

![image](https://user-images.githubusercontent.com/659557/213223743-f2838cbe-25bf-4762-9deb-3f7cca15b276.png)

Since the Sharp PC uses a CMOS 5v logic, while the Nucleo board is a 3.3v device, some level-shifting is required in between the two. Nucleo inputs are is 5v-tolerant, so the board inputs could easily accept the Sharp outputs without the need any converter, but the board 3.3v output isn't enough to drive the 5v input on the Sharp. The level converter I choose is one of this type: https://www.sparkfun.com/products/12009 (actually, one of its many clones).

I initially struggled a lot, before I got the Nucleo board properly receive the Device Code from the PC, which is the first step of the communication handshake. At first, using the level converter on each data line, I always got a 0xFF (0x41 is expected instead, when a FILES command for example is issued on the Sharp-PC, to invoke the Disk Drive). In spite the converters are in principle bi-directional, after a number of trials and errors I found out that they kept a constant high value on Nucleo inputs, regardless of the Sharp setting a low, because of the normally high impedance of Sharp-PC outputs, I think.

So, I decided to use the level converters only in the Nucleo-to-Sharp direction. I also configured Nucleo internal pull-down on each input line (45K resistor, as per datasheet) and added a 10K in series, as in the schematics above, to achieve the 5-to-3.3 divide in the opposite direction. This way, I reached a stage where the correct 0x41 device code, as well as the follow-up command sequence was received, but it also forced me to use different pins of the Nucleo board for the return lines (Nucleo-to-Sharp), converting them to 5v and issuing to the 11-pin connector through diodes to isolate them from the inputs (Sharp-to-Nucleo). See schematics above. Output and input stages are time-separated, and during output, input pins on Nucleo needs to be set to a PullNone (i.e. high impedance) mode.

About power, the Sharp and the Nucleo do not share the 5v power line, just gnd. This is to prevent the relatively low capacity internal coin cells to be drained by the Nucleo board. At present, the board is powered through its USB plug, but I plan to make it battery powered, maybe rechargeable.

## Emulation software description
This emulator tries to respond as closely as possible (given the knowledge we have at present of the protocol) to the commands issues by the Sharp-PC. Unfortunately, the official CE140-F Service Manual doesn't go beyond the low-level hardware description, so I tried summarizing in another document what we've found so far about the protocol:

[Protocol](https://github.com/ffxx68/Sharp_ce140f_emul/blob/main/protocol.md)

Being this a work in progress, I recommend using the source code as the ultimate reference, anyway.

## Software build notes
Board firmware is built using the standard methods offered by the online MBed Keil Studio IDE - https://studio.keil.arm.com/ - importing this GitHub repository and selecting the NUCLEO-L053R8, or NUCLEO-L432KC, depending on the actual hardware, as the target. Refer to MBed Keil Studio documentation for details about how to proceed. I will share the complied binary too, ready for upload onto the board...

The MBed library included now with this repository is the (formally unsupported) version 2. This choice is imposed by the small footprint it offers, compared to v6 (even with a "bare metal" build profile). A v6 build simply doesn't fit in the L053R8. I will upgrade it for the L432KC, but after i reached a somewhat more stable version.

The SD File System library is a small revision of the version found here: https://os.mbed.com/cookbook/SD-Card-File-System (the original didn't work out of the box, to me). Moving to v6 and standard SD libs might solve these issues, but for the timebeing I'm using a local copy. It didn't compile on the latest revision of the MBed library, anyway, so I had to rollback MBed (still v2) to revision #137. In any case, all of the above is already included in present repo, which compiles as is, no intervention needed.

The emulation software in itself is still under development, as each command from the Sharp (DSKF, FILES, SAVE, LOAD, etc.) needs data retrieved from the SD to be properly formatted back to be accepted by the Sharp. This is a rather lengthy process, involving a big deal of reverse engineering. So, stay tuned for updates...

## Acknowledgements
Remy, author of the https://pockemul.com/ emulator, who reverse engineered the CE-140F protocol.

Walter (http://www.cavefischer.at/spc/index.htm) who helped on the hardwware interface front.

The entire community of Sharp-PC enthusiasts.

The MBed forums.
