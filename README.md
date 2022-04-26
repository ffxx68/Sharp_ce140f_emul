# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt (its not working, yet!) of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR05R8 in this implementation, 
attached to a Sharp Pocket Computer through the proprietary 11-pin interface. 

Since the Sharp PC uses a CMOS 5v logic, a bidirectional level shifter is placed in between the two devices.

Schematics like below:
![image](https://user-images.githubusercontent.com/659557/165309696-1571d601-b5a6-4c15-a0b4-bf41b30a29c0.png)
