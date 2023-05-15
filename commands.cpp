#include "commands.h"
#include "SDFileSystem.h"
#include <cstdint>

// from other modules
extern void debug_log(const char *fmt, ...);
extern void debug_hex(volatile uint8_t *buf, volatile uint16_t len);
extern void outDebugDump( void );
extern RawSerial         pc;
extern volatile uint16_t outDataGetPosition;

// shared over different threads
volatile uint8_t     inDataBuf[IN_BUF_SIZE];
volatile uint8_t     outDataBuf[OUT_BUF_SIZE];
volatile uint16_t    inBufPosition;
volatile uint16_t    inBufStart;
volatile uint16_t    outDataPutPosition;
volatile bool        cmdComplete;
volatile uint8_t     skipDeviceCode = 0;

// open file pointers
typedef struct {
    uint8_t fn;
    uint8_t mode;
    FILE* fp;
    uint16_t pos;
} finfo_t ;

finfo_t open_files[MAX_N_FILES];

// locals
uint8_t  out_checksum = 0;
FILE    *fp;
int      fileCount; 
uint8_t  FileName[17];
int      file_size;
int      file_pos = 0;

// SD Card (SDFileSystem library)
#if defined TARGET_NUCLEO_L053R8
//DigitalIn    sdmiso(PB_4);
uint8_t sdmiso  = 1; // probing pin doesn't work! 
SDFileSystem sd(PB_5, PB_4, PB_3, PA_10, "sd"); // mosi, miso, sclk, cs
#endif
#if defined TARGET_NUCLEO_L432KC
//DigitalIn    sdmiso(PA_6);
uint8_t sdmiso  = 1; // probing pin doesn't work! 
// NOTE - SB16 and SB18 has to be open (on board back), to use PA5 and PA6
SDFileSystem sd(PA_7, PA_6, PA_5, PB_5, "sd"); // mosi, miso, sclk, cs
#endif

uint8_t CheckSum(uint8_t b) {
    out_checksum = (out_checksum + b) & 0xff;
    return b;
}

#ifdef ASYNCHOUT
void outDataAppend(uint8_t b) {
    if ( (outDataPutPosition++) == OUT_BUF_SIZE ) {
        // buffer full - hold until the spooler has reached buffer end
        // (a timeout should be added! in case the spooler hangs...)
        while ( outDataGetPosition < outDataPutPosition ) 
            wait(.1); // wait
        // spool complete - reset buffer positions
        outDataPutPosition = 0;
        outDataGetPosition = 0;
    }
    // ok - add byte to send
    outDataBuf[ outDataPutPosition ] = b;
}
#else
void outDataAppend(uint8_t b) {

    // We should check for buffer full!!
    outDataBuf[ outDataPutPosition ++ ] = b;
        
}
#endif

void sendString(char* s) {
    for (int i=0;i<strlen((char*)s);i++) {
        outDataAppend(CheckSum(s[i]));
    }
}

// in-place whitespace removal
void trim(uint8_t* s) {
    uint8_t* d = s;
    do {
        while (*d == ' ') {
            ++d;
        }
    } while (*s++ = *d++);
}

uint8_t *formatFileName(char *s) {
    uint8_t tmp[15];
    const char *p = strstr(s, ".BAS");
    if (p) {
        strncpy ((char*)tmp, (const char*)s, (p-s));
        trim(tmp); // shouldn' be needed... files are stored on SD without blanks
        sprintf ((char*)FileName, "X:%-8s.BAS ", tmp ); // '%-8s' pads to 8 blanks
        return FileName;
    } else
        return NULL;
}

