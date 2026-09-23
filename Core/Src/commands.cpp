#include "commands.h"
#include "ff.h"
#include "diskio.h"
#include <stdio.h>
#include <string.h>
#include <cstdint>

extern void debug_log(const char *fmt, ...);
extern void debug_hex(volatile uint8_t *buf, volatile uint16_t len);
extern UART_HandleTypeDef huart2;
extern volatile uint16_t outDataGetPosition;
extern volatile bool highNibbleOut;
extern void SendOutputData(void);

// Global structures
SRAM2_DATA volatile uint8_t inDataBuf[IN_BUF_SIZE];
volatile uint8_t outDataBuf[OUT_BUF_SIZE];
volatile uint16_t inBufPosition;
volatile uint16_t inBufStart;
volatile uint16_t outDataPutPosition;
volatile bool cmdComplete;
volatile uint8_t skipDeviceCode = 0;

// Open file wrappers for FatFs system rules
typedef struct {
	uint8_t fn;
	uint8_t mode;
	FIL fp;
	bool isOpen;
	uint16_t pos;
} finfo_t;

SRAM2_DATA finfo_t open_files[MAX_N_FILES];

// Variables
uint8_t out_checksum = 0;
SRAM2_DATA FIL staticFile;
bool staticFileOpen = false;
/* True when staticFile was opened by an OPEN command (the ASCII SAVE route),
 * which is the only case where CLOSE owns it. A binary SAVE or a LOAD opens it
 * itself and closes it when the sequence ends - and the Sharp sends a CLOSE
 * while those are in flight, which would otherwise pull the file out from
 * under them. */
static bool staticFromOpen = false;
int fileCount;
uint8_t FileName[32];
int file_size;
int file_pos = 0;

// Target tracking macros
uint8_t sdmiso = 1; // Handled directly via FatFs f_mount checks
SRAM2_DATA FATFS FatFs;

/* Closing a file during ASCII LOAD operations after a timeout.
 * Needed in case Sharp gets an error during LOAD and no longer issues a 0x12
 * command to get the next line, while the file is still open.
 * Mbed used a hardware Timeout; here it is serviced from the main loop. */
#define LOAD_WD_TIMEOUT 3000  /* ms */
static volatile uint32_t loadWdTimestamp = 0;
static volatile bool loadWdActive = false;

void loadWatchdogArm(void) {
	loadWdTimestamp = HAL_GetTick();
	loadWdActive = true;
}

void loadWatchdogDisarm(void) {
	loadWdActive = false;
}

void loadWatchdogService(void) {
	if (loadWdActive && ((HAL_GetTick() - loadWdTimestamp) >= LOAD_WD_TIMEOUT)) {
		loadWdActive = false;
		debug_log("loadWatchdog triggered\n");
		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}
	}
}

/* Mount once and keep it mounted.
 *
 * The port used to call f_mount(...,1) at the start of every command. That
 * clears fs_type and bumps the volume id inside FatFs, so every file left open
 * between two commands becomes FR_INVALID_OBJECT - and LOAD, SAVE, OPEN/PRINT/
 * INPUT are all multi-command sequences that keep a file open across them. */
static bool fsMounted = false;

bool sd_ready(void) {
	if (fsMounted && ((disk_status(0) & STA_NOINIT) == 0)) {
		return true;
	}
	fsMounted = (f_mount(&FatFs, "0:", 1) == FR_OK);
	if (!fsMounted) {
		debug_log("f_mount failed\n");
	}
	return fsMounted;
}

void sd_invalidate(void) {
	fsMounted = false;
}

/* Which directory entries the Sharp is allowed to see.
 *
 * A card that has been plugged into a Mac is full of entries the PC-E500 must
 * not list: macOS writes a metadata file "._NAME.BAS" next to every file, plus
 * .DS_Store, .Spotlight-V100 and .fseventsd. With long file names disabled
 * those show up under their generated 8.3 alias - "._1920.BAS" becomes
 * "_1920~1.BAS" - which is where the odd "_" names come from. Listing them also
 * shifts the file numbering, so FILES and FILES_LIST disagree.
 *
 * Both loops must use exactly the same test, or the index the Sharp asks for
 * will not match the file that gets sent back. */
/* ff.h only exports AM_DIR and AM_ARC; the rest live inside ff.c */
#define CE140F_AM_HID 0x02
#define CE140F_AM_SYS 0x04
#define CE140F_AM_VOL 0x08

/* Wild card matching for FILES.
 *
 * The Sharp expands the pattern itself before sending it: FILES "X:C*.BAS"
 * arrives as the 16-byte command
 *     05 'X' ':' 'C' ?  ?  ?  ?  ?  ?  ?  '.' 'B' 'A' 'S' checksum
 * so only '?' ever reaches us, one per character position, 8 for the name and
 * 3 for the extension. The emulator used to ignore all of it and list the whole
 * card - the behaviour the CE-140F manual documents but the firmware never
 * implemented.
 *
 * Kept as state because FILES with no argument reuses the previous pattern. */
static uint8_t filePattern[12] = {
    '?','?','?','?','?','?','?','?', '.', '?','?','?'
};

static bool name_matches_pattern(const char *fname) {
    char n83[11];
    memset(n83, ' ', sizeof(n83));

    const char *dot = strchr(fname, '.');
    size_t nlen = dot ? (size_t)(dot - fname) : strlen(fname);
    if (nlen > 8) nlen = 8;
    memcpy(n83, fname, nlen);

    if (dot) {
        size_t elen = strlen(dot + 1);
        if (elen > 3) elen = 3;
        memcpy(n83 + 8, dot + 1, elen);
    }

	/* Only '?' is a wild card. A space is a literal: the Sharp pads short
	 * patterns with them, so "??????\x20\x20.BAS" means a name of six
	 * characters or fewer - the 8.3 entry is space-padded too and matches
	 * position by position. */
	for (int i = 0; i < 8; i++) {
		if (filePattern[i] != '?' && filePattern[i] != n83[i]) return false;
	}
	for (int i = 0; i < 3; i++) {
		if (filePattern[9 + i] != '?' && filePattern[9 + i] != n83[8 + i]) return false;
	}
    return true;
}

