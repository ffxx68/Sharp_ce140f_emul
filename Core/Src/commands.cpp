#include "commands.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <cstdint>

extern void debug_log(const char *fmt, ...);
extern void debug_hex(volatile uint8_t *buf, volatile uint16_t len);
extern UART_HandleTypeDef huart2;
extern volatile uint16_t outDataGetPosition;

// Global structures
volatile uint8_t inDataBuf[IN_BUF_SIZE];
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

finfo_t open_files[MAX_N_FILES];

// Variables
uint8_t out_checksum = 0;
FIL staticFile;
bool staticFileOpen = false;
int fileCount;
uint8_t FileName[32];
int file_size;
int file_pos = 0;

// Target tracking macros
uint8_t sdmiso = 1; // Handled directly via FatFs f_mount checks
FATFS FatFs;

void print_to_pc(const char *msg) {
	HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), HAL_MAX_DELAY);
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

	debug_log("FILES\n");
	outDataAppend(CheckSum(0x00));

	if (f_mount(&FatFs, "0:", 1) != FR_OK) {
		ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		outDataAppend(CheckSum(0x00));
		outDataAppend(out_checksum);
		return;
	}

	if (f_opendir(&dir, SD_HOME) == FR_OK) {
		while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0
				&& n_files < 0xFF) {
			if (strstr(fno.fname, "."))
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

	uint8_t tx = 'f';
	HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
	tx = 0x30 + cmd;
	HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
	tx = '\n';
	HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);

	debug_log("FILES_LIST 0x%02X\n", cmd);
	outDataAppend(0x00);
	out_checksum = 0;

	if (f_mount(&FatFs, "0:", 1) != FR_OK) {
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

	if (f_opendir(&dir, SD_HOME) == FR_OK) {
		while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0
				&& n_files < 0xFF) {
			if (strstr(fno.fname, ".")) {
				n_files++;
			}
			if (n_files == fileCount)
				break;
		}

		debug_log("<%s>\n", fno.fname);
		strcpy((char*) tmp, fno.fname);
		char *p = strstr(fno.fname, ".");
		char *s = fno.fname;
		if (p) {
			char clearTmp[16];
			strncpy(clearTmp, s, (p - s));
			clearTmp[p - s] = 0x00;
			uint8_t *uTmp = (uint8_t*) clearTmp;
			trim(uTmp);
			sprintf((char*) FileName, "X:%-8s%4s ", (char*) uTmp, p);
		} else {
			outDataAppend(0xFF);
			ERR_PRINTOUT(" ERR clean\n");
		}
		debug_log("formatted <%s>\n", FileName);
		sendString((char*) FileName);
		outDataAppend(out_checksum);
		f_closedir(&dir);
	}
}

void process_LOAD(uint8_t cmd) {
	debug_log("LOAD 0x%02X\n", cmd);
	BYTE c = 0;
	UINT bytesRead;
	uint8_t tmpFile[16];

	out_checksum = 0;
	if (f_mount(&FatFs, "0:", 1) != FR_OK) {
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
		uint8_t tx = 'l';
		HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
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
			ERR_PRINTOUT("f_open error\n");
			break;
		}
		staticFileOpen = true;
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
		uint8_t tx = '0';
		HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
		if (f_read(&staticFile, &c, 1, &bytesRead) == FR_OK && bytesRead > 0) {
			file_pos++;
			outDataAppend(0x00);
			debug_log("first byte 0x%02X\n", c);
			outDataAppend(CheckSum(c));
			outDataAppend(out_checksum);
		} else {
			ERR_PRINTOUT("f_read EOF");
			outDataAppend(0xff);
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		}
		break;
	}
	case 0x12: {
		uint8_t tx = 'a';
		HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
		outDataAppend(0x00);
		if (!staticFileOpen) {
			outDataAppend(CheckSum(0x1A));
			outDataAppend(out_checksum);
			outDataAppend(0x00);
			break;
		}
		do {
			if (f_read(&staticFile, &c, 1, &bytesRead) != FR_OK
					|| bytesRead == 0) {
				c = 0xFF; // Mirror EOF state
			} else {
				file_pos++;
				outDataAppend(CheckSum(c));
			}
		} while ((bytesRead > 0) && (c != 0x0d));

		if (c != 0x0d) {
			debug_log("EOF\n");
			outDataAppend(CheckSum(0x1A));
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
		uint8_t tx = '.';
		HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
		outDataAppend(0x00);
		uint16_t data_start = file_pos;
		do {
			if (f_read(&staticFile, &c, 1, &bytesRead) == FR_OK
					&& bytesRead > 0) {
				file_pos++;
				outDataAppend(CheckSum(c));
				if (((file_pos - data_start) % 0x100) == 0) {
					outDataAppend(out_checksum);
					out_checksum = 0;
				}
			} else {
				break;
			}
		} while (file_pos < file_size);
		outDataAppend(out_checksum);
		outDataAppend(0x00);
		if (file_pos == file_size) {
			debug_log("file complete (file_size %d)\n", file_size);
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		}
		break;
	}
	}
}

