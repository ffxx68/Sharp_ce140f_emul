# CE-140F Communication Protocol

This document summarizes the CE-140F command protocol as implemented by the
current firmware. It focuses on the exchanges seen on the Sharp 11-pin
interface, rather than on the firmware's internal implementation. The
official [CE-140F Service Manual](CE140F_Service_manual.pdf) describes the
electrical handshaking; command meanings and several reply details are based
on observed behavior and reverse engineering, including work by
[Pockemul](https://pockemul.com/).

## Physical transfer

The Sharp begins device selection by holding `X_OUT` and `D_OUT` high for
more than 40 ms, then sends an 8-bit device code on `D_OUT`, least-significant
bit first. The CE-140F device code is `0x41`. `BUSY` and `ACK` handshake each
bit.

After selection, `X_OUT` is low and data bytes travel as two 4-bit nibbles.
The low nibble is sent first. The four data bits are carried on `SEL_1`,
`SEL_2`, `D_OUT`, and `D_IN`:

| Signal | Bit |
|--------|-----|
| `SEL_1` | 0 |
| `SEL_2` | 1 |
| `D_OUT` | 2 |
| `D_IN` | 3 |

`BUSY` and `ACK` strobe each nibble. The same lines are used in both transfer
directions; which side drives the nibble depends on whether the Sharp is
sending or receiving.

## Command frames and checksums

A command from the Sharp is a byte string:

```text
command | command data (if any) | checksum
```

The checksum is the 8-bit sum of the command and data bytes, modulo 256; the
checksum byte itself is excluded. The current firmware treats the string as
complete after 50 ms without a completed incoming byte. A bad checksum
receives a one-byte `0xFF` error response.

Replies are transferred as byte strings using the same two-nibble handshake.
There is **not one universal reply layout**: many replies begin with status
`0x00`, followed by command-specific data and a checksum, but some commands
return a single value or use other delimiters and lengths. Error replies are
also command-specific; do not assume every error is a one-byte `0xFF`.

Some operations consist of multiple command/reply exchanges. In a continued
transfer, the next data block may follow without a new device-code selection.
The continuation codes shown below (`0xFD`, `0xFE`, and `0xFF`) are used by
the firmware for these exchanges.

## Implemented command codes

The table lists the command codes dispatched by the current firmware. Entries
marked “continuation” are used as part of a larger exchange, not generally as
standalone BASIC commands.

| Code | Operation | Protocol purpose |
|------|-----------|------------------|
| `0x03` | `OPEN` | Open a named file with the requested mode and file number. |
| `0x04` | `CLOSE` | Close a file, or close all files when the file number is `0xFF`. |
| `0x05` | `FILES` | Begin or refresh a directory listing; an optional 8.3 pattern can restrict the listing. |
| `0x06` | `FILES` continuation | Request the next entry in the listing. |
| `0x07` | `FILES` continuation | Request the previous entry in the listing. |
| `0x0A` | `KILL` | Delete the named file. |
| `0x0B` | `NAME` | Rename a file; the request contains the old and new names. |
| `0x0C` | `SET` | Set or clear the file's protected/read-only attribute. |
| `0x0D` | `COPY` | Copy the named source file to the named destination. |
| `0x0E` | `LOAD` | Start a binary load and return file information, including its size. |
| `0x0F` | `LOAD` continuation | Transfer the binary file contents. |
| `0x10` | `SAVE` | Create the destination file for a binary save. |
| `0x11` | `SAVE` continuation | Supply the binary save length and begin the incoming data stream. |
| `0x12` | `LOAD` continuation | Return the next ASCII line; `0x1A` marks end of file. |
| `0x13`, `0x14` | `INPUT#` | Read a line from an open file. |
| `0x15` | `PRINT#` | Begin a write to an open file. |
| `0x16` | `SAVE ,A` | Begin the ASCII save exchange. |
| `0x17` | `LOAD` continuation | Return the next single file byte, used while reading file metadata. |
| `0x1A` | `EOF` | Return whether the specified open file is at end of file. |
| `0x1B` | `LOF` | Return the length of the specified open file in bytes. |
| `0x1C` | `LOC` | Return the current record position of the specified open file. |
| `0x1D` | `DSKF` | Return free space in Mbytes; disk number `2` returns the fixed value `65535`. |
| `0x20` | `INPUT#` continuation | Read the remaining file data for the corresponding input operation. |
| `0xFD` | `PRINT#` continuation | Receive and write a data block for the active `PRINT#`. |
| `0xFE` | ASCII `SAVE` continuation | Receive an ASCII data block; a leading `0x1A` ends the transfer. |
| `0xFF` | Binary `SAVE` continuation | Receive a binary data block until the declared length is reached. |

File names use the Sharp's 8.3-style names and drive prefix in the request.
This emulator stores files on one SD card, so drive letters do not select
separate physical disks.

`INIT` codes `0x08` and `0x09` are not implemented; disk formatting is
intentionally unsupported. Other unlisted codes are not implemented by the
current command dispatcher.

## Multi-part exchanges

### Binary `LOAD`

`0x0E` starts the operation and returns status and file-size information.
The Sharp can then issue `0x17` requests to read individual bytes, for example
the file header, followed by `0x0F` to receive the binary file body. The body
is transferred as a stream with checksums at 256-byte boundaries and a final
terminator. Exact request order and lengths are determined by the Sharp's
LOAD mode.

### ASCII `LOAD`

The Sharp requests successive lines with `0x12`. Each reply contains the
available line data; the firmware terminates the final response with the
ASCII end-of-file marker `0x1A`.

### Binary `SAVE`

`0x10` creates the destination. `0x11` supplies the expected data length, then
one or more `0xFF` continuation blocks carry the file bytes. The transfer ends
when the received byte count reaches the declared length.

### ASCII `SAVE` and `PRINT#`

ASCII `SAVE` uses `0x16` to begin and `0xFE` for subsequent data blocks; a
block beginning with `0x1A` ends the save. `PRINT#` uses `0x15` to identify
the open file and `0xFD` to send the text block. These continuation exchanges
can proceed without repeating peripheral selection between blocks.

## Notes on observed behavior

The CE-140F Service Manual specifies the low-level device-selection and
data-transfer handshakes, but does not document these command payloads and
reply layouts. Those details come from reverse engineering and firmware
behavior, and may vary across Sharp computer models or command modes. For
command-specific response lengths, checksums, and error cases, consult the
current implementation and test against the target computer.
