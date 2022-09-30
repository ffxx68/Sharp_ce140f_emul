#include "commands.h"
#include "SDFileSystem.h"

// from other modules
extern void debug_log(const char *fmt, ...);
extern RawSerial         pc;

// shared with different threads
volatile char     inDataBuf[BUF_SIZE];
volatile char     outDataBuf[BUF_SIZE];
volatile uint16_t inBufPosition;
volatile uint16_t outBufPosition;

// local
uint8_t  out_checksum = 0;
FILE    *fp;
int      fileCount; 
char     FileName[15];
int      file_size;
int      file_pos = 0;

// SD Card (SDFileSystem library)
SDFileSystem sd(PB_5, PB_4, PB_3, PA_10, "sd"); // mosi, miso, sclk, cs

char CheckSum(char b) {
    out_checksum = (out_checksum + b) & 0xff;
    return b;
}

void outDataAppend(char b) {
    // NB in case we're going to implement a circular output buffer...
    //    I expect functions appending data to be faster than the
    //    consumer (outNibbleSend, increasing outDataPointer every about 10 ms).
    //    We should wait here, when outBufPosition reaches outDataPointer,
    //    for outDataPointer to increase again
    outDataBuf[ outBufPosition++ ] = b;
}

void sendString(char* s) {
    for (int i=0;i<strlen(s);i++) {
        outDataAppend(CheckSum(s[i]));
    }
}

/*
QString formatFileName(QString s) {
    QString r = "X:";
    r = r + s.left(s.indexOf(".")).leftJustified(8,' ',true) + s.mid(s.indexOf(".")).rightJustified(4,' ',true);
    return r;
}
*/

// in-place whitespace removal
void trim(char* s) {
    char* d = s;
    do {
        while (*d == ' ') {
            ++d;
        }
    } while (*s++ = *d++);
}

char *formatFileName(char *s) {
    char tmp[15];
    const char *p = strstr(s, ".BAS");
    if (p) {
        strncpy ( tmp, (const char*)s, (p-s));
        trim(tmp); // should not be necessary... files are stored on SD without blanks
        sprintf ( FileName, "X:%-8s.BAS ", tmp ); // '%-8s' pads to 8 blanks
        return FileName;
    } else
        return NULL;
}

void process_FILES_LIST(uint8_t cmd) {
    // QString fname;
    struct dirent* ent;
    DIR *dir;
    int n_files = -1;
    char *ext_pos;

    pc.putc('l');pc.putc(0x30+cmd);pc.putc('\n');
    debug_log ("FILES_LIST 0x%02X\n", cmd);
    outDataAppend(0x00);
    out_checksum=0;

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
            if ( (ext_pos = strstr (ent->d_name, ".BAS")) != 0 ) {
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
            sendString(FileName);
            outDataAppend(out_checksum);
        } else {
            pc.putc('x');
            outDataAppend(0xFF); // send err back
            debug_log(" ERR clean\n");
        }
        closedir (dir);

    }

}

void process_FILES(void) {
    
    struct dirent* ent;
    DIR *dir;
    int n_files = 0x00;
    char *ext_pos;
    
    debug_log ( "FILES\n" ); 
    outDataAppend(CheckSum(0x00));
    // Send nb files, from specified dir (with wildcards)
    /* 
    QString s ="";
    for (int i =3;i< 15;i++) {
        s.append(QChar(data.at(i)));
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
            if ( (ext_pos = strstr (ent->d_name, ".BAS")) != 0 ) 
                n_files++; 
        }
        closedir (dir);
        fileCount = -1;
        if ( n_files > 255 ){
            pc.putc('x');
            pc.printf("Number of files greater than 255\n");
            outDataAppend(0x00);
        } else {
            debug_log("%d files\n", n_files);
            outDataAppend(CheckSum(n_files));
            outDataAppend(out_checksum);
        }
    } else {
        // could not open directory
        pc.putc('x');
        pc.printf("Could not open SD directory\n");
        outDataAppend(0x00);
    }
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
    char tmpFile[16];
    switch (cmd) {
        case 0x0E:  { // open file and send file size
            pc.putc('l');
            /*
            for (int i=3;i<15;i++) {
                s.append(QChar(data.at(i)));
            }
            file_load.setFileName(s);
            if (!file_load.open(QIODevice::ReadOnly)) {
                emit msgError(tr("ERROR opening file : %1").arg(s));
            }
            */
            strncpy ( tmpFile, (const char *)(inDataBuf+3), 12 );
            tmpFile[12] = '\0'; // terminate
            trim (tmpFile); // remove blanks
            sprintf (FileName, "%s%s", SD_HOME, tmpFile);
            //pc.printf(FileName);
            debug_log ( "opening <%s>\n", FileName );
            if ( fp != NULL ) fclose ( fp ); // just in case...
            fp = fopen(FileName, "r"); // this needs to stay open until EOF
            pc.putc('l');
            if ( fp == NULL ) {
                debug_log ( "fopen error\n");
                pc.putc('x');
                break;
            }
            file_size = getFileSize(fp);
            pc.putc('l');
            if ( file_size <= 0 ) {
                debug_log ( "getFileSize error %d\n", file_size);
                if ( fp != NULL ) fclose ( fp );
                pc.putc('x');
                break;
            }    
            debug_log ( "size %d\n", file_size);
            file_pos = 0;
            outDataAppend(0x00);
            out_checksum = 0;
            sendString(" "); // ?
            // Send file size : 3 bytes (optimistic!) + checksum
            outDataAppend(CheckSum(file_size & 0xff));
            outDataAppend(CheckSum((file_size >> 8) & 0xff));
            outDataAppend(CheckSum((file_size >> 16) & 0xff));
            outDataAppend(out_checksum);
            /* 
            ba_load = file_load.readAll();
            */
            break;
        }
        case 0x17: { // send header bytes
            // header for non-ASCII files is read from 0xFF to 0x0F 
            // sending several 0x17 commands, one byte each.
            // For an ASCII file the header is nonexistent
            // and first 0x17 command will returns a char != 0xFF
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
                pc.putc('x');
                debug_log ( "first byte EOF!");
                outDataAppend(0xff); // error to Sharp
                if ( fp != NULL ) fclose ( fp );
            }
            //ba_load.remove(0,0x10);
            //wait_data_function = 0xfd;
            break;
        }        
        case 0x12: { // ASCII data chunk (one line max)
            pc.putc('-');
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
        case 0x0f: { // non-ASCII data chunk (256 bytes max)
            pc.putc('.');
            outDataAppend(0x00);
            do {
                c=fgetc(fp);
                file_pos++;
                outDataAppend(CheckSum(c));
                if (file_pos%0x100==0)
                    // break after 256 bytes
                    break;
            } while (c != EOF && file_pos < file_size);
            debug_log ("block (pos %d)\n", file_pos);
            outDataAppend(out_checksum);
            outDataAppend(0x00);
            if ( c==EOF || file_pos == file_size) {
                debug_log ("EOF (size %d)\n", file_size);
                if ( fp != NULL ) fclose ( fp );
            }
            break;
        }
        default: {
            debug_log ("unknown LOAD sub-command\n");
            pc.putc('x');
            if ( fp != NULL ) fclose ( fp );
            break;
        }
    }

}