void process_SAVE(int cmd) {
	debug_log("SAVE 0x%02X\n", cmd);
	UINT bytesWritten;

	out_checksum = 0;
	if (f_mount(&FatFs, "0:", 1) != FR_OK) {
		ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
		outDataAppend(0xFF);
		return;
	}

	switch (cmd) {
	case 0x10: {
		uint8_t tx[2] = { 's', '0' };
		HAL_UART_Transmit(&huart2, tx, 2, HAL_MAX_DELAY);
		getFileName();
		if (staticFileOpen) {
			f_close(&staticFile);
			staticFileOpen = false;
		}
		if (f_open(&staticFile, (char*) FileName, FA_CREATE_ALWAYS | FA_WRITE)
				!= FR_OK) {
			ERR_PRINTOUT("f_open write error\n");
			outDataAppend(0xFF);
			break;
		}
		staticFileOpen = true;
		file_pos = 0;
		outDataAppend(0x00);
		break;
	}
	case 0x11: {
		uint8_t tx[2] = { 's', '1' };
		HAL_UART_Transmit(&huart2, tx, 2, HAL_MAX_DELAY);
		file_size = (int) inDataBuf[2] + (int) (inDataBuf[3] << 8)
				+ (int) (inDataBuf[4] << 16);
		debug_log("filesize: %d\n", file_size);
		outDataAppend(0x00);
		skipDeviceCode = 0xFF;
		break;
	}
	case 0x16: {
		uint8_t tx[2] = { 's', '6' };
		HAL_UART_Transmit(&huart2, tx, 2, HAL_MAX_DELAY);
		outDataAppend(0x00);
		file_pos = 0;
		skipDeviceCode = 0xFE;
		break;
	}
	case 0xFF: {
		uint8_t tx = '.';
		HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
		int buf_pos = 0;
		skipDeviceCode = 0xFF;
		while (buf_pos < inBufPosition - 1) {
			f_write(&staticFile, (void*) &inDataBuf[buf_pos], 1, &bytesWritten);
			buf_pos++;
			file_pos++;
		}
		if (file_pos == file_size) {
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
			debug_log("file done\n");
			skipDeviceCode = 0x00;
		}
		outDataAppend(0x00);
		break;
	}
	case 0xFE: {
		uint8_t tx = '.';
		HAL_UART_Transmit(&huart2, &tx, 1, HAL_MAX_DELAY);
		int buf_pos = 0;
		if (inDataBuf[buf_pos] == 0x1A) {
			debug_log("file done\n");
			if (staticFileOpen) {
				f_close(&staticFile);
				staticFileOpen = false;
			}
		} else {
			while (buf_pos < inBufPosition - 1) {
				f_write(&staticFile, (void*) &inDataBuf[buf_pos], 1,
						&bytesWritten);
				buf_pos++;
				file_pos++;
			}
		}
		outDataAppend(0x00);
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
		if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
			uint64_t freeMb = (uint64_t) fs->csize * (uint64_t) fs->free_clst
					* 512 / 1048576;
			diskspace = (uint32_t) freeMb;
			debug_log("SD free Mb %d\n", diskspace);
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
	} else {
		fn = fn - 2;
		if (open_files[fn].isOpen) {
			f_close(&open_files[fn].fp);
			open_files[fn].isOpen = false;
		}
	}
	outDataAppend(CheckSum(0x00));
}
void process_OPEN(void) {
	uint8_t mode = inDataBuf[15];
	uint8_t fn = inDataBuf[16] - 2;
	getFileName();
	debug_log("OPEN <%s> FOR '%d' AS #%d\n", FileName, mode, fn + 2);
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
		ERR_PRINTOUT("fopen error\n");
		outDataAppend(0xFF);
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
		skipDeviceCode = 0xFD;
		outDataAppend(0x00);
		break;
	case 0xFD:
		int buf_pos = 0;
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
void process_KILL(void) {
	uint8_t tmpFile[13];
	for (int i = 0; i < 12; i++) {
		tmpFile[i] = inDataBuf[3 + i];
	}
	tmpFile[12] = '\0';
	trim(tmpFile);
	snprintf((char*) FileName, sizeof(FileName), "%s%s", SD_HOME, tmpFile);
	if (f_unlink((char*) FileName) == FR_OK) {
		outDataAppend(CheckSum(0x00));
	} else {
		outDataAppend(0xFF);
	}
}
void ProcessCommand(void) {
	out_checksum = 0;
	cmdComplete = false;
	uint8_t commandCode = inDataBuf[0];
	if (skipDeviceCode != 0)
		commandCode = skipDeviceCode;
	skipDeviceCode = 0;
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
	case 0x1D:
		process_DSKF();
		break;
	case 0x20:
		process_INPUT(0x20);
		break;
	default:
		outDataAppend(CheckSum(0x00));
		break;
	}
	cmdComplete = true;
}