# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Interface schematics (subject to further revisions):

![sharp-ce140f-emul_v3](https://user-images.githubusercontent.com/659557/167366891-7e178500-a439-4742-8654-3d3bba74f75f.png)

Since the Sharp PC uses a CMOS 5v logic, a level shifter is required in between the two devices. The level converter I used is one of this type: https://www.sparkfun.com/products/12009 (actually, one of the many clones).

with each line being like

![image](https://user-images.githubusercontent.com/659557/166907967-b0771314-bf71-4cde-9ebd-4cc6bff93868.png)

I struggled to get the Nucleo board properly receive the Device Code from the PC, which is the first step of the communication handshake. At first, using the level converter on all data lines, I always got a 0xFF (0x41 is expected, when a FILES command for example is issued on the Sharp-PC, to invoke the Disk Drive). The board is 5v-input tolerant, but its 3.3v output isn't enough to drive the 5v input on the PC, so I left the level converter only for the ACK line, removing it from the BUSY, DOUT and XOUT ones, as I found out the converter kept a high value on inputs, in spite of having pull-down resistors. This way, I managed to get the board receive "some" device code from the PC, except I get a 0xC1, in place of the 0x41 that is expected. Looks like the PC output is normally at a high impedance state. I wonder what the IO port topology is.

Later, by chance, measuring voltage on the BUSY line with my digital multimeter while the code handshake was taking place, I was surprised to see that the 0x41 was correctly received! The impedance of the multimeter must have influenced communication. After some trial and error, I added a voltage divider on each line, as in the schematics above, with the series resistance at 1K and the parallel at 220k, although I want to experiment some more before concluding those are the optimal values. For the timebeing anyhow, the device code is correctly recognized and the follow-up command receive sequence is completed as well. Now I have to complete the code to process commands... Apart from understanding what is the best electrical connection setup for this interface!