static bool file_is_listable(const FILINFO *fno) {
	if (fno->fattrib & (AM_DIR | CE140F_AM_HID | CE140F_AM_SYS | CE140F_AM_VOL))
		return false;
	if (fno->fname[0] == '.') return false;      /* dot files */
	if (strchr(fno->fname, '~')) return false;   /* 8.3 alias of a long name */
	if (strcmp(fno->fname, DIRLIST_NAME) == 0) return false;  /* ours, see below */
	if (strstr(fno->fname, ".") == 0) return false;  /* must have an extension */
	return name_matches_pattern(fno->fname);
}

/* The FatFs objects live in .sram2, which the startup code does not clear,
 * so anything that used to rely on being zero at reset is zeroed here. */
void commands_init(void) {
	memset((void*) open_files, 0, sizeof(open_files));
	memset((void*) &staticFile, 0, sizeof(staticFile));
	memset((void*) &FatFs, 0, sizeof(FatFs));
	memset((void*) inDataBuf, 0, IN_BUF_SIZE);
	staticFileOpen = false;
	fsMounted = false;
}

/* Closes the handle LOAD, SAVE and an OPEN with file number 0/1 all share.
 * Without this FatFs never writes the directory entry back and an ASCII SAVE
 * leaves a 0-byte file: the Sharp ends that sequence with CLOSE. */
static void close_static_file(void) {
	if (!staticFileOpen) return;
	uint32_t t0 = HAL_GetTick();
	FRESULT r = f_close(&staticFile);
	debug_log("f_close static %d in %lums\n", r,
	          (unsigned long)(HAL_GetTick() - t0));
	staticFileOpen = false;
	staticFromOpen = false;
}

void print_to_pc(const char *msg) {
	debug_puts(msg);   /* same buffer as debug_log: the UART has one writer */
}

uint8_t CheckSum(uint8_t b) {
	out_checksum = (out_checksum + b) & 0xff;
	return b;
}

void outDataAppend(uint8_t b) {
	if (outDataPutPosition < OUT_BUF_SIZE) {
		outDataBuf[outDataPutPosition++] = b;
	}
}

void sendString(const char *s) {
	for (size_t i = 0; i < strlen(s); i++) {
		outDataAppend(CheckSum(s[i]));
	}
}

void trim(uint8_t *s) {
	uint8_t *d = s;
	do {
		while (*d == ' ') {
			++d;
		}
	} while ((*s++ = *d++));
}

bool file_exists(char *filename) {
	FIL tmp;
	if (f_open(&tmp, filename, FA_READ) == FR_OK) {
		f_close(&tmp);
		return true;
	}
	return false;
}

void getFileName(void) {
	uint8_t tmpFile[16];
	strncpy((char*) tmpFile, (const char*) (inDataBuf + 3), 12);
	tmpFile[12] = 0;
	trim(tmpFile);
	sprintf((char*) FileName, "%s%s", SD_HOME, tmpFile);
	debug_log("SDcard filename: %s\n", FileName);
}

void process_FILES(void) {
	DIR dir;
	FILINFO fno;
	int n_files = 0x00;

	/* "X:" then 8 name chars, '.', 3 extension chars - 16 bytes with the
	 * command and checksum. Anything shorter came without a pattern. */
	if (inBufPosition >= 16) {
		memcpy(filePattern, (const void*) &inDataBuf[3], sizeof(filePattern));
		debug_log("FILES <%.8s.%.3s>\n", filePattern, filePattern + 9);
	} else {
		debug_log("FILES\n");
	}
	outDataAppend(CheckSum(0x00));

	if (!sd_ready()) {
		ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		outDataAppend(CheckSum(0x00));
		outDataAppend(out_checksum);
		return;
	}

	if (f_opendir(&dir, SD_HOME) == FR_OK) {
		while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0
				&& n_files < 0xFF) {
			if (file_is_listable(&fno))
				n_files++;
		}
		f_closedir(&dir);
		fileCount = -1;
		if (n_files > 255) {
			ERR_PRINTOUT("Number of files greater than 255!\n");
			outDataAppend(0x00);
		} else {
			debug_log("%d files\n", n_files);
			outDataAppend(CheckSum(n_files));
		}
	} else {
		ERR_PRINTOUT("Could not open SD home directory!\n");
		outDataAppend(0x00);
	}
	outDataAppend(out_checksum);
}

void process_FILES_LIST(uint8_t cmd) {
	DIR dir;
	FILINFO fno;
	int n_files = -1;
	uint8_t tmp[15];

	debug_putc('f');
	debug_putc((char)(0x30 + cmd));
	debug_putc('\n');

	debug_log("FILES_LIST 0x%02X\n", cmd);
	outDataAppend(0x00);
	out_checksum = 0;

	if (!sd_ready()) {
		ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		outDataAppend(0xFF);
		outDataAppend(out_checksum);
		return;
	}

	switch (cmd) {
	case 0:
		fileCount++;
		break;
	case 1:
		fileCount--;
		break;
	}
	debug_log("file # %d\n", fileCount);

	if (f_opendir(&dir, SD_HOME) != FR_OK) {
		ERR_PRINTOUT("Could not open SD home directory!\n");
		outDataAppend(0xFF);
		outDataAppend(out_checksum);
		return;
	}

	bool found = false;
	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0
			&& n_files < 0xFF) {
		if (file_is_listable(&fno)) {
			n_files++;
			if (n_files == fileCount) {
				found = true;
				break;      /* this is the file we were asked for */
			}
		}
	}
	f_closedir(&dir);

	if (!found) {
		/* Walking off the end of the directory used to leave fno.fname empty
		 * and still send the previous request's name after the error byte. */
		debug_log("file # %d not found (%d listable)\n", fileCount, n_files);
		ERR_PRINTOUT(" ERR clean\n");
		outDataAppend(0xFF);
		outDataAppend(out_checksum);
		return;
	}

	debug_log("<%s>\n", fno.fname);
	char *p = strstr(fno.fname, ".");
	char *s = fno.fname;
	strncpy((char*) tmp, s, (size_t)(p - s));
	tmp[p - s] = 0x00;
	trim(tmp);
	/* file name as the Sharp expects it: "X:NAME    .BAS "
	 *
	 * The manual says FILES shows a P next to a protected file. Tried putting
	 * it in that trailing position and the machine does not display it, so the
	 * attribute travels somewhere else - probably a longer reply. Left as it
	 * was rather than experiment on the one command that gets used most. */
	/* file name as the Sharp expects it: "X:NAME    .BAS "
	 *
	 * The manual says the listing shows a P beside a protected file, and this
	 * firmware cannot make that happen. Measured, so nobody repeats it:
	 *
	 *   - the reply is 17 bytes - status, 15 of name, checksum. Sending one
	 *     more, the machine takes 17 and stops ("SO Err2 pos: 17"), so the
	 *     attribute can only live inside those 15;
	 *   - put in the trailing position, neither FILES on screen nor LFILES on
	 *     paper draws it;
	 *   - during a FILES the machine issues no other command, so it is not
	 *     asking for the attribute anywhere else.
	 *
	 * The P that does print comes from DIRLIST.TXT, which this firmware writes
	 * itself. */
	snprintf((char*) FileName, sizeof(FileName), "X:%-8s%4s ", (char*) tmp, p);
	debug_log("formatted <%s>\n", FileName);
	sendString((char*) FileName);
	outDataAppend(out_checksum);
}

