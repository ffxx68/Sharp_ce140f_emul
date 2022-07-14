# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Interface schematics (subject to further revisions):

![sharp-ce140f-emul_v5](https://user-images.githubusercontent.com/659557/173877293-9986bfe0-33c2-4b01-9439-2c26cb54d2a0.png)

Since the Sharp PC uses a CMOS 5v logic, a level shifter is required in between the two devices. The level converter I used is one of this type: https://www.sparkfun.com/products/12009 (actually, one of the many clones).

with each line being like

![image](https://user-images.githubusercontent.com/659557/166907967-b0771314-bf71-4cde-9ebd-4cc6bff93868.png)

I initially struggled to get the Nucleo board properly receive the Device Code from the PC, which is the first step of the communication handshake. At first, using the level converter on all data lines, I always got a 0xFF (0x41 is expected, when a FILES command for example is issued on the Sharp-PC, to invoke the Disk Drive). After a number of trials and errors, I found out that the converter kept a high value on Nucleo inputs because of the normally high impedance of Sharp-PC outputs. The board is 5v-input tolerant, but its 3.3v output isn't enough to drive the 5v input on the PC, so I left the level converter only for the ACK line, removing it from the other ones. I then finally configured Nucleo internal pull-down on each input line (45K resistor, as per datasheet) and added 10K in series, as in the schematics above, to achieve the 5-to-3.3 divide.

This way, I reached a point where the correct 0x41 device code was got, and the follow-up command sequence is received as well, but forced me to use different pins of the Nucleo boardf or the return lines (Nucleo-to-Sharp) I hence decided  converted to 5v and issued to the 11-pin connector through diodes, so to isolate them from inputs (Sharp-to-Nucleo).

Then I wrote some code to process only a simple 'DSKF' command, for the timebeing, which is to query free space on the Disk Drive.
It worked as expected! Issuing this command on the Sharp:

```
>DSKF 1
```
returns succesfully the expected nuber of bytes in the (emulated) disk:
```
 16384
```

Just for the sake of completeness, I attach below a fragment from the board debug log: 
```
10953529 Device ID 0x41
10953645 CE140F
 0:1D [1D] 1:01 [1E] 2:1E [3C]
11019089 Processing...
11019233 inBufPosition 3...
11019418 checksum 0x1E vs 0x1E
11020588 command 0x1D
11020815 dataout 5 [A4] -1
11026114 0(0):0x0
11026664 ok 49989
11031927 1(1):0x0
11032755 ok 49978
11038043 1(0):0x2
11038781 ok 49983
11044095 2(1):0x0
11044858 ok 49983
11050197 2(0):0x0
11050984 ok 49983
11056347 3(1):0x5
11057253 ok 49979
11062641 3(0):0x0
11063409 ok 49986
11068823 4(1):0x0
11069663 ok 49984
11075101 4(0):0x2
11075829 ok 49990
11081293 5(1):0x5
11082298 ok 49979
11082774 send complete
```

Next steps will be to complete code for the most fundamental commands (FILES, SAVE, LOAD, at least), using an SD-Card for storage. So, first of all, I need to add an SD-CArd to the board...

## Acknowledgements
All of this was made possible thanks to the help of the community of Sharp-PC enthusiasts, and in particular to the invaluable and excellent contribution by Remy, author of the https://pockemul.com/ emulator, who reverse engineered the CE-140F protocol.