void process_SAVE(int cmd) {
    debug_log ( "SAVE 0x%02X\n", cmd);
    int c=0;
    char tmpFile[16];

    switch (cmd) {
        case 0x10: {
            pc.putc('s');pc.putc('0');
            strncpy ( tmpFile, (const char *)(inDataBuf+3), 11 );
            trim (tmpFile); // remove blanks, for the SD card
            sprintf (FileName, "%s%s", SD_HOME, tmpFile);
            debug_log ( "creating <%s>\n", FileName );
            // create file
            if ( fp != NULL ) fclose ( fp ); // just in case...
            fp = fopen(FileName, "w"); // this needs to stay open until SAVE complete
            if ( fp == NULL ) {
                debug_log ( "fopen error\n");
                pc.putc('x');
                outDataAppend(0xFF); // NOT ok!
                break;
            }
            file_pos = 0;
            outDataAppend(0x00); // ok, got it
            break;
        }
        case 0x11: { //  data stream
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
                debug_log ("size %d\n", file_size);
                outDataAppend(0x00); // ok, got size
            } else {
                // wait_data_function = 0xff; 
                // handling next data stream chunk
                int buf_pos = 0;
                while ( buf_pos < inBufPosition ) {
                    fputc ( (int)(inDataBuf[buf_pos]), fp) ;
                    buf_pos ++;
                    file_pos ++;
                    debug_log ("position %d\n", file_pos);
                }
                if ( file_pos == file_size ) {
                    fclose (fp); // done
                    debug_log ("file done\n");
                }
                outDataAppend(0x00); // ok, got size     
            }
            break;
        }
        case 0x16: { // ASCII data stream
            pc.putc('s');pc.putc('6'); 
            strncpy ( tmpFile, (const char *)(inDataBuf+3), 12 );
            debug_log ("<%s>", tmpFile);
            outDataAppend(0x00);

            // ??? wait_data_function = 0xfe;

            break;
        }
        default: {
            debug_log ("unknown SAVE sub-command\n");
            pc.putc('x');
            if ( fp != NULL ) fclose ( fp );
            break;
        }

    }
}

void process_DSKF(void) {
        int diskspace = 20488; // test example
        debug_log ( "DSKF\n" ); 
        debug_log ( "diskspace %d\n",  diskspace);

        outDataAppend(CheckSum(0x00));
        outDataAppend(CheckSum(diskspace & 0xff));  // number of bytes 
        outDataAppend(CheckSum((diskspace >> 8) & 0xff));  // number of 256Bytes free sectors
        outDataAppend(CheckSum((diskspace >> 16) & 0xff));
        outDataAppend(out_checksum);      
}    

void ProcessCommand ( void ) {

    out_checksum=0;
    outBufPosition = 0;

    switch (inDataBuf[0]) {
    /*
    case 0x04: process_CLOSE(0);break;
    */
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
        debug_log ( "Unsupported command (yet...)" ); 
        pc.putc('x');
        break;
    }
        
}