void process_LOAD(uint8_t cmd) {
	debug_log("LOAD 0x%02X\n", cmd);
	BYTE c = 0;
	UINT bytesRead;
	uint8_t tmpFile[16];

	out_checksum = 0;
	if (!sd_ready()) {
		ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		outDataAppend(0x00);
		sendString(" ");
		outDataAppend(0x00);
		outDataAppend(0x00);
		outDataAppend(0x00);
		outDataAppend(out_checksum);
		return;
	}

	switch (cmd) {
	case 0x0E: {
		debug_putc('l');
		strncpy((char*) tmpFile, (const char*) (inDataBuf + 3), 12);
		tmpFile[12] = '\0';
		trim(tmpFile);
		sprintf((char*) FileName, "%s%s", SD_HOME, tmpFile);
		debug_log("opening <%s>\n", FileName);

		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}

		if (f_open(&staticFile, (char*) FileName, FA_READ) != FR_OK) {
			/* No such file. Answering nothing leaves the Sharp holding BUSY
			 * for ever - it is waiting for a response that never comes and
			 * only BREAK gets it back. 0xFF is the error answer the rest of
			 * the commands already use (SAVE, OPEN, KILL). */
			debug_log("file not found <%s>\n", FileName);
			ERR_PRINTOUT("f_open error\n");
			/* Measured on the machine, in this order:
			 *   - answering nothing at all: the Sharp holds BUSY for ever;
			 *   - a bare 0xFF: goes out fine, still hangs - the reply is
			 *     shorter than the six bytes it is waiting for;
			 *   - six bytes led by 0x00: accepted, and the Sharp carries on
			 *     with the 0x17 header reads as if the file were there.
			 * So it needs the full length AND the error in the leading byte.
			 * 0xFF is the value tried and verified here; do not swap it for
			 * ERR_REPLY without measuring again - the one-byte replies were
			 * measured separately and behave differently. */
			outDataAppend(0xFF);
			sendString(" ");
			outDataAppend(CheckSum(0x00));
			outDataAppend(CheckSum(0x00));
			outDataAppend(CheckSum(0x00));
			outDataAppend(out_checksum);
			break;
		}
		staticFileOpen = true;
		staticFromOpen = false;
		file_size = f_size(&staticFile);
		debug_log("size %d\n", file_size);
		file_pos = 0;
		outDataAppend(0x00);
		sendString(" ");
		outDataAppend(CheckSum(file_size & 0xff));
		outDataAppend(CheckSum((file_size >> 8) & 0xff));
		outDataAppend(CheckSum((file_size >> 16) & 0xff));
		outDataAppend(out_checksum);
		break;
	}
	case 0x17: {
		debug_putc('0');
		if (f_read(&staticFile, &c, 1, &bytesRead) == FR_OK && bytesRead > 0) {
			file_pos++;
			outDataAppend(0x00);
			debug_log("first byte 0x%02X\n", c);
			outDataAppend(CheckSum(c));
			outDataAppend(out_checksum);
		} else {
			/* Same trap as the 0x0E error above: a lone 0xFF is shorter than
			 * the reply the Sharp is clocking out, and it hangs waiting for
			 * the rest. Keep the three-byte shape of a good answer. */
			ERR_PRINTOUT("f_read EOF");
			outDataAppend(0x00);
			outDataAppend(CheckSum(0xFF));
			outDataAppend(out_checksum);
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		}
		break;
	}
	case 0x12: {
		debug_putc('a');
		outDataAppend(0x00);
		loadWatchdogArm();  // closes the file if no further 0x12 comes
		if (!staticFileOpen) {
			ERR_PRINTOUT("File is not open. Watchdog triggered?\n");
			outDataAppend(CheckSum(0x1A));
			outDataAppend(out_checksum);
			outDataAppend(0x00);
			break;
		}
		do {
			if (f_read(&staticFile, &c, 1, &bytesRead) != FR_OK
					|| bytesRead == 0) {
				c = 0xFF;               /* real end of file */
				bytesRead = 0;
				break;
			}
			if (c == 0x1A) {
				/* The file carries its own end-of-file marker: anything written
				 * through COM: or by a PC tool ends in 1A. Passed through as
				 * data it lands in the middle of a line and the Sharp errors on
				 * it, which is exactly the symptom - a correct listing with one
				 * bad line at the end. The file is telling us it ends here. */
				debug_log("1A in file at %d -> EOF\n", file_pos);
				c = 0xFF;
				bytesRead = 0;
				break;
			}
			file_pos++;
			outDataAppend(CheckSum(c));
		} while (c != 0x0d);

		if (c != 0x0d) {
			debug_log("EOF\n");
			outDataAppend(CheckSum(0x1A));
			loadWatchdogDisarm();
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		} else {
			debug_log("line\n");
		}
		outDataAppend(out_checksum);
		outDataAppend(0x00);
		break;
	}
	case 0x0f: {
		debug_putc('.');
		outDataAppend(0x00);
		int data_start = file_pos;
		do {
			if (f_read(&staticFile, &c, 1, &bytesRead) == FR_OK
					&& bytesRead > 0) {
				file_pos++;
				outDataAppend(CheckSum(c));
				if (((file_pos - data_start) % 0x100) == 0) {
					outDataAppend(out_checksum);
					out_checksum = 0;
				}
				/* Send and drain instead of staging the whole file.
				 *
				 * The buffer only holds OUT_BUF_SIZE bytes; past that
				 * outDataAppend() drops data silently, so a large file was
				 * reported complete having been truncated and the Sharp hung
				 * waiting for the rest (issue #6, and still broken for
				 * anything over ~40 KB even with the fix). Flushing here
				 * removes the size limit entirely - the Sharp simply waits a
				 * little longer between nibbles while the next chunk is read. */
				if (outDataPutPosition >= OUT_BUF_SIZE - 8) {
					debug_log("flush at %d of %d\n", file_pos, file_size);
					SendOutputData();
					outDataPutPosition = 0;
					outDataGetPosition = 0;
					highNibbleOut = false;
				}
			} else {
				break;      /* end of file: no byte of its own */
			}
		} while (file_pos < file_size);
		outDataAppend(out_checksum);
		outDataAppend(0x00);
		if (file_pos != file_size) {
			ERR_PRINTOUT("f_read error during LOAD\n");
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		}
		if (file_pos == file_size) {
			debug_log("file complete (file_size %d)\n", file_size);
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		}
		break;
	}
	default: {
		ERR_PRINTOUT("unknown LOAD sub-command\n");
		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}
		break;
	}
	}
}