void process_FILES_LIST(uint8_t cmd) {
    // QString fname;
    struct dirent* ent;
    DIR *dir;
    int n_files = -1;
    uint8_t *ext_pos;
    uint8_t tmp[15];

    pc.putc('f');pc.putc(0x30+cmd);pc.putc('\n');
    debug_log ("FILES_LIST 0x%02X\n", cmd);
    outDataAppend(0x00);
    out_checksum=0;
    if ( sdmiso == 0 ) {
        ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
        outDataAppend(0xFF); // returning an error to Sharp?
        outDataAppend(out_checksum);
        return;
    }

    switch (cmd) {
    case 0:
            fileCount++; // current file number
            break;
    case 1:
            fileCount--;
            break;
    }
    debug_log ("file # %d\n", fileCount);
    if ((dir = opendir (SD_HOME)) != NULL) { 
        // browse all the files, and stop at 'fileCount'
        // same loop as in process_FILES (which only counts them)
        // NOTE - We browse the entire SD-card directory each time
        // so to avoid keeping in memory the entire file list
        // (256 files x 15 = potentially up to 3Kb)
        while ((ent = readdir (dir)) != NULL
            && n_files < 0xFF  // max 255 files 
            ) { 
            //debug_log("<%s>\n", ent->d_name);
            if ( (ext_pos = (uint8_t *)strstr (ent->d_name, ".")) != 0 ) {
                n_files++;
                //debug_log("BAS %d\n", n_files);
            }
            if ( n_files == fileCount )
                break; // this is the BAS file we're looking for
        }
        // 'ent' now points to current file
        // file name expected like "X:A       .BAS "
        debug_log("<%s>\n", ent->d_name);
        strcpy((char*)tmp, ent->d_name);
        const char *p = strstr(ent->d_name, ".");
        const char *s = ent->d_name;
        if (p) {
            strncpy ((char*)tmp, s, (p-s));
            tmp[(p-s)] = 0x00;
            trim(tmp); // shouldn't be needed... files stored on SD without blanks
            sprintf ((char*)FileName, "X:%-8s%4s ",(char*)tmp, p); // '%-8s' pads to 8 blanks
            debug_log("<%s>\n", ent->d_name);
        } else {
            outDataAppend(0xFF); // send err back
            ERR_PRINTOUT(" ERR clean\n");
        }
        debug_log("formatted <%s>\n", FileName);
        // send formatted file name
        sendString((char*)FileName);
        outDataAppend(out_checksum);
        closedir (dir);
    }
}

void process_FILES(void) {
    
    struct dirent* ent;
    DIR *dir;
    int n_files = 0x00;
    uint8_t *ext_pos;
    
    debug_log ( "FILES\n" ); 
    outDataAppend(CheckSum(0x00));
    if ( sdmiso == 0 ) {
        ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
        outDataAppend(CheckSum(0x00)); // no files
        outDataAppend(out_checksum);
        return;
    }
    // file name wildcards (* ?) to be handled, yet ...
    if ((dir = opendir (SD_HOME)) != NULL) { // ignore files other than BASIC
        // print all the files and directories within directory
        while ((ent = readdir (dir)) != NULL
            && n_files < 0xFF ) { // max 255 files
            //debug_log("<%s>\n", ent->d_name);
            if ( (ext_pos = (uint8_t *)strstr (ent->d_name, ".")) != 0 ) 
                n_files++; 
        }
        closedir (dir);
        fileCount = -1;
        if ( n_files > 255 ){
            ERR_PRINTOUT("Number of files greater than 255!\n");
            outDataAppend(0x00);
        } else {
            debug_log("%d files\n", n_files);
            outDataAppend(CheckSum(n_files));
        }
    } else {
        // could not open directory
        ERR_PRINTOUT("Could not open SD home directory!\n");
        outDataAppend(0x00);
    }
    outDataAppend(out_checksum);
}

bool file_exists (char *filename) {
    FILE *file;
    if ((file = fopen(filename, "r")))
    {
        fclose(file);
        return true;
    }
    return false;
}

int getFileSize(FILE *fp) {
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    return size;
};

