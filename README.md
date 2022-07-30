# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a L053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Interface schematics (subject to further revisions):

![sharp-ce140f-emul](https://user-images.githubusercontent.com/659557/181733096-94cec730-24d1-45ca-934d-304d5a6b01da.png)

Since the Sharp PC uses a CMOS 5v logic, a level shifter is required in between the two devices. The level converter I used is one of this type: https://www.sparkfun.com/products/12009 (actually, one of the many clones).

with each line being like

![image](https://user-images.githubusercontent.com/659557/166907967-b0771314-bf71-4cde-9ebd-4cc6bff93868.png)

I initially struggled to get the Nucleo board properly receive the Device Code from the PC, which is the first step of the communication handshake. At first, using the level converter on all data lines, I always got a 0xFF (0x41 is expected, when a FILES command for example is issued on the Sharp-PC, to invoke the Disk Drive). After a number of trials and errors, I found out that the converter kept a high value on Nucleo inputs because of the normally high impedance of Sharp-PC outputs. The board is 5v-input tolerant, but its 3.3v output isn't enough to drive the 5v input on the PC, so I left the level converter only for the ACK line, removing it from the other ones. I then finally configured Nucleo internal pull-down on each input line (45K resistor, as per datasheet) and added 10K in series, as in the schematics above, to achieve the 5-to-3.3 divide.

This way, I reached a stage where the correct 0x41 device code, as well as the follow-up command sequence is received, but it forced me to use different pins of the Nucleo board for the return lines (Nucleo-to-Sharp), converting them to 5v and issuing to the 11-pin connector through diodes, to isolate them from inputs (Sharp-to-Nucleo). See schematics. Output and Input stages are time-separated, and during output, input pins on Nucleo needs to be set to PullNone (i.e. high impedance) mode.

Then, I wrote some code to process only a simple 'DSKF' command, for the timebeing, which is to query free space on the Disk Drive.
It worked as expected, as issuing this on the Sharp:

```
> DSKF 1
```
returns succesfully the number of free bytes in the (emulated) disk. e.g.:
```
 20482
```

with the following debug log from the board:
```
1370263148 Device ID 0x41
1370263259 CE140F
1370328622 Processing...
1370328733 inBufPosition 3
1370328875 checksum 0x1E vs 0x1E
1370334873 command 0x1D
1370335038 DSKF
1370335184 dataout 5 [52]
1370340949 0: 0x00
1370352673 1: 0x02
1370364360 2: 0x50
1370375994 3: 0x00
1370387652 4: 0x52
1370393905 send complete
```

Demo video here:
https://youtu.be/_KBjp6MPla8

The working prototype looks like this at present:
![20220721_090621](https://user-images.githubusercontent.com/659557/180180992-6d9be30f-607c-4927-bcbf-eb3c7a3ea95e.jpg)
but I have in mind to move to a better looking proto-board build, maybe switching to a slightly more compact (and powerful) board, like the L432KC for example This will happen I think as soon as I reach a sufficiently stable version, mainly on the software side, where there's a work-in-progess on the most fundamental commands (FILES, SAVE, LOAD, at least), using the SD-Card as the storage device.

## Software build notes
Board firmware is built using the standard methods offered by the online MBed compiler (https://os.mbed.com/), importing this GitHub repository and selecting the NUCLEO-L053R8 as the target.

The MBed library included within this repository is the (now formally unsupported) version 2. This choice is imposed by the small footprint it offers, compared to v6 (even with a "bare metal" build profile). If and when I move to a larger board (e.g. a L432KC), I might upgrade to latest versions.

The SD File System library is a small revision of the version found here: https://os.mbed.com/cookbook/SD-Card-File-System (the original didn't work out of the box, to me). By the way, it doesn't compile on the latest revision of the MBed library, so I had to rollback MBed (still v2) to revision #137.

## Acknowledgements
All of this was made possible thanks to the help of the community of Sharp-PC enthusiasts, and in particular to the invaluable and excellent contribution by Remy, author of the https://pockemul.com/ emulator, who reverse engineered the CE-140F protocol.

The MBed community as well was necessary to solve many issues I met along the way.