void process_SAVE(int cmd) {
	debug_log("SAVE 0x%02X\n", cmd);
	UINT bytesWritten;

	out_checksum = 0;
	if (!sd_ready()) {
		ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		outDataAppend(ERR_REPLY);
		return;
	}

	switch (cmd) {
	case 0x10: {
		debug_puts("s0");
		getFileName();
		debug_log("creating <%s>\n", FileName);
		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}
		FRESULT ores = f_open(&staticFile, (char*) FileName,
				FA_CREATE_ALWAYS | FA_WRITE);
		if (ores != FR_OK) {
			debug_log("f_open write error %d\n", ores);
			ERR_PRINTOUT("f_open write error\n");
			outDataAppend(ERR_REPLY);
			break;
		}
		staticFileOpen = true;
		staticFromOpen = false;
		file_pos = 0;
		outDataAppend(0x00);
		break;
	}
	case 0x11: {
		debug_puts("s1");
		if (file_pos != 0) {
			ERR_PRINTOUT("unexpected 0x11\n");
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
			outDataAppend(ERR_REPLY);
			break;
		}
		file_size = (int) inDataBuf[2] + (int) (inDataBuf[3] << 8)
				+ (int) (inDataBuf[4] << 16);
		debug_log("filesize: %d\n", file_size);
		outDataAppend(0x00);
		skipDeviceCode = 0xFF;
		break;
	}
	case 0x16: {
		debug_puts("s6");
		outDataAppend(0x00);
		file_pos = 0;
		skipDeviceCode = 0xFE;
		break;
	}
	case 0xFF: {
		debug_putc('.');
		int buf_pos = 0;
		skipDeviceCode = 0xFF;
		if (!staticFileOpen) {
			ERR_PRINTOUT("file not open\n");
			outDataAppend(ERR_REPLY);
			break;
		}
		debug_log("block %d bytes, at %d of %d\n", inBufPosition - 1, file_pos,
				file_size);
		while (buf_pos < inBufPosition - 1) {
			FRESULT wres = f_write(&staticFile, (void*) &inDataBuf[buf_pos], 1,
					&bytesWritten);
			if (wres != FR_OK || bytesWritten != 1) {
				debug_log("f_write error %d at %d\n", wres, file_pos);
				ERR_PRINTOUT("f_write error\n");
				f_close(&staticFile);
				staticFileOpen = false;
				sd_invalidate();
				skipDeviceCode = 0x00;
				outDataAppend(ERR_REPLY);
				return;
			}
			buf_pos++;
			file_pos++;
		}
		if (file_pos == file_size) {
			if (staticFileOpen) {
				uint32_t t0 = HAL_GetTick();
				FRESULT cres = f_close(&staticFile);
				staticFileOpen = false;
				debug_log("f_close %d in %lums\n", cres,
				          (unsigned long)(HAL_GetTick() - t0));
				if (cres != FR_OK) {
					ERR_PRINTOUT("f_close error\n");
					sd_invalidate();
					skipDeviceCode = 0x00;
					outDataAppend(ERR_REPLY);
					return;
				}
			}
			debug_log("file done\n");
			skipDeviceCode = 0x00;
		}
		outDataAppend(0x00);
		break;
	}
	case 0xFE: {
		debug_putc('.');
		int buf_pos = 0;
		if (!staticFileOpen) {
			ERR_PRINTOUT("file not open\n");
			outDataAppend(ERR_REPLY);
			break;
		}
		/* Only the first byte: scanning the whole line risks a false positive
		 * on a legitimate 0x1A in the data, and inBufPosition - 1 underflows
		 * to 65535 on an empty block. */
		if (inDataBuf[buf_pos] == 0x1A) {
			debug_log("file done\n");
			close_static_file();
		} else {
			while (buf_pos < inBufPosition - 1) {
				FRESULT wres = f_write(&staticFile, (void*) &inDataBuf[buf_pos],
						1, &bytesWritten);
				if (wres != FR_OK || bytesWritten != 1) {
					debug_log("f_write error %d at %d\n", wres, file_pos);
					ERR_PRINTOUT("f_write error\n");
					f_close(&staticFile);
					staticFileOpen = false;
					sd_invalidate();
					outDataAppend(ERR_REPLY);
					return;
				}
				buf_pos++;
				file_pos++;
			}
		}
		outDataAppend(0x00);
		break;
	}
	default: {
		ERR_PRINTOUT("unknown SAVE sub-command\n");
		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}
		break;
	}
	}
}
void process_DSKF(void) {
	uint8_t dn = inDataBuf[1];
	uint32_t diskspace = 65535;
	debug_log("DSKF %d\n", dn);
	if (dn != 2) {
		FATFS *fs;
		DWORD fre_clust;
		/* Mount first: DSKF used to rely on some earlier command having done it,
		 * so it reported the 65535 placeholder whenever it was the first thing
		 * run after a reset. */
		if (!sd_ready()) {
			ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		} else if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
			uint64_t freeMb = (uint64_t) fs->csize * (uint64_t) fs->free_clst
					* 512 / 1048576;
			uint64_t totMb = (uint64_t) fs->csize * (uint64_t) (fs->n_fatent - 2)
					* 512 / 1048576;
			diskspace = (uint32_t) freeMb;
			/* fs_type: 1=FAT12 2=FAT16 3=FAT32. A 32 GB card reporting ~2 GB
			 * means it was formatted with a 2 GB partition (FAT16's ceiling). */
			debug_log("SD fat%d csize%u clusters%lu free%lu -> %lu/%lu Mb\n",
			          fs->fs_type, fs->csize,
			          (unsigned long)(fs->n_fatent - 2),
			          (unsigned long) fs->free_clst,
			          (unsigned long) freeMb, (unsigned long) totMb);
		}
	}
	outDataAppend(CheckSum(0x00));
	outDataAppend(CheckSum(diskspace & 0xff));
	outDataAppend(CheckSum((diskspace >> 8) & 0xff));
	outDataAppend(CheckSum((diskspace >> 16) & 0xff));
	outDataAppend(out_checksum);
}
void process_CLOSE(void) {
	uint8_t fn = inDataBuf[1];
	debug_log("CLOSE 0x%02X\n", fn);
	out_checksum = 0;
	if (fn == 0xFF) {
		for (int i = 0; i < MAX_N_FILES; i++) {
			if (open_files[i].isOpen) {
				f_close(&open_files[i].fp);
				open_files[i].isOpen = false;
			}
		}
		/* staticFile is what LOAD/SAVE and an OPEN with file number 0/1 use.
		 * Without closing it here FatFs never writes the directory entry back,
		 * so an ASCII SAVE transfers every line and still leaves a 0-byte file:
		 * the Sharp ends the sequence with CLOSE, not with a 0x1A block. */
		if (staticFromOpen) close_static_file();
	} else if (fn < 2) {
		if (staticFromOpen) close_static_file();
	} else {
		fn = fn - 2;
		if (fn >= MAX_N_FILES) {
			ERR_PRINTOUT("Invalid file #\n");
			outDataAppend(ERR_REPLY);
			return;
		}
		if (open_files[fn].isOpen) {
			f_close(&open_files[fn].fp);
			open_files[fn].isOpen = false;
		}
	}
	outDataAppend(CheckSum(0x00));
}
/* A directory listing as a readable file.
 *
 * LFILES cannot help here: it prints through the 11-pin bus, and a serial
 * printer on COM: is out of its reach - the machine does not even address it.
 * And BASIC has no way to get the file names into variables, FILES only paints
 * them on the screen. So opening this name for input writes the listing to the
 * card first and hands back a normal file:
 *
 *   10 OPEN "COM:" AS #1
 *   20 OPEN "X:DIRLIST.TXT" FOR INPUT AS #2
 *   30 IF EOF(2) THEN 70
 *   40 INPUT# 2,A$
 *   50 PRINT# 1,A$
 *   60 GOTO 30
 *   70 CLOSE
 *
 * The name belongs to the emulator: it is rewritten on every open and kept out
 * of the FILES listing. This is an invention of this firmware, not something
 * the CE-140F ever did. */