void process_LOAD(uint8_t cmd) {
    debug_log ( "LOAD 0x%02X\n", cmd); 
    int c=0;
    uint8_t tmpFile[16];

    out_checksum = 0;
    if ( sdmiso == 0 ) {
        ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
        outDataAppend(0x00); 
        sendString(" "); // ?
        outDataAppend(0x00);
        outDataAppend(0x00); 
        outDataAppend(0x00); // 0-size file
        outDataAppend(out_checksum);
        return;
    }
    switch (cmd) {
        case 0x0E:  { // open file and send file size
            pc.putc('l');
            /*
            for (int i=3;i<15;i++) {
                s.append(Quint8_t(data.at(i)));
            }
            file_load.setFileName(s);
            if (!file_load.open(QIODevice::ReadOnly)) {
                emit msgError(tr("ERROR opening file : %1").arg(s));
            }
            */
            strncpy ((char*)tmpFile, (const char *)(inDataBuf+3), 12);
            tmpFile[12] = '\0'; // terminate
            trim (tmpFile); // remove blanks
            sprintf ((char*)FileName, "%s%s", SD_HOME, tmpFile);
            //pc.printf((char*)FileName);
            debug_log ( "opening <%s>\n", FileName );
            if ( fp != NULL ) fclose ( fp ); // just in case...
            fp = fopen((char*)FileName, "r"); // this needs to stay open until EOF
            if ( fp == NULL ) {
                ERR_PRINTOUT("fopen error\n");
                break;
            }
            file_size = getFileSize(fp);
            if ( file_size <= 0 ) {
                ERR_PRINTOUT("getFileSize error\n");
                if ( fp != NULL ) fclose ( fp );
                pc.putc('x');
                break;
            }    
            debug_log ( "size %d\n", file_size);
            file_pos = 0;
            outDataAppend(0x00);
            sendString(" "); // ?
            // Send file size : 3 bytes (optimistic!) + checksum
            outDataAppend(CheckSum(file_size & 0xff));
            outDataAppend(CheckSum((file_size >> 8) & 0xff));
            outDataAppend(CheckSum((file_size >> 16) & 0xff));
            outDataAppend(out_checksum);
            break;
        }
        case 0x17: { // send header bytes
            // header for non-ASCII files is read from 0xFF to 0x0F 
            // sending several 0x17 commands, one byte each.
            // For an ASCII file the header is nonexistent
            // and first 0x17 command will returns a uint8_t != 0xFF
            pc.putc('0');
            //ba_load.remove(0,0x0f); // remove first byte 'ff'
            //ba_load.chop(1);
            c = fgetc(fp);
            file_pos++;
            if ( c != EOF ) {
                outDataAppend(0x00);
                debug_log ( "first byte 0x%02X\n", c);
                outDataAppend(CheckSum(c));
                outDataAppend(out_checksum); 
            } else {
                ERR_PRINTOUT("fgetc EOF");
                outDataAppend(0xff); // error to Sharp
                if ( fp != NULL ) fclose ( fp );
            }
            //ba_load.remove(0,0x10);
            //wait_data_function = 0xfd;
            break;
        }        
        case 0x12: { // ASCII data chunk (one line max)
            pc.putc('a');
            // Envoyer une ligne complete
            // Until end-of-line 0x0D
            // or end-of-file (EOF)
            // Start at 0x10 ??
            outDataAppend(0x00);
            do {
                c=fgetc(fp);
                file_pos++;
                outDataAppend(CheckSum(c));
            } while ((c != EOF) && (c!=0x0d));
            if (c!=0x0d) {
                if (c == EOF) {
                    debug_log ("EOF\n");
                    outDataAppend(CheckSum(0x1A));  // 0x1A pour fin de fichier
                    if ( fp != NULL ) fclose ( fp );
                }
            } else
                debug_log ("line\n");
            outDataAppend(out_checksum);
            outDataAppend(0x00);
            break;
        }
        case 0x0f: { // non-ASCII data stream (single chunk)
            pc.putc('.');
            outDataAppend(0x00);
            uint16_t data_start = file_pos;
            do {
                c=fgetc(fp);
                file_pos++;
                //debug_log (" %02X\n", c);
                outDataAppend(CheckSum(c));
                if (((file_pos-data_start)%0x100)==0) {
                    outDataAppend(out_checksum);
                    out_checksum=0;
                }
            } while ( c != EOF && file_pos < file_size );
            outDataAppend(out_checksum);
            outDataAppend(0x00);
            if ( c==EOF && file_pos != file_size ) {
                ERR_PRINTOUT("fgetc error during LOAD");
                if ( fp != NULL ) fclose ( fp );
                // how to tell Sharp-PC to stop sending more LOAD commands?
            }
            if ( file_pos == file_size ) {
                debug_log ("file complete (file_size %d)\n", file_size);
                if ( fp != NULL ) fclose ( fp );  
            } 
            break;

        }
        default: {
            ERR_PRINTOUT("unknown LOAD sub-command\n");
            if ( fp != NULL ) fclose ( fp );
            break;
        }
    }
}

