# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an hardware module emulating the Sharp CE-140F disk drive with an SD-Card, using an STM32 microcontroller to 
interface with a Sharp Pocket Computer (PC) through its proprietary 11-pin interface.

What I think the Sharp-PC was missing the most (like any retro computers from the 80's) is a quick and convenient way to store and retrieve programs to and from its memory. Entering a BASIC listing by hand has always been the most frustrating task, in the early days of home computing. Yes, a software-emulated cassette interface does exist already, but disk storage is way much better! 

The Sharp CE140-F Disk Driver was a rare and expensive extension, compatible with several of the Sharp-PC models:

PC-1280, PC-1280(Jap), PC-1285, PC-1403, PC-1403H, PC-1425, PC-1425(Jap), PC-1460, PC-1460(Jap), PC-1470U, PC-1475, PC-1475(Jap), EL-5500III, PC-E500, PC-E500(Jap), PC-E500S, PC-E550, PC-E650, PC-U6000. As well as the PC-1360, PC-1360K, PC-1365 and PC-1365K after 1986. 

See also: [pocket.free.fr](http://pocket.free.fr/html/sharp/ce-140f_e.html?fbclid=IwAR3U31Bk95W-eKc_V7EStOoxpfgk6qsZ3UOhHXM6cb7czv4wsmH_SPU4H-8)

This interface is a cheap alternative, to effectively have disk file management on the Sharp PC.

## The PCB board

After optimizing the project with this early prototype on the Nucleo L053R8 board:

<img src="https://user-images.githubusercontent.com/659557/180180992-6d9be30f-607c-4927-bcbf-eb3c7a3ea95e.jpg" width=100% height=100%>

an Indiegogo campaign ([Sharp_ce140f_emul on Indiegogo](https://www.indiegogo.com/projects/sharp-ce-140f-disk-drive-emulator/x/32084495#/)) has succesfully crowd-funded the manufacturing of the PCB board version. This PCB holds the Nucleo (here a more powerful L432KC), the SD card, and the interface circuitry:

![20230307_111529](https://user-images.githubusercontent.com/659557/232753380-9727eaa1-7254-4868-8ac6-99e3e37953b8.jpg)

Here's a demonstration video, using the LOAD and SAVE commands for example, performed with the PCB version of the emulator:
 
[LOAD or SAVE demo](https://www.youtube.com/watch?v=3_DliJE_47g&t=2s)

The complete KiCAD project is public ([KiCad v1 project](https://github.com/ffxx68/Sharp_ce140f_emul/tree/main/KiCad_v1)), while at the following link there's the PCB manufacturer's project, ready for production, in case you would like to order some directly:

[Sharp_ce140f_emul by AISLER](https://aisler.net/p/DIQRWUOC)

## Hardware interface notes

Interface schematics (find the complete KiCad project in the repo):

![image](https://user-images.githubusercontent.com/659557/213223743-f2838cbe-25bf-4762-9deb-3f7cca15b276.png)

Since the Sharp PC uses a CMOS 5v logic, while the Nucleo board is a 3.3v device, some level-shifting is required in between the two. Nucleo inputs are is 5v-tolerant, so the board inputs could easily accept the Sharp outputs without the need any converter, but the board 3.3v output isn't enough to drive the 5v input on the Sharp. The level converter I choose is one of this type: https://www.sparkfun.com/products/12009 (actually, one of its many clones).

I initially struggled a lot, before I got the Nucleo board properly receive the Device Code from the PC, which is the very first step of the communication handshake. At first, using the level converter on each data line and sharng the Nucleo pins for both input and output, the board always received a 0xFF, instead of the exepcted 0x41 (the hardcoded value issued by the Sharp-PC, to invoke the CE140-F Disk Drive). After a number of trials and errors I found out that the converter kept a constant high value on the Nucleo inputs, regardless of the Sharp setting a low. That was because of the normally high impedance of Sharp-PC outputs, I think.

So, I decided to use different pins of the Nucleo board for the return lines (Nucleo-to-Sharp) and leave the level converters only for this direction. See schematics. I also configured Nucleo internal pull-down on each input line (45K resistor, as per datasheet) and added a 10K in series, as in the schematics above, to achieve the 5-to-3.3 divide in the opposite direction. Output and input stages are time-separated, and during output, input pins on Nucleo needs to be set to a PullNone (i.e. high impedance) mode. This way, I finally reached a stage where the 0x41 device code, as well as the follow-up command sequence was correctly received by the board and was ready to be processed in software.

About power, the Sharp and the Nucleo do not share the 5v power line, just gnd. This is to prevent the relatively low capacity internal coin cells to be drained by the Nucleo board. At present, the board is powered through its USB plug, but I plan to make it battery powered, maybe rechargeable.

**Important Note** - With the Nucleo L432KC, by default the PA_5 (A4) and PA_6 (A5) pins can only be used as Input floating (ADC function). SB16 and SB18 solder bridges (0-ohm resistors, actually) must be removed, in order to use these pins as Digital output and have access to other functions (DigitalOut, SPI, PWM, etc...). Refer to the user manual for more details.

## Alternative designs

An issue has arised, while testing the finished board on different Sharp PC models. The description and a complete story of its analysis can be found at [Issue #4](https://github.com/ffxx68/Sharp_ce140f_emul/issues/4).

A relatively easy [workaround](https://github.com/ffxx68/Sharp_ce140f_emul/issues/4#issuecomment-1560580153) was later found, using stronger Pull-up resistors, which seems to fix the problem in most of cases. Still, someone has designed and built alternative version of the board. Like, for example the one suggested by Yuuichi Akagawa:

[YuuichiAkagawa's board](https://github.com/ffxx68/Sharp_ce140f_emul/issues/4#issuecomment-1596058545)

![image](https://github.com/ffxx68/Sharp_ce140f_emul/assets/659557/dce6d306-3472-459d-8ccb-559d5e070487)

where he used integrated level converters (SN74LV1T34), in place of the discrete mosfet-based original solution.

An alternative design was also proposed by Pokoyama Danna:

[Pokoyama Danna's board](https://github.com/ffxx68/Sharp_ce140f_emul/tree/main/Board_v1.5_PoyokomaDanna)

![image](https://github.com/ffxx68/Sharp_ce140f_emul/assets/659557/f0aaea48-6eab-4ee4-bb78-c86369f2b7e5)

where a programmable device (Renesas SLG46826) was used instead. 

Please refer to the owners of these designs for additional info (e.g. [Yuuichi](https://github.com/YuuichiAkagawa), or [PoyokomaDanna](ac_sent@yahoo.co.jp)).

## Emulation software description
This emulator tries to respond as closely as possible (given the knowledge we have at present of the protocol) to the commands issues by the Sharp-PC. Unfortunately, the official CE140-F Service Manual doesn't go beyond the low-level hardware description, so I tried summarizing in another document what we've found so far about the communication protocol:

[Protocol](https://github.com/ffxx68/Sharp_ce140f_emul/blob/main/protocol.md)

Being this a work in progress, I recommend using the source code as the ultimate reference, anyway.

## Software build notes

The complied firmware binaries are shared [here](https://github.com/ffxx68/Sharp_ce140f_emul/releases) as well, ready for begin uploaded onto the board. As with any Nucleo board, the fw upload procedure is to plug your board to the USB and just upload (drag&drop) the .bin file on the device, which has appeared as a (virtual) disk. This is for Windows... not sure how to do it in Linux, sorry.

If one wishes to build his firmware from code, this is possible using the standard methods offered by the online [MBed Keil Studio IDE](https://studio.keil.arm.com) - importing this complete GitHub repository in a new project and selecting the NUCLEO-L432KC as the target board for it. Then, build. Please refer to MBed Keil Studio documentation, for further details about how to proceed.

The MBed library included now with this repository is the (formally unsupported) version 2. This choice was initially imposed by the early prototype on the L053R8 by the small footprint it offers, compared to latest Mbed-OS version 6 (even with a "bare metal" build profile). The v6 build simply didn't fit in the L053R8. Then, work shifted to hardware, while software stayed on v2. An upgrade to v6 with the larger L432KC memory is possible, sooner or later...

The SD File System library is also a small revision of [this one](https://os.mbed.com/cookbook/SD-Card-File-System) (the original code didn't work out of the box, to me). Moving to v6 and standard SD libs might solve these issues, but for the timebeing I'm using a local copy. Also, it didn't compile on the latest revision of the MBed library, so I had to rollback MBed (while still on v2) to revision #137. In any case, all of the above is already included in present repo, which compiles as is - no intervention needed.

The emulation software in itself is still under development, as each command from the Sharp (DSKF, FILES, SAVE, LOAD, etc.) needs data retrieved from the SD to be properly formatted back to be accepted by the Sharp. This is a rather lengthy process, involving a big deal of reverse engineering. So, stay tuned for updates...

## Evolutions
As noted, version v1 of the board needs to be powered through an USB cable. Making the emulator entirely portable, battery powered, is the most sensible next step that gets to my mind. To this aim, I have started a second revision of the board design, aimed mainly at:

- USB-rechargable battery power
- an enclosure-ready form factor
- fixing Issue#4 (see above)

A DRAFT verions of this v2 PCB is here: 

https://github.com/ffxx68/Sharp_ce140f_emul/tree/main/KiCad_v2

but it's very far from the making, yet. Firmware would need to be revised too.

## Acknowledgements
Many have have contributed with support and suggestions to this project. 

First of all let me mention Remy, author of the https://pockemul.com/ emulator, who originally reverse engineered the CE-140F protocol.

Then Walter (http://www.cavefischer.at/spc/index.htm), who helped on the hardware interface front.

Then, the entire community of Sharp-PC enthusiasts.

Then, the contributors to the PCB-crowdfunding campaign.

Thank you all.
