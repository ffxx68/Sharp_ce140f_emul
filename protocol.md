# CE 140 F Communication Protocol

Referring to the Appendix 1 (excerpted from the CE 140 F Service Manual), and to the excellent reverse-engineering job made by [Pockemul](https://pockemul.com/), using his real device and a basic logic analyzer, I can summarize the protocol as being composed basically of three layers:

1. device selection 
2. byte string receiving and sending
3. command decode and handling

The CE 140 F Service Manual unfortunately describes only the hardware level of layers 1 and 2, but nothing is said about the command decode and handling stage.

So, I will try to add some information here, with respect to what is found in the Service Manual. 

I made use of the MBed-OS GPIO and Timer triggers, instead of making a pure procedural processing of signals, as I believe that's closer to an hardware emulation. It's just a "style" chiuice, though. 

## 1 - Device selection 

A device code is issued each time the Sharp PC starts a communication with a device attached to the 11-pin interface. The protocol is basically a 1-bit serial sent over the `D_OUT` line, with high `X_OUT` high as a “device code sending” flag, and `BUSY` and `ACK` as control signals. More in Appendix 1, about that. 

Reading the _main.cpp_ file, from bottom to top, the `main` function simply maps the 11-pin interface pins to the board GPIOs, and assigns a trigger function to a rising level on the X_OUT line: 

```
irq_X_OUT.rise(&startDeviceCodeSeq);
```

When triggered, `startDeviceCodeSeq` in turns checks the state on `D_OUT` and assigns a couple more trigger functions to the rise and fall events on the `BUSY` line:

```
irq_BUSY.rise(&bitReady);
irq_BUSY.fall(NULL);
```

`bitReady` is the place where the actual bit value is handled. Here, when the 8-th bit has been received, the device code is verified and if the `0x41` code has been got (the CE 140 F has been invoked), the `BUSY` line triggers are replaced by the nibble-handling code:

```
irq_BUSY.fall(&inNibbleAck);
irq_BUSY.rise(&inNibbleReady);
```

## 2.	byte string receiving and sending

The `inNibbleReady` function handles the reception from the Sharp PC of each 4-bit low- and high-half byte. Each byte is stored in a static array here:

```
inDataBuf[]
```

for later processing. 

A timeout, reset on each byte, is used to understand if the string of bytes is complete:

```
inDataReadyTimeout.attach_us( &inDataReady, IN_DATAREADY_TIMEOUT );
```

When this timeout is triggered it means data sent from Sharp PC is complete and it can be processed. This happens inside

```
inDataReady
```

From what we understood until now, a byte string is typically made of:

 1 byte : a “command code”, issued from the Sharp PC to the Disk Drive
 
 N bytes: a variable-length command payload
 
 1 byte: a checksum, summing (modulus 0xFF) over all the previous bytes
 
Hence, a checksum verification is performed first of all. If successful, the main command decode and processing function is invoked:

```
ProcessCommand ();
```

The outcome of which is stored in another static array:

```
outDataBuf[]
```

which must be sent back to the Sharp PC. Sending is performed within

```
SendOutputData ();
```

where each byte is sent a 4-bit nibble at a time, over the GPIO output lines to the 11-pin interface, checking directly the strobing BUSY line with waiting loops here (no use of triggers, at present...).

When data sending is completed, the system is expected to return back to the device code sequence listening state: each Sharp PC-to-Disk command always begins with a device code acknowledging stage, except for some commands, e.g. SAVE, when multiple chunks of data are expected to be received. A “flag” variable:

```
skipDeviceCode
```

is used, to control such multi-chunk commands.

_Note_ - the GPIO input lines are set as “pull none” (i.e. high impedance) during the data sending stage, while they’re in “pull down” (i.e. pulled to ground by internal resistors) during the receive stage. This is a solution that I found to handle input and output over different GPIO lines. I would have preferred to reuse the same lines, but I had problems with interfacing the 3.3v Nucleo board logic with the 5v Sharp lines over the same GPIOs for both input and output. Suggestions welcome…

## 3.	command decode and handling

The `ProcessCommand` function is implemented in _commands.cpp_. The first byte in `inDataBuf[]` is checked and the logic corresponding to each command is invoked. 

Below the list of codes, with functions corresponding to each command:

```C
case 0x04: process_CLOSE(0);break;
case 0x05: process_FILES();break;
case 0x06: process_FILES_LIST(0);break;
case 0x07: process_FILES_LIST(1);break;
case 0x08: process_INIT(0x08);break;
case 0x09: process_INIT(0x09);break;
case 0x0A: process_KILL(0x0A);break;
case 0x0B: process_NAME(0x0B);break;
case 0x0C: process_SET(0x0C);break;
case 0x0D: process_COPY(0x0D);break;
case 0x0E: process_LOAD(0x0E);break;
case 0x0F: process_LOAD(0x0F);break;
case 0x10: process_SAVE(0x10);break;
case 0x11: process_SAVE(0x11);break;
case 0x16: process_SAVE(0x16);break;    // SAVE ASCII
case 0xFE: process_SAVE(0xfe);break;    // Handle ascii saved data stream
case 0xFF: process_SAVE(0xff);break;    // Handle saved data stream
case 0x12: process_LOAD(0x12);break;
case 0x13: process_INPUT(0x13);break;
case 0x14: process_INPUT(0x14);break;
case 0x15: process_PRINT(0x15);break;
case 0x17: process_LOAD(0x17);break;
case 0x1A: process_EOF(0x1A);break;
case 0x1C: process_LOC(0x1C);break;
case 0x1D: process_DSKF(); break;
case 0x1F: process_INPUT(0x1f);break;
case 0x20: process_INPUT(0x20);break;
```

Function names reflect the disk command, or statement, issued on the Sharp PC, as per the CE 140 F Operation Manual. Not every function is implemented, yet!

I don’t want to describe in detail each and every function here, for which I refer to the latest version of the code. I just highlight that each function would read the received payload, to complete the command (a LOAD command for example would require a filename, which is included in the input buffer), from the . 

On successful completion of a command processing, which would normally include an interaction with the physical SD-Card storage medium, a return payload is prepared on the output buffer 

```
outDataBuf[]
```

which is normally composed of:

1 byte: 0x00 

N bytes: return payload

1 byte: checksum

Some commands are “split” in sub-command send and receive sequences. E.g. LOAD of a BASIC file in binary format would start with a 0x0E command, for the disk emulator to get the file name and send back file size, a sequence of 16 0x17 commandd and responses, one for each of the file header bytes, and finally a 0x0E command, which would read the file from SD-card and send it back as a single block of data.

Other commands would handle multiple chunks for data sending, or receiving. This is the case for example for the LOAD of a FILE stored in ASCII mode (binary or ASCI mode is determined by how SAVE was issued and it’s stored in the file header), in which case a 0x17 line is sent  . 

Another example of multi-chuck data exchange is the SAVE command, which would expect data  to be received from Sharp PC and stored on disk over multiple segments, each 256-byte long in binary mode, or one line (terminated by a 0x0D) in ASCII mode.

But, with reverse engineering of several commands to be implemented yet, more "surprises" are expected to come...

_Note_ - Present synchronous, sequential approach (receive-process-send) is made possible because of the relatively quick SD response times and the large amount of memory, especially in the L432KC Nucleo board. Infact, with the L053R8 board, which has a smaller memory, the file size during LOAD is limited. A more sophisticated, asynchronous, approach could be possible in principle, to overcome the memory limitations, for example with two threads (read and send) and a ring buffer in between, but the development is way more complex, both to write and to test - worth it?

# APPENDIX 1 - Excerpt from the CE 140 F (Disk drive) Service Manual

## 6.4 PROTOCOL

In order to put the CE140F into action from the pocket computer, connection must be confirmed first using the device code and data transfer is carried out by receiving the protocol which represents the command code, checksum, and data.

### Device code transfer

A specific device code is issued to all devices but to choose only one device required (printer or floppy disk).

#### Device code sending protocol

First, the pocket computer issues a high state of signal on XOUT and DOUT lines for more than 40ms. (The reason why DOUT is turned high is to discriminate a high state of XOUT and low state of DOUT when beep or cassette 1/0 is done.)

As soon as device recognizes, a high state of ACK is issued. For ACK lines are wired OR one another, a high on the ACK line from any device causes the pocket computer ACK line to turn high level.

![image](https://user-images.githubusercontent.com/659557/210807167-57780c82-4e90-4006-82d4-aa39ed3f426f.png)

Upon the pocket computer recognizes a high stale of signal on ACK, the pocket computer sends out on the DOUT line the device code bit by bit from LSB through MSB. Each time a bit was sent, the pocket computer sets BUSY high. Device receives DOUT after recognizing a high state of BUSY and ACK is set. BUSY is set low by the pocket computer after recognizing a low state of ACK to complete a single bit data output. After device finishes data reception, ACK is set high again to inform the pocket computer that it is ready to receive a next data. So, the pocket computer sends out a next. one bit on DOUT in response to a high state of ACK and BUSY is set high again. In this manner, an 8-bit device code is sent to devices. 11 ACK were not set high within 5ms after BUSY turned high, the pocket computer assumes it to be an error. If the desired device exists, the ACK line is forced high in 5ms after a high to low transition of BUSY upon confirming a low on XOUT and BUSY. So, the pocket computer recognizes connection with the device.

### Data transfer

BUSY and ACK are used for strobe and input signal for handshaking. One byte of data is transferred in two units of four bits each in the parallel transmit mode.

XOUT is at a low in the data transfer mode. 

*Transfer data*

|Pin name |First handshake |Second handshake |
| -------:|:----:|:----:|
|Din	  |bit3	 |bit7  |
|Dout	  |bit2	 |bit6  |
|I02	  |bit1	 |bit5  |
|I01	  |bit0	 |bit4  |

#### Data transmit protocol (pocket computer to FDD)

The pocket computer sets low order four bits of one byte data on DIN, DOUT, I02 and I01. Then, BUSY is forced high. On the FDD side, ACK is forced low after confirming a low state of BUSY to finish the first handshaking. As the second handshaking is done in the same manner, transfer of one byte data is completed.

![image](https://user-images.githubusercontent.com/659557/210807291-2e33263f-ac46-49ec-b12a-35f9d2e4fd54.png)
 
#### Data receive protocol (FDD to pocket computer)

If ACK is high for the FDD is in process, ACK is set low for more than 10ms before data are transferred from the FDD after completion of its processing.

The FDD set ACK high after setting data on DIN, DOUT, IO2 and IO1. However, the data must be sent out in 50ms after ACK turned low.

After recognizing a high state of ACK, the pocket computer receives a 4-bit data, then BUSY is set low. After the FDD confirms a high state of BUSY, ACK is set low. After the pocket computer confirms a low state of ACK, BUSY is forced low to terminate the first handshaking. A single byte data is received by repeating this procedure.

![image](https://user-images.githubusercontent.com/659557/210807344-86515772-1925-42a6-bec7-bfd904cde2dc.png)

 