FILE* openWriteFile( void ){
    // create (or replace?) file
    debug_log ("creating <%s>\n", FileName );
    if ( file_exists ( (char*)FileName ) ) {
        // file exists, remove it
        int r = remove ( (char*)FileName );
        debug_log ("remove: %d\n", r);
    }
    if ( fp != NULL ) {
        int r = fclose ( fp ); // just in case...
        debug_log ("fclose: %d\n", r);
    }
    return fp = fopen((char*)FileName, "w"); // stay open until command complete
}

void getFileName( void ) {
    uint8_t tmpFile[16];
    strncpy ((char*)tmpFile, (const char *)(inDataBuf+3), 12 );
    tmpFile[12]=0; // terminate string
    trim (tmpFile); // remove blanks, for the SD card
    sprintf ((char*)FileName, "%s%s", SD_HOME, tmpFile);
    debug_log ("SDcard filename: %s\n", FileName);
}

void process_SAVE(int cmd) {
    debug_log ( "SAVE 0x%02X\n", cmd);
    int c=0;

    out_checksum = 0;
    if ( sdmiso == 0 ) {
        ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
        outDataAppend(0xFF); 
        return;
    }
    switch (cmd) {
        case 0x10: { // file name : create (or replace)
            pc.putc('s');pc.putc('0');
            getFileName();
            if ( openWriteFile() == NULL ) {
                ERR_PRINTOUT( "openWriteFile error\n");
                outDataAppend(0xFF); // NOT ok!
                break;
            }
            file_pos = 0;
            outDataAppend(0x00); // ok, done
            break;
        }
        case 0x11: { // file size (non-ASCII)
            pc.putc('s');pc.putc('1');
            if ( file_pos != 0 ) {
                // unexpected 0x11 here
                ERR_PRINTOUT("unexpected 0x11 @%d");
                if (fp != NULL ) fclose (fp);
                outDataAppend(0xFF); // return with error
                break;
            }
            // 6 bytes, for file size
            // 0 : 0x11 (command)
            // 1 : 00
            // 2 : Size 1
            // 3 : Size 2
            // 4 : Size 3
            // 5 : checksum 
            file_size = (int)inDataBuf[2] + (int)(inDataBuf[3]<<8) + (int)(inDataBuf[4]<<16);
            // debug_log (" 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
            //     inDataBuf[0],inDataBuf[1],inDataBuf[2],
            //     inDataBuf[3],inDataBuf[4],inDataBuf[5], inDataBuf[6] );
            debug_log ("filesize: %d\n", file_size);
            outDataAppend(0x00); // ok, got size and file open
            // next command, without a device-code sequence
            skipDeviceCode = 0xFF;
            break;
        }
        case 0x16: { // ASCII data stream
            pc.putc('s');pc.putc('6');
            outDataAppend(0x00); // ok, file open
            file_pos = 0;
            // next command, without a device-code sequence
            skipDeviceCode = 0xFE;
            break;
        }
        case 0xFF: { // save file data block (non-ASCII)
            pc.putc('.');
            int buf_pos = 0;
            skipDeviceCode = 0xFF; 
            if ( fp == NULL ) {
                    ERR_PRINTOUT( "file not open\n");
                    outDataAppend(0xFF); // NOT ok!
                    break;
            }
            debug_log ("inDataBuf size %d\n", inBufPosition);
            while ( buf_pos < inBufPosition - 1 ) { // last byte is checksum
                fputc ( (int)(inDataBuf[buf_pos]), fp) ;
                buf_pos ++;
                file_pos ++;
            }
            debug_log ("file_pos %d file_size %d\n", file_pos, file_size);
            if ( file_pos == file_size ) {
                fclose (fp); // done
                debug_log ("file done\n");
                skipDeviceCode = 0x00;
            }
            outDataAppend(0x00); // ok
            break;
        }
        
        case 0xFE: { // ASCII file block (one line)
            pc.putc('.');
            int buf_pos = 0;
            if ( fp == NULL ) {
                ERR_PRINTOUT( "file not open\n");
                outDataAppend(0xFF); // NOT ok!
                break;
            }
            //debug_log ("<%s>\n", inDataBuf);
            if ( inDataBuf[buf_pos] == 0x1A ) { // file end (to store it as well?)
                debug_log ("file done\n");
                fclose (fp);
            } else {
                // store one line as is (including line termination 0x0D+0x0A)
                while ( buf_pos < inBufPosition - 1 ) { // last byte is checksum
                    fputc ((int)(inDataBuf[buf_pos]), fp) ;
                    buf_pos ++;
                    file_pos ++;
                }
            }
            outDataAppend(0x00);
            break;
        }
        default: {
            ERR_PRINTOUT("unknown SAVE sub-command\n");
            if ( fp != NULL ) fclose ( fp );
            break;
        }
    }
}

