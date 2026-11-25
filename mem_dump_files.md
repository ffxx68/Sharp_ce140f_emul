# SAVE M file header

Two `SAVE M` captures suggest the file saved is composed of a 16-byte header followed by the memory dump.

A possible header structure is given in the following:

| Offset | Bytes | Possible meaning |
|--------|-------|------------------|
| 00 | FF | Binary-file marker |
| 01–04 | 20 00 00 10 | Unknown; possibly type or flags |
| 05–07 | Variable | Payload length, 3-byte little-endian |
| 08–0A | Variable | Start address, 3-byte little-endian; field width unconfirmed |
| 0B–0D | Variable | "Execution" address, 3-byte little-endian; field width unconfirmed |
| 0E–0F | 00 0F | Unknown; 0F may be a terminator |

For `SAVE M "TEST.BIN",&E030,$E040,&E030`, the length is `11 00 00`
(17 bytes), and both address fields begin `30 E0`.

For `SAVE M "TEST2.BIN",&E030,$E130,&E0FF`, the length is `01 01 00`
(257 bytes); the start field begins `30 E0` and the execution field
begins `FF E0`.

In the `SAVE M` command, three addresses follow the file name.
First and second addresses contain the start and end of the dump area, respectively. 
Third address is stored in the header (named above as ""Execution" address" field), 
but what a `LOAD M` on the same file makes of this address is not known.
It may be ignored, or it may be used to set the program counter after loading. 

The end address appears implicit: start + length − 1. The memory dump
starts at offset `0x10` in the stored file. Transfer checksums are not part of the file.

These field meanings remain hypotheses; other debug captures with a different
start address would help confirm the start-address field.