static FRESULT build_dir_list(void) {
	DIR dir;
	FILINFO fno;
	FIL out;
	UINT bw;
	char line[40];
	char base[16];
	char *dot;

	if (f_opendir(&dir, SD_HOME) != FR_OK) return FR_NO_PATH;
	FRESULT res = f_open(&out, SD_HOME DIRLIST_NAME, FA_CREATE_ALWAYS | FA_WRITE);
	if (res != FR_OK) {
		f_closedir(&dir);
		return res;
	}

	int n_files = 0;
	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
		if (!file_is_listable(&fno)) continue;
		dot = strchr(fno.fname, '.');
		if (dot == 0) continue;
		size_t len = (size_t)(dot - fno.fname);
		if (len > sizeof(base) - 1) len = sizeof(base) - 1;
		memcpy(base, fno.fname, len);
		base[len] = '\0';
		int n = snprintf(line, sizeof(line), "%-8s.%-3s %6u %c\r\n", base,
				dot + 1, (unsigned int) fno.fsize,
				(fno.fattrib & AM_RDO) ? 'P' : ' ');
		if (f_write(&out, line, n, &bw) != FR_OK || bw != (UINT) n) {
			res = FR_DISK_ERR;
			break;
		}
		n_files++;
	}
	f_closedir(&dir);
	FRESULT cres = f_close(&out);
	if (res == FR_OK) res = cres;
	debug_log("DIRLIST built: %d files, res %d\n", n_files, res);
	return res;
}

static bool is_dir_list(const char *path) {
	size_t plen = strlen(path);
	size_t nlen = strlen(DIRLIST_NAME);
	return (plen >= nlen) && (strcmp(path + plen - nlen, DIRLIST_NAME) == 0);
}

