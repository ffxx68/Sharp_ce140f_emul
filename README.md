# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt (its not working, yet!) of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Since the Sharp PC uses a CMOS 5v logic, a bidirectional level shifter is placed in between the two devices.

Schematics like below:

![sharp-ce140f-emul_v3](https://user-images.githubusercontent.com/659557/167366891-7e178500-a439-4742-8654-3d3bba74f75f.png)

The level converter I used is one of this type: https://www.sparkfun.com/products/12009 (actually, one of the many clones).

with each line being like

![image](https://user-images.githubusercontent.com/659557/166907967-b0771314-bf71-4cde-9ebd-4cc6bff93868.png)

I struggled a lot, to get the board properly receive the Device Code. At first, I always got a 0xFF (0x41 is expected in the case of a disk drive, when a FILES command for example is issued on the Sharp-PC), when using the level converter on all data lines. The board is 5v-input tolerant, but its 3.3v output isn't enough to drive the 5v input on the PC, so I left the level converter only for the ACK line, removing it from the BUSY, DOUT and XOUT ones, as I found out it kept a high value on inputs, in spite of the pull-down resistors. This way, I managed to get the board receive "some" device code from the PC, except I get a 0xC1, in place of the 0x41 that is expected. 

Then, by chance, measuring voltage on the BUSY line with my digital multimeter while that code handshake was taking place, I found out the 0x41 was correclty reecived! I thought the input resistance of the multimeter influenced the communincation and after some trial and error I setup an input resistance network, as in the schamtics above, with the series at 1K and parallel at 220k, but I want to experiment more beofre concluding thse are the ptimal values. FOr the timebeing anyhow, the device code is correctly recognized and the following command receive sequence is completed as well.