void process_DSKF(void) {
        int diskspace = 4680; // 20488; // test examples
        debug_log ( "DSKF\n" ); 
        debug_log ( "diskspace %d\n",  diskspace);

        // we should calculate actual disk free space here...
        outDataAppend(CheckSum(0x00));
        outDataAppend(CheckSum(diskspace & 0xff));  // number of bytes 
        outDataAppend(CheckSum((diskspace >> 8) & 0xff));  // number of 256Bytes free sectors
        outDataAppend(CheckSum((diskspace >> 16) & 0xff));
        outDataAppend(out_checksum);      
}    

void process_CLOSE( void ) {
        uint8_t fn = inDataBuf[1];
        debug_log ( "CLOSE 0x%02X\n", fn);
        out_checksum = 0;
        // a CLOSE 0xFF (on all files) is issued also at RUN
        if ( fn == 0xFF )
            for  (int i=0; i<MAX_N_FILES; i++) {
                if ( open_files[i].fp != NULL ) 
                    fclose ( open_files[i].fp );
                open_files[i].fp = NULL;
                open_files[i].mode = 0;
                open_files[i].pos = 0;
            }
        else {
            fn = fn - 2; // array index
            if ( open_files[fn].fp != NULL ) {
                fclose ( open_files[fn].fp );
                open_files[fn].fp = NULL;
                open_files[fn].mode = 0;
                open_files[fn].pos = 0; 
            }
            else
                ERR_PRINTOUT("file not open");
        }
        outDataAppend(CheckSum(0x00));
}  

