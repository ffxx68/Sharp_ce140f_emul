# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt (its not working, yet!) of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Since the Sharp PC uses a CMOS 5v logic, a bidirectional level shifter is placed in between the two devices.

Schematics like below:

![sharp-ce140f-emul_v2](https://user-images.githubusercontent.com/659557/166918378-815e555b-2549-4b1c-9d16-7c1dcbc702a1.png)

The level converter I used is one of this type: https://www.sparkfun.com/products/12009 (actually, one of the many clones).

with each line being like

![image](https://user-images.githubusercontent.com/659557/166907967-b0771314-bf71-4cde-9ebd-4cc6bff93868.png)

After initially struggling to get the board properly receive the Device Code (I always got a 0xFF; 0x41 is expected in the case of a disk drive, when a FILES command for example is issued on the Sharp-PC), I left the level converted only on the ACK line, removing it from the BUSY, DOUT and XOUT lines. The level onverter kept a high value on inputs, in spite of pull-down resistors. Infact, the board is 5v-input tolerant, but its 3.3v output isn't enough to drive the 5v input on the PC. 

With this setup, I managed to get the board receive a device code from the PC, except I get a 0xC1 code, in place of the 0x41 expected... But is that the expected value really?

Issue now is that (apart from confirming the 0xC1 code is correct!) this configuration is fine while receving the device code, but when exchanging actual data, lines are bi-directional, so the level converter would be required on data lines too. Should I use different board pins for inputs and outputs and "merge" them externally? I hope there's a simpler approach.