void process_OPEN(void) {
	uint8_t mode = inDataBuf[15];
	uint8_t rawFn = inDataBuf[16];
	getFileName();
	debug_log("OPEN <%s> FOR '%d' AS #%d\n", FileName, mode, rawFn);

	/* Reading the listing: write it out fresh, then carry on as usual. */
	if (mode == 1 && is_dir_list((char*) FileName)) {
		FRESULT dres = build_dir_list();
		if (dres != FR_OK) {
			ERR_PRINTOUT("DIRLIST error\n");
			outDataAppend(ERR_REPLY);
			return;
		}
	}

	/* SAVE "X:NAME",A opens its destination with file number 1 and then streams
	 * the lines with 0x16/0xFE, so this OPEN has to land on the same handle
	 * process_SAVE() writes to. Mbed got away with it because one global FILE*
	 * was shared by OPEN, LOAD and SAVE; here those paths are separate, so the
	 * low file numbers are routed to staticFile explicitly.
	 * (Otherwise fn = 1 - 2 wraps to 255 in a uint8_t, the range check below
	 * rejects it, and the ASCII SAVE then finds no open file at all.) */
	if (rawFn < 2) {
		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}
		BYTE flags = (mode == 1) ? FA_READ
		           : (mode == 3) ? (FA_OPEN_APPEND | FA_WRITE)
		                         : (FA_CREATE_ALWAYS | FA_WRITE);
		FRESULT ores = f_open(&staticFile, (char*) FileName, flags);
		if (ores != FR_OK) {
			debug_log("f_open error %d\n", ores);
			ERR_PRINTOUT("f_open error\n");
			outDataAppend(ERR_REPLY);
			return;
		}
		staticFileOpen = true;
		staticFromOpen = true;
		file_pos = 0;
		outDataAppend(CheckSum(0x00));
		return;
	}

	uint8_t fn = rawFn - 2;
	if (fn >= MAX_N_FILES) {
		ERR_PRINTOUT("Invalid file #\n");
		outDataAppend(ERR_REPLY);
		return;
	}
	if (open_files[fn].isOpen) {
		f_close(&open_files[fn].fp);
		open_files[fn].isOpen = false;
	}
	FRESULT res = FR_INT_ERR;
	switch (mode) {
	case 1:
		res = f_open(&open_files[fn].fp, (char*) FileName, FA_READ);
		break;
	case 2:
		res = f_open(&open_files[fn].fp, (char*) FileName,
				FA_CREATE_ALWAYS | FA_WRITE);
		break;
	case 3:
		res = f_open(&open_files[fn].fp, (char*) FileName,
				FA_OPEN_APPEND | FA_WRITE);
		break;
	}
	if (res != FR_OK) {
		debug_log("fopen error %d\n", res);
		ERR_PRINTOUT("fopen error\n");
		outDataAppend(ERR_REPLY);
	} else {
		open_files[fn].isOpen = true;
		open_files[fn].mode = mode;
		open_files[fn].pos = 0;
		outDataAppend(CheckSum(0x00));
	}
}
uint8_t cur_fn;
void process_PRINT(int cmd) {
	debug_log("PRINT 0x%02X\n", cmd);
	UINT bytesWritten;
	switch (cmd) {
	case 0x15:
		cur_fn = inDataBuf[1] - 2;
		if (cur_fn >= MAX_N_FILES) {
			ERR_PRINTOUT("Invalid file #\n");
			outDataAppend(ERR_REPLY);
			break;
		}
		skipDeviceCode = 0xFD;
		outDataAppend(0x00);
		break;
	case 0xFD:
		int buf_pos = 0;
		if (cur_fn >= MAX_N_FILES || !open_files[cur_fn].isOpen) {
			ERR_PRINTOUT("file not open\n");
			outDataAppend(ERR_REPLY);
			break;
		}
		while (buf_pos < inBufPosition - 2) {
			f_write(&open_files[cur_fn].fp, (void*) &inDataBuf[buf_pos], 1,
					&bytesWritten);
			buf_pos++;
			open_files[cur_fn].pos++;
		}
		if (inDataBuf[inBufPosition - 3] != 0x0A) {
			uint8_t cr = 0x0D, lf = 0x0A;
			f_write(&open_files[cur_fn].fp, &cr, 1, &bytesWritten);
			f_write(&open_files[cur_fn].fp, &lf, 1, &bytesWritten);
		}
		outDataAppend(CheckSum(0x00));
		break;
	}
}
void process_INPUT(int cmd) {
	debug_log("INPUT 0x%02X\n", cmd);
	cur_fn = inDataBuf[1] - 2;
	UINT bytesRead;
	if (cur_fn >= MAX_N_FILES) {
		ERR_PRINTOUT("Invalid file #\n");
		outDataAppend(ERR_REPLY);
		return;
	}
	switch (cmd) {
	case 0x13:
	case 0x14: {
		outDataAppend(0x00);
		char c;
		char line[82];
		line[0] = 0x00;
		do {
			if (f_read(&open_files[cur_fn].fp, &c, 1, &bytesRead) != FR_OK
					|| bytesRead == 0) {
				break;
			}
			strncat(line, &c, 1);
			open_files[cur_fn].pos++;
		} while (c != 0x0A);
		sendString(line);
		outDataAppend(0x00);
		outDataAppend(out_checksum);
		outDataAppend(0x00);
		break;
	}
	case 0x20: {
		outDataAppend(0x00);
		char c;
		while (f_read(&open_files[cur_fn].fp, &c, 1, &bytesRead) == FR_OK
				&& bytesRead > 0) {
			outDataAppend(CheckSum(c));
			open_files[cur_fn].pos++;
		}
		outDataAppend(0x00);
		outDataAppend(out_checksum);
		outDataAppend(0x00);
		break;
	}
	}
}
SRAM2_DATA static uint8_t copyBuf[512];

void process_SET(void) {
	/* SET "X:NOMBRE.EXT","P" arrives as
	 *
	 *   0C 'X' ':' D A T O S ' ' ' ' ' ' '.' D A T  00 01 <checksum>
	 *
	 * The letter is not on the wire: byte 16 carries a flag, 1 for the
	 * protection the manual calls P - the same two trailing bytes OPEN uses
	 * for its mode and file number. Anything else clears it. */
	uint8_t flag = inDataBuf[16];
	getFileName();
	debug_log("SET <%s> %02X %02X\n", FileName, inDataBuf[15], flag);

	/* Measured: "P" sends 00 01 and " " sends FF FE - the complement, not a
	 * letter. Only those two values exist in this BASIC ("P" protects, a space
	 * clears, says the PC-E500 manual), so bit 0 tells them apart. A literal
	 * 'P' is accepted too, in case another model sends the character. */
	bool protect = ((flag & 0x01) != 0) || (flag == 'P');
	FRESULT res = f_chmod((char*) FileName, protect ? AM_RDO : 0, AM_RDO);
	if (res != FR_OK) {
		debug_log("f_chmod error %d\n", res);
		ERR_PRINTOUT("f_chmod error\n");
		outDataAppend(ERR_REPLY);
	} else {
		outDataAppend(CheckSum(0x00));
	}
}

