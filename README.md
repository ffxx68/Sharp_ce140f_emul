# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Interface schematics (subject to further revisions):

![sharp-ce140f-emul_v4](https://user-images.githubusercontent.com/659557/170262231-fe509e50-176d-4df2-8618-fe65b4dc2052.png)

Since the Sharp PC uses a CMOS 5v logic, a level shifter is required in between the two devices. The level converter I used is one of this type: https://www.sparkfun.com/products/12009 (actually, one of the many clones).

with each line being like

![image](https://user-images.githubusercontent.com/659557/166907967-b0771314-bf71-4cde-9ebd-4cc6bff93868.png)

I initially struggled to get the Nucleo board properly receive the Device Code from the PC, which is the first step of the communication handshake. At first, using the level converter on all data lines, I always got a 0xFF (0x41 is expected, when a FILES command for example is issued on the Sharp-PC, to invoke the Disk Drive). The board is 5v-input tolerant, but its 3.3v output isn't enough to drive the 5v input on the PC, so I left the level converter only for the ACK line, removing it from the other ones, as I found out the converter kept a high value on inputs, because of the normally high impedance of Sharp-PC outputs. After a number of trials and errors, I configured Nucleo inputs as pull-down (45K resistor each) and added a series of 10K, as in the schematics above, and in the picture here:

![interface_v4](https://user-images.githubusercontent.com/659557/170263607-a86845d6-b4c1-4170-922e-25a63df65546.jpg)

I reached a point where the correct 0x41 device code is got, and the follow-up command sequence is received as well:

```
287873074 Device ID 0x41
287873193 CE140F
 0:05 [05] 1:58 [5D] 2:3A [97] 3:2A [C1] 4:20 [E1] 5:20 [01] 6:20 [21] 7:20 [41] 8:20 [61] 9:20 [81] 10:20 [A1] 11:2E [CF] 12:2A [F9] 13:20 [19] 14:20 [39] 15:39 [72]
288918489 Processing...
288918623 inBufPosition 16...
288918792 checksum 0x39 vs 0x39
288919924 command 0x05
...
```

Now I have to set up the return path, as well as complete the code to properly process commands and send results back...

## Acknowledgements
All of this was made possible thanks to the help of the community of Sharp-PC enthusiasts, and in particular to the invaluable contribution by Remy, author of the https://pockemul.com/ emulator, who reverse engineered the CE-140F protocol.
