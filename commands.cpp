#include "commands.h"
#include "SDFileSystem.h"
#include <cstdint>


// from other modules
extern void debug_log(const char *fmt, ...);
extern void debug_hex(volatile uint8_t *buf, volatile uint16_t len);
extern void debugOutput( void );
extern RawSerial         pc;
extern volatile uint16_t outDataGetPosition;

// shared over different threads
volatile uint8_t     inDataBuf[IN_BUF_SIZE];
volatile uint8_t     outDataBuf[OUT_BUF_SIZE];
volatile uint16_t    inBufPosition;
volatile uint16_t    outDataPutPosition;
volatile bool        cmdComplete;
volatile uint8_t     skipDeviceCode = 0;

// locals
uint8_t  out_checksum = 0;
FILE    *fp;
int      fileCount; 
uint8_t  FileName[15];
int      file_size;
int      file_pos = 0;

// SD Card (SDFileSystem library)
#if defined TARGET_NUCLEO_L053R8
//DigitalIn    sdmiso(PB_4);
uint8_t sdmiso  = 1; // probing pin doesn't work! 
SDFileSystem sd(PB_5, PB_4, PB_3, PA_10, "sd"); // mosi, miso, sclk, cs
#endif
#if defined TARGET_NUCLEO_L432KC
DigitalIn    sdmiso(PA_6);
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

    // Should check for buffer full!!
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
        trim(tmp); // should not be necessary... files are stored on SD without blanks
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
            // ignore files other than BASIC (for the timebeing...),
            if ( (ext_pos = (uint8_t *)strstr (ent->d_name, ".BAS")) != 0 ) {
                n_files++;
                //debug_log("BAS %d\n", n_files);
            }
            if ( n_files == fileCount )
                break; // this is the BAS file we're looking for
        }
        // 'ent' now points to current file
        // file name for Sharp is expected to be "X:A       .BAS "
        debug_log("<%s>\n", ent->d_name);
        if ( formatFileName (ent->d_name) != NULL ) {
            debug_log("formatted <%s>\n", FileName);
            // send cleaned file name
            sendString((char*)FileName);
            outDataAppend(out_checksum);
        } else {
            outDataAppend(0xFF); // send err back
            ERR_PRINTOUT(" ERR clean\n");
        }
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
    // Send nb files, from specified dir (with wildcards)
    /* 
    QString s ="";
    for (int i =3;i< 15;i++) {
        s.append(Quint8_t(data.at(i)));
    }
    //    s="*.BAS"; 
    fileList = directory.entryList( QStringList() << s.replace(" ",""),QDir::Files);
    outDataAppend(CheckSum(fileList.size()));
    fileCount = -1;
    */
    if ((dir = opendir (SD_HOME)) != NULL) { // ignore files other than BASIC
        // print all the files and directories within directory
        while ((ent = readdir (dir)) != NULL
            && n_files < 0xFF ) { // max 255 files
            //debug_log("<%s>\n", ent->d_name);
            // ignore files other than BASIC (for the timebeing...)
            if ( (ext_pos = (uint8_t *)strstr (ent->d_name, ".BAS")) != 0 ) 
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
            pc.putc('l'); // debug
            if ( fp == NULL ) {
                ERR_PRINTOUT("fopen error\n");
                break;
            }
            pc.putc('l'); // debug
            file_size = getFileSize(fp);
            pc.putc('l'); // debug
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
            pc.putc('.');
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
            }
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

void process_SAVE(int cmd) {
    debug_log ( "SAVE 0x%02X\n", cmd);
    int c=0;
    uint8_t tmpFile[16];

    out_checksum = 0;
    if ( sdmiso == 0 ) {
        ERR_PRINTOUT(ERR_SD_CARD_NOT_PRESENT);
        outDataAppend(0xFF); 
        return;
    }
    switch (cmd) {
        case 0x10: { // get file name and create file on SD
            pc.putc('s');pc.putc('0');
            strncpy ((char*)tmpFile, (const char *)(inDataBuf+3), 12 );
            tmpFile[12]=0; // terminate string
            trim (tmpFile); // remove blanks, for the SD card
            sprintf ((char*)FileName, "%s%s", SD_HOME, tmpFile);
            debug_log ("SDcard filename: %s\n", FileName);
            file_pos = 0;
            outDataAppend(0x00); // ok, done
            break;
        }
        case 0x11: { // get file size
            pc.putc('s');pc.putc('1');
            if ( file_pos == 0 ) {
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
                outDataAppend(0x00); // ok, got size
                // next command, without device-code sequence
                skipDeviceCode = 0xFF;

            } else {
                // unexpected command 0x11
                ERR_PRINTOUT("unexpected 0x11 @%d");
                if (fp != NULL ) fclose (fp);
                outDataAppend(0xFF); // return with error
            }
            break;
        }
        case 0xFF: { // save a data chunk (non-ASCII)
                pc.putc('s');pc.putc('F');
                int buf_pos = 0;
                debug_log ("inDataBuf size %d\n", inBufPosition);
                debug_log ("creating <%s>\n", FileName );
                // create (or replace?) file
                if ( file_exists ( (char*)FileName ) )
                    // file exists, remove it
                    remove ( (char*)FileName );
                if ( fp != NULL ) fclose ( fp ); // just in case...
                fp = fopen((char*)FileName, "w"); // this needs to stay open until SAVE complete
                if ( fp == NULL ) {
                    ERR_PRINTOUT( "fopen error\n");
                    outDataAppend(0xFF); // NOT ok!
                    break;
                }
                // store data
                file_pos = 0;
                while ( buf_pos < inBufPosition - 1 ) { // last byte is checksum
                    fputc ( (int)(inDataBuf[buf_pos]), fp) ;
                    buf_pos ++;
                    file_pos ++;
                }
                debug_log ("file_pos %d file_size %d\n", file_pos, file_size);
                if ( file_pos == file_size ) {
                    fclose (fp); // done
                    debug_log ("file done\n");
                    skipDeviceCode = 0;
                } else {
                    skipDeviceCode = 0xFF; // expected new chunk
                }
                outDataAppend(0x00); // ok
            }
            break;
        case 0x16: { // ASCII data stream
            pc.putc('s');pc.putc('6');
            strncpy ((char*)tmpFile, (const char *)(inDataBuf+3), 12 );
            debug_log ("<%s>", tmpFile);
            
            outDataAppend(0x00);
            // next command, without device-code sequence
            skipDeviceCode = 0xFE;
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
        int diskspace = 20488; // test example
        debug_log ( "DSKF\n" ); 
        debug_log ( "diskspace %d\n",  diskspace);

        // we should calculate actual disk free space here...
        outDataAppend(CheckSum(0x00));
        outDataAppend(CheckSum(diskspace & 0xff));  // number of bytes 
        outDataAppend(CheckSum((diskspace >> 8) & 0xff));  // number of 256Bytes free sectors
        outDataAppend(CheckSum((diskspace >> 16) & 0xff));
        outDataAppend(out_checksum);      
}    

void process_CLOSE( uint8_t file ) {
        debug_log ( "CLOSE 0x%02X\n",  file);
        out_checksum = 0;
        // a CLOSE 0x00 (all files?) is issued also at RUN
        // ???
        outDataAppend(CheckSum(0x00));
}  

void ProcessCommand ( void ) {

    out_checksum = 0;
    cmdComplete = false;

    // verify the command sequence is correct
    // for multi-part commands
    if ( skipDeviceCode!=0 && inDataBuf[0]!=skipDeviceCode ) {
        ERR_PRINTOUT("unexpected command\n");
        pc.printf( " 0x%02X vs 0x%02X\n", inDataBuf[0], skipDeviceCode );
    }

    skipDeviceCode = 0;
    switch (inDataBuf[0]) {
    
    case 0x04: process_CLOSE(0);break;
    case 0x05: process_FILES();break;
    case 0x06: process_FILES_LIST(0);break;
    case 0x07: process_FILES_LIST(1);break;
    /* 
    case 0x08: process_INIT(0x08);break;
    case 0x09: process_INIT(0x09);break;
    case 0x0A: process_KILL(0x0A);break;
    */
        //    case 0x0B: process_NAME(0x0B);break;
        //    case 0x0C: process_SET(0x0C);break;
        //    case 0x0D: process_COPY(0x0D);break;
    case 0x0E: process_LOAD(0x0E);break;
    case 0x0F: process_LOAD(0x0F);break;
    case 0x10: process_SAVE(0x10);break;
    case 0x11: process_SAVE(0x11);break;
    case 0x16: process_SAVE(0x16);break;    // SAVE ASCII
    case 0xFE: process_SAVE(0xfe);break;    // Handle ascii saved data stream
    case 0xFF: process_SAVE(0xff);break;    // Handle saved data stream
    case 0x12: process_LOAD(0x12);break;
        //    case 0x13: process_INPUT(0x13);break;
        //    case 0x14: process_INPUT(0x14);break;
        //    case 0x15: process_PRINT(0x15);break;
    case 0x17: process_LOAD(0x17);break;
        //    case 0x1A: process_EOF(0x1A);break;
        //    case 0x1C: process_LOC(0x1C);break;
    case 0x1D: process_DSKF(); break;
        //    case 0x1F: process_INPUT(0x1f);break;
        //    case 0x20: process_INPUT(0x20);break;
    default:
        pc.printf(" command 0x%02X - ", inDataBuf[0]);
        ERR_PRINTOUT( "Unsupported (yet...)\n" ); 
        outDataAppend(CheckSum(0x00));
        break;
    }

    // command complete
    cmdComplete = true;
}
