# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a L053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

This is a demo video I made, just to show the emulator processing for example the Sharp FILES command:

https://youtu.be/5GLLVkL09qo

I started from here:

<img src="https://user-images.githubusercontent.com/659557/180180992-6d9be30f-607c-4927-bcbf-eb3c7a3ea95e.jpg" width=100% height=100%>

and a proto-board version, still based on the L053R8, presently looks like this:

<img src="https://user-images.githubusercontent.com/659557/197160147-e2b9a441-d164-4adc-97ca-d35f92db984d.jpg" width=100% height=100%>

A move to a more compact (and powerful) L432KC board is in progress. This is how it appears at present:

<img src="https://user-images.githubusercontent.com/659557/202128859-ecb3f3f4-8933-494c-a026-fa893b7ce3a1.jpg" width=100% height=100%>

I'm also wondering about the realization of a custom PCB...

## Hardware interface notes!

Interface schematics (this one is the pinout of the L053R8 board):

<img src="https://user-images.githubusercontent.com/659557/197160553-7b8a961b-1c60-4c4f-9ef8-4a4cbc0db2b4.png" width=100% height=100%>

Since the Sharp PC uses a CMOS 5v logic, while the Nucleo board is a 3.3v device, some level-shifting is required in between the two. Nucleo inputs are is 5v-tolerant, so the board inputs could easily accept the Sharp outputs without the need any converter, but the board 3.3v output isn't enough to drive the 5v input on the Sharp. The level converter I choose is one of this type: https://www.sparkfun.com/products/12009 (actually, one of its many clones).

I initially struggled a lot, before I got the Nucleo board properly receive the Device Code from the PC, which is the first step of the communication handshake. At first, using the level converter on each data line, I always got a 0xFF (0x41 is expected instead, when a FILES command for example is issued on the Sharp-PC, to invoke the Disk Drive). In spite the converters are in principle bi-directional, after a number of trials and errors I found out that they kept a constant high value on Nucleo inputs, regardless of the Sharp setting a low, because of the normally high impedance of Sharp-PC outputs, I think.

So, I decided to use the level converters only in the Nucleo-to-Sharp direction. I also configured Nucleo internal pull-down on each input line (45K resistor, as per datasheet) and added a 10K in series, as in the schematics above, to achieve the 5-to-3.3 divide in the opposite direction. This way, I reached a stage where the correct 0x41 device code, as well as the follow-up command sequence was received, but it also forced me to use different pins of the Nucleo board for the return lines (Nucleo-to-Sharp), converting them to 5v and issuing to the 11-pin connector through diodes to isolate them from the inputs (Sharp-to-Nucleo). See schematics above. Output and input stages are time-separated, and during output, input pins on Nucleo needs to be set to a PullNone (i.e. high impedance) mode.

About power, the Sharp and the Nucleo do not share the 5v power line, just gnd. This is to prevent the relatively low capacity internal coin cells to be drained by the Nucleo board. At present, the board is powered through its USB plug, but I plan to make it battery powered, maybe rechargeable.

## Emulation software description
This emulator tries to respond as closely as possible (given the knowledge we have at present of the protocol) to the commands issues by the main Sharp-PC (I'll call it "PC" in the following), over the 11-pin interface. Unfortunately, the Official Service manual doesn't go beyond the low-level hardware description, so I tried summarizing in another document what we've found so far about the entire protocol:

[Protocol](https://github.com/ffxx68/Sharp_ce140f_emul/blob/main/protocol.md)

Being this a work in progress, I anyway recommend using the source code as the ultimate reference, though.

## Software build notes
Board firmware is built using the standard methods offered by the online MBed Keil Studio IDE - https://studio.keil.arm.com/ - importing this GitHub repository and selecting the NUCLEO-L053R8, or NUCLEO-L432KC, depending on the actual target hardware, as the target. Refer to MBed Keil Studio documentation for details about how to proceed.

The MBed library included within this repository is the (now formally unsupported) version 2. This choice is imposed by the small footprint it offers, compared to v6 (even with a "bare metal" build profile). If and when I move to a larger board (e.g. a L432KC), I might upgrade to latest versions.

The SD File System library is a small revision of the version found here: https://os.mbed.com/cookbook/SD-Card-File-System (the original didn't work out of the box, to me). By the way, it doesn't compile on the latest revision of the MBed library, so I had to rollback MBed (still v2) to revision #137. In any case, all of this is already included in present repo, which should compile out of the box.

The board software is still under development, as each command from the Sharp (DSKF, FILES, SAVE, LOAD, etc.) needs data retrieved from the SD to be properly formatted back to be accepted by the Sharp. This is a rather lengthy process, invoving a big deal of reverse engineering.

## Acknowledgements
Remy, author of the https://pockemul.com/ emulator, who reverse engineered the CE-140F protocol.

Walter (http://www.cavefischer.at/spc/index.htm) who helped on the hardwware interface front.

The entire community of Sharp-PC enthusiasts.

The MBed forums.