void process_OPEN( void ) {
    FILE* fp;
    uint8_t mode = inDataBuf[15]; // 1: input, 2: output, 3: append
    uint8_t fn = inDataBuf[16]-2; // file#

    getFileName();
    debug_log ( "OPEN <%s> FOR '%d' AS #%d\n",FileName,mode,fn+2);

    // file # from 2, used as file info array index
    if ( fn<0||fn>MAX_N_FILES ) {
        ERR_PRINTOUT( "Invalid file #\n");
        outDataAppend(0xFF); // NOT ok!
    }
    if ( open_files[fn].fp != NULL ) 
        fclose ( open_files[fn].fp ); // just in case...
    switch ( mode ) {
        case 1:{
            // for 'input'          
            fp = fopen((char*)FileName, "r"); // If the file exists already, contents overwritten
            break;
        } 
        case 2:{
            // for 'output'
            fp = fopen((char*)FileName, "w"); // If the file exists already, contents overwritten
            break;
        }         
        case 3:{
            // for 'append'
            // Sharp expects an error if file don't exists
            if ( !file_exists ( (char*)FileName ) ) {
                ERR_PRINTOUT("append no file\n");
                outDataAppend(0xFF);
                break;
            }
            fp = fopen((char*)FileName, "a"); // appending to exisiting file (nee)
            break;
        } 
    }
    if ( fp == NULL ) {
        ERR_PRINTOUT("fopen error\n");
        outDataAppend(0xFF);
    } else {
        open_files[fn].fp = fp;
        open_files[fn].mode = mode;
        open_files[fn].pos = 0;
        // done
        outDataAppend(CheckSum(0x00));    
    }     
}

uint8_t cur_fn ;
void process_PRINT( int cmd ) {

    debug_log ( "PRINT 0x%02X\n", cmd);
    // Similar to SAVE - should move common parts to functions
    switch (cmd) {
        case 0x15: { 
            // file for current command
            cur_fn = inDataBuf[1]-2; // file# from 2
            debug_log ( "file #%d 0x%02X '%d' @ %d\n", 
                cur_fn+2, 
                open_files[cur_fn].fp,
                open_files[cur_fn].mode, 
                open_files[cur_fn].pos);

            // check if file mode is coherent with PRINT?

            skipDeviceCode = 0xFD; // next PRINT sub-command
            outDataAppend(0x00); // ok, done
            break;
        }
        case 0xFD: {
            int buf_pos = 0;
            debug_log ( " current file #%d\n", cur_fn+2); 
            debug_log ( " inBufPosition %d\n", inBufPosition); 
            // skip empty message (CRLF only)
            if (!(inDataBuf[1] == 0x0A && inDataBuf[0] == 0x0D)) {
                // similar to SAVE
                while ( buf_pos < inBufPosition - 2 ) { // omit 0x00+checksum
                    // skip an empty message (CRLF only)
                    if (!(  (buf_pos == 0 && inDataBuf[buf_pos] == 0x0D)
                        ||(buf_pos == 1 && inDataBuf[buf_pos] == 0x0A)))
                        fputc ((int)(inDataBuf[buf_pos]), open_files[cur_fn].fp) ;
                    buf_pos ++;
                    open_files[cur_fn].pos++; // store current file position in the array
                }
                if ( inDataBuf[inBufPosition-3] != 0x0A ) {
                    // append line termination, when missing from the message
                    debug_log ( " appending CFLF\n");
                    fputc (0X0D, open_files[cur_fn].fp); 
                    fputc (0X0A, open_files[cur_fn].fp);
                }
            }
            outDataAppend(CheckSum(0x00));
            break;
        }
        default: {
            ERR_PRINTOUT( "openWriteFile error\n");
            outDataAppend(0xFF); // NOT ok!
        }
    }
}