void process_COPY(void) {
	/* Same shape as NAME: two 8.3 names, source at byte 3, destination at 17.
	 * The original drive had two units and could copy X: to Y:; here both
	 * names land on the same card, so the drive letters are ignored. */
	uint8_t tmpFile[16];
	char dstName[32];
	FIL src, dst;
	UINT br, bw;

	getFileName();
	strncpy((char*) tmpFile, (const char*) (inDataBuf + 17), 12);
	tmpFile[12] = '\0';
	trim(tmpFile);
	snprintf(dstName, sizeof(dstName), "%s%s", SD_HOME, (char*) tmpFile);
	debug_log("COPY <%s> -> <%s>\n", FileName, dstName);

	if (f_open(&src, (char*) FileName, FA_READ) != FR_OK) {
		ERR_PRINTOUT("COPY: source not found\n");
		outDataAppend(ERR_REPLY);
		return;
	}
	if (f_open(&dst, dstName, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		f_close(&src);
		ERR_PRINTOUT("COPY: cannot create destination\n");
		outDataAppend(ERR_REPLY);
		return;
	}

	FRESULT res = FR_OK;
	for (;;) {
		res = f_read(&src, copyBuf, sizeof(copyBuf), &br);
		if (res != FR_OK || br == 0) break;
		res = f_write(&dst, copyBuf, br, &bw);
		if (res != FR_OK || bw != br) {
			res = (res == FR_OK) ? FR_DISK_ERR : res;
			break;
		}
	}
	unsigned int copied = (unsigned int) f_size(&dst);
	f_close(&src);
	FRESULT cres = f_close(&dst);
	if (res == FR_OK && cres != FR_OK) res = cres;

	if (res != FR_OK) {
		debug_log("COPY failed %d\n", res);
		ERR_PRINTOUT("COPY error\n");
		outDataAppend(ERR_REPLY);
	} else {
		debug_log("COPY done (%u bytes)\n", copied);
		outDataAppend(CheckSum(0x00));
	}
}

void process_LOC(void) {
	/* LOC(n) - 1C <file number> <checksum>, three bytes, the file number in
	 * the same place INPUT# and PRINT# carry it.
	 *
	 * "The number of records read or written since the file was opened. One
	 * record is 256 bytes long", says the PC-E500 manual, so it is not a byte
	 * count. A file of 184 bytes read to the end is one record, hence the
	 * rounding up.
	 *
	 * The answer is status, a 16-bit value low byte first, and the checksum -
	 * four bytes. Not five like DSKF: sending the three-byte value it uses, the
	 * machine clocked out four bytes and stopped ("SO Err2 pos: 4"), taking our
	 * third value byte for the checksum and reporting I/O ERROR. And a single
	 * byte is not enough either - it sits on BUSY waiting for the rest. */
	uint8_t rawFn = inDataBuf[1];
	uint8_t fn = rawFn - 2;
	uint32_t records = 0;

	if (fn >= MAX_N_FILES || !open_files[fn].isOpen) {
		debug_log("LOC on #%u, not open\n", rawFn);
		outDataAppend(0xFF);
		outDataAppend(CheckSum(0x00));
		outDataAppend(CheckSum(0x00));
		outDataAppend(out_checksum);
		return;
	}

	uint32_t pos = (uint32_t) f_tell(&open_files[fn].fp);
	records = (pos + 255) / 256;
	debug_log("LOC #%u: %u records (%u bytes)\n", rawFn,
			(unsigned int) records, (unsigned int) pos);

	outDataAppend(CheckSum(0x00));
	outDataAppend(CheckSum(records & 0xff));
	outDataAppend(CheckSum((records >> 8) & 0xff));
	outDataAppend(out_checksum);
}

void process_LOF(void) {
	/* LOF(n) - 1B <file number> <checksum>, the same three-byte shape as LOC.
	 *
	 * "Returns the size of the specified file... in bytes", so unlike LOC this
	 * one can need more than 16 bits - a 54 KB program does not fit in two.
	 * Answered with the five-byte frame DSKF uses (status, three bytes of
	 * value, checksum), which is also how LOAD reports a file size. */
	uint8_t rawFn = inDataBuf[1];
	uint8_t fn = rawFn - 2;

	if (fn >= MAX_N_FILES || !open_files[fn].isOpen) {
		debug_log("LOF on #%u, not open\n", rawFn);
		outDataAppend(0xFF);
		outDataAppend(CheckSum(0x00));
		outDataAppend(CheckSum(0x00));
		outDataAppend(CheckSum(0x00));
		outDataAppend(out_checksum);
		return;
	}

	uint32_t size = (uint32_t) f_size(&open_files[fn].fp);
	debug_log("LOF #%u: %u bytes\n", rawFn, (unsigned int) size);

	outDataAppend(CheckSum(0x00));
	outDataAppend(CheckSum(size & 0xff));
	outDataAppend(CheckSum((size >> 8) & 0xff));
	outDataAppend(CheckSum((size >> 16) & 0xff));
	outDataAppend(out_checksum);
}

void process_NAME(void) {
	/* NAME "X:VIEJO.BAS" AS "NUEVO.BAS" arrives as two 8.3 names, each with
	 * its own drive letter - the Sharp puts the X: on the second one even
	 * when you do not type it:
	 *
	 *   0B 'X' ':' D A T O S ' ' ' ' ' ' '.' D A T
	 *      'X' ':' D A ' ' ' ' ' ' ' ' ' ' '.' D A T  <checksum>
	 *
	 * so the old name starts at byte 3 and the new one at byte 17. */
	uint8_t tmpFile[16];
	char newName[32];

	getFileName();                  /* the old name, from byte 3 */
	strncpy((char*) tmpFile, (const char*) (inDataBuf + 17), 12);
	tmpFile[12] = '\0';
	trim(tmpFile);
	snprintf(newName, sizeof(newName), "%s%s", SD_HOME, (char*) tmpFile);
	debug_log("NAME <%s> -> <%s>\n", FileName, newName);

	FRESULT res = f_rename((char*) FileName, newName);
	if (res != FR_OK) {
		debug_log("f_rename error %d\n", res);
		ERR_PRINTOUT("f_rename error\n");
		outDataAppend(ERR_REPLY);
	} else {
		outDataAppend(CheckSum(0x00));
	}
}

void process_EOF(void) {
	/* EOF(n) - has the file opened as n been read through?
	 *
	 * Never implemented, here or in the mbed firmware, where the line is
	 * written but commented out. It fell into the "unknown command" answer, a
	 * plain 0x00, which BASIC reads as "not the end yet": a read loop written
	 * as IF EOF(1) THEN ... never finished. That single byte IS the value, so
	 * 0x01 is the end of the file. */
	uint8_t rawFn = inDataBuf[1];
	uint8_t fn = rawFn - 2;
	if (fn >= MAX_N_FILES || !open_files[fn].isOpen) {
		debug_log("EOF on #%u, not open\n", rawFn);
		outDataAppend(CheckSum(0x01));  /* end, or the loop would never stop */
		return;
	}
	uint8_t atEnd = f_eof(&open_files[fn].fp) ? 1 : 0;
	debug_log("EOF #%u: %u (%u of %u)\n", rawFn, atEnd,
			(unsigned int) f_tell(&open_files[fn].fp),
			(unsigned int) f_size(&open_files[fn].fp));
	outDataAppend(CheckSum(atEnd));
}

void process_KILL(void) {
	uint8_t tmpFile[13];
	for (int i = 0; i < 12; i++) {
		tmpFile[i] = inDataBuf[3 + i];
	}
	tmpFile[12] = '\0';
	trim(tmpFile);
	snprintf((char*) FileName, sizeof(FileName), "%s%s", SD_HOME, tmpFile);
	debug_log("KILL <%s>\n", FileName);
	FRESULT ures = f_unlink((char*) FileName);
	if (ures == FR_OK) {
		outDataAppend(CheckSum(0x00));
	} else {
		/* FR_DENIED (7) here means the file carries the P attribute, which is
		 * what the manual says KILL must refuse. FR_NO_FILE (4) is a name that
		 * is not on the card. */
		debug_log("f_unlink error %d\n", ures);
		/* KILL of a file that is not there. The 0xFF answered before was
		 * taken as success by the Sharp: no message, exactly as if the file
		 * had been deleted. */
		debug_log("nothing to delete\n");
		outDataAppend(ERR_REPLY);
	}
}
void ProcessCommand(void) {
	out_checksum = 0;
	cmdComplete = false;
	uint8_t commandCode = inDataBuf[0];
	if (skipDeviceCode != 0)
		commandCode = skipDeviceCode;
	skipDeviceCode = 0;
	/* Mount here, once, for every command. OPEN, KILL, PRINT# and INPUT# never
	 * did it themselves, so they failed whenever they were the first thing
	 * asked after a reset - the same bug DSKF had, fixed in v2.6 for that one
	 * command only. The handlers that check sd_ready() still report a missing
	 * card their own way. */
	sd_ready();
	switch (commandCode) {
	case 0x03:
		process_OPEN();
		break;
	case 0x04:
		process_CLOSE();
		break;
	case 0x05:
		process_FILES();
		break;
	case 0x06:
		process_FILES_LIST(0);
		break;
	case 0x07:
		process_FILES_LIST(1);
		break;
	case 0x0A:
		process_KILL();
		break;
	case 0x0B:
		process_NAME();
		break;
	case 0x0C:
		process_SET();
		break;
	case 0x0D:
		process_COPY();
		break;
	case 0x0E:
		process_LOAD(0x0E);
		break;
	case 0x0F:
		process_LOAD(0x0F);
		break;
	case 0x10:
		process_SAVE(0x10);
		break;
	case 0x11:
		process_SAVE(0x11);
		break;
	case 0x16:
		process_SAVE(0x16);
		break;
	case 0xFE:
		process_SAVE(0xfe);
		break;
	case 0xFF:
		process_SAVE(0xff);
		break;
	case 0x12:
		process_LOAD(0x12);
		break;
	case 0x13:
		process_INPUT(0x13);
		break;
	case 0x14:
		process_INPUT(0x14);
		break;
	case 0x15:
		process_PRINT(0x15);
		break;
	case 0xFD:
		process_PRINT(0xfd);
		break;
	case 0x17:
		process_LOAD(0x17);
		break;
	case 0x1A:
		process_EOF();
		break;
	case 0x1B:
		process_LOF();
		break;
	case 0x1C:
		process_LOC();
		break;
	case 0x1D:
		process_DSKF();
		break;
	case 0x20:
		process_INPUT(0x20);
		break;
	default:
		debug_log("command 0x%02X - ", inDataBuf[0]);
		ERR_PRINTOUT("Unsupported (yet...)\n");
		/* Dump the frame. For a command nobody has implemented this is the
		 * only way to learn how it arrives: where the file number sits, how
		 * many names it carries and where. */
		debug_hex(inDataBuf, (inBufPosition < 32) ? inBufPosition : 32);
		debug_log("\n");
		outDataAppend(CheckSum(0x00));
		break;
	}
	/* Every command must answer something. A handler that bails out without
	 * putting a byte in the buffer sends nothing at all, and the Sharp waits
	 * on BUSY until BREAK is pressed - the failure mode is a hung machine
	 * rather than an error message, so make it an error. */
	if (outDataPutPosition == 0) {
		debug_log("no answer for 0x%02X - sending error\n", commandCode);
		outDataAppend(ERR_REPLY);
	}
	cmdComplete = true;
}