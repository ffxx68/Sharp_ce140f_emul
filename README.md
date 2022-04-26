# Sharp_ce140f_emul
Sharp CE-140F disk drive emulator with an ST-Nucleo board

This is an attempt (its not working, yet!) of emulating the Sharp CE-140F disk drive with an ST-Nucleo board, a LR053R8 in this implementation, 
attached to a Sharp Pocket Computer (PC-1403) through the Sharp proprietary 11-pin interface. 

Since the Sharp PC uses a CMOS 5v logic, a bidirectional level shifter is placed in between the two devices.

Schematics like below:

![image](https://user-images.githubusercontent.com/659557/165309696-1571d601-b5a6-4c15-a0b4-bf41b30a29c0.png)

I'm presently struggling to get the board properly receive the Device Code, which is the first step of the communication protocol (0x4E in the case of adisk drive, when a FILES command for example is issued on the Sharp-PC). I'm always getting an 0xFF code instead (that is, DOUT is always high). The connection init sequence (X_OUT and DOUT both going high for at least 40 ms, as per CE-140F service manual) is correctly acknowledged, so PC-to-CE signals a re corectly issued. I have a doubt about the ACK return line...