void process_INPUT( int cmd ) {
    debug_log ( "INPUT 0x%02X\n", cmd);
    // file# for current command
    cur_fn = inDataBuf[1]-2;
    debug_log ( "file #%d 0x%02X '%d' @ %d\n", 
        cur_fn+2, 
        open_files[cur_fn].fp,
        open_files[cur_fn].mode, 
        open_files[cur_fn].pos);
    
    // check if file mode is coherent with INPUT?
    // Similar to LOAD (ascii) - move common parts to functions?
    switch (cmd) {
        case 0x13: // string
        case 0x14: // number
        case 0x20: // number array
        { 
            outDataAppend(0x00);
            char c;
            char line [82];
            line[0]=0x00;
            // Similar to a 'LOAD ascii' (one line)
            do {
                c=fgetc(open_files[cur_fn].fp);
                if ( c == 0xFF ) {
                    ERR_PRINTOUT( "fgetc 0xFF\n");
                    outDataAppend(0xFF);
                    break;        
                }
                strncat (line,&c,1);
                open_files[cur_fn].pos++;
            } while ((c != EOF) && (c!=0x0A)); // line ends with 0D+0A
            if (c == EOF)
                debug_log ("EOF!\n");
            else
                debug_log ("line: <%s>\n", line);            sendString(line);
            outDataAppend(0x00);
            outDataAppend(out_checksum);
            outDataAppend(0x00);
            break;
        }
        default: {
            ERR_PRINTOUT( "unknown INPUT sub-command\n");
            outDataAppend(0xFF); // NOT ok!
        }
    }
}

void process_KILL( void ) {
    uint8_t tmpFile[13];
    debug_log ( "process_KILL\n");
    strncpy ((char*)tmpFile, (const char *)(inDataBuf+3), 12);
    tmpFile[12] = '\0'; // terminate
    trim (tmpFile); // remove blanks
    sprintf ((char*)FileName, "%s%s", SD_HOME, tmpFile);
    debug_log ( "KILL <%s>\n", FileName );
    if ( file_exists ( (char*)FileName ) ) {
        int r = remove ( (char*)FileName );
        debug_log ("remove: %d\n", r);
        outDataAppend(CheckSum(0x00));
    } else {
        ERR_PRINTOUT("file not present\n");
        outDataAppend(0xFF);
    }
} 


void ProcessCommand ( void ) {

    out_checksum = 0;
    cmdComplete = false;

    uint8_t commandCode = inDataBuf[0];
    if (skipDeviceCode != 0 )
        commandCode = skipDeviceCode;
    skipDeviceCode = 0;

    switch (commandCode) {
    case 0x03: process_OPEN();break;
    case 0x04: process_CLOSE();break;
    case 0x05: process_FILES();break;
    case 0x06: process_FILES_LIST(0);break;
    case 0x07: process_FILES_LIST(1);break;
    /* 
    case 0x08: process_INIT(0x08);break;
    case 0x09: process_INIT(0x09);break;
    */
    case 0x0A: process_KILL();break;
        //    case 0x0B: process_NAME(0x0B);break;
        //    case 0x0C: process_SET(0x0C);break;
        //    case 0x0D: process_COPY(0x0D);break;
    case 0x0E: process_LOAD(0x0E);break;
    case 0x0F: process_LOAD(0x0F);break;
    case 0x10: process_SAVE(0x10);break;
    case 0x11: process_SAVE(0x11);break;
    case 0x16: process_SAVE(0x16);break;    // SAVE ASCII
    case 0xFE: process_SAVE(0xfe);break;    // next SAVE ascii cmd
    case 0xFF: process_SAVE(0xff);break;    // next SAVE cmd
    case 0x12: process_LOAD(0x12);break;
    case 0x13: process_INPUT(0x13);break; // INPUT #x, X$
    case 0x14: process_INPUT(0x14);break; // INPUT #x, X
    case 0x15: process_PRINT(0x15);break;
    case 0xFD: process_PRINT(0xfd);break; // next PRINT cmd
    case 0x17: process_LOAD(0x17);break;
        //    case 0x1A: process_EOF(0x1A);break;
        //    case 0x1C: process_LOC(0x1C);break;
    case 0x1D: process_DSKF(); break;
        //    case 0x1F: process_INPUT(0x1f);break;
    case 0x20: process_INPUT(0x20);break;
    default:
        pc.printf(" command 0x%02X - ", inDataBuf[0]);
        ERR_PRINTOUT( "Unsupported (yet...)\n" ); 
        outDataAppend(CheckSum(0x00));
        break;
    }

    // command complete
    cmdComplete = true;
}
