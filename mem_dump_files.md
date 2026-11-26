# SAVE M / LOAD M

`SAVE M` "\<filename\>",start address,end address\[,execution address\]

Store on a disk file (\<filename\>, in the usual Sharp 5.3 format) a dump of a memory block. 

First is the address where the memory block begins, the second represents the last byte of the memory block. 
And the third is the autostart-address, which you can use to autostart the app directly after loading it from disk again.

`LOAD M` "\<filename\>"\[,start address\]

Restore a memory block from a file (\<filename\>).

The start address is optional, and if not specified, the start address stored in the file header is used.
If the file was saved with an execution address, load will auto-start the program from that address, after loading the memory block.
If you use the load command with a different start address the auto-start will not work, as it would likely cause a crash.

The saved file is composed of a 16-byte header, followed by the actual memory dump.

A possible header structure is given in the following:

| Offset | Bytes | Possible meaning |
|--------|-------|------------------|
| 00 | FF | Binary-file marker |
| 01–04 | 20 00 00 10 | Unknown; possibly type or flags |
| 05–07 | Variable | Payload length, 3-byte little-endian |
| 08–0A | Variable | Start address, 3-byte little-endian|
| 0B–0D | Variable | Auto-start address, 3-byte little-endian; field width unconfirmed |
| 0E–0F | 00 0F | Unknown; 0F may be a terminator |
