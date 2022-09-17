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
int  fileCount; 
char FileName[15];

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
QString cleanFileName(QString s) {
    QString r = "X:";
    r = r + s.left(s.indexOf(".")).leftJustified(8,' ',true) + s.mid(s.indexOf(".")).rightJustified(4,' ',true);
    return r;
}
*/
char *cleanFileName(char *s) {
    char tmp[15];
    const char *p = strstr(s, ".BAS");
    if (p) {
        // File A.BAS is expected to be like "X:A       .BAS "
        // '%-8s' pads the string to 8 
        strncpy ( tmp, (const char*)s, (p-s));
        sprintf ( FileName, "X:%-8s.BAS ", tmp );
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
    debug_log ("fileCount %d\n", fileCount);
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

    if ((dir = opendir (SD_HOME)) != NULL) { 
        // browse all the files, and stop at 'fileCount'
        // same loop as in process_FILES (which only counts them)
        // NOTE - We browse the SD-card directory each time
        // and stop at 'fileCount' (current file to show)
        // in order to avoid keping in memory all the files list
        // (256 files x 15 = up to 3Kb)
        while ((ent = readdir (dir)) != NULL
            && n_files < 0xFF  // max 255 files 
            ) { 
            //debug_log("<%s>\n", ent->d_name);
            // ignore files other than BASIC (for the timebeing...),
            if ( (ext_pos = strstr (ent->d_name, ".BAS")) != 0 ) {
                n_files++;
                debug_log("BAS %d\n", n_files);
            }
            if ( n_files == fileCount )
                break; // this is the BAS file we're looking for
        }
        // 'ent' now points to current file
        cleanFileName (ent->d_name);
        debug_log("clean <%s>\n", FileName);
        closedir (dir);
        // send cleaned file name
        sendString(FileName);
        outDataAppend(out_checksum);
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
            // debug_log("<%s>\n", ent->d_name);
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
    debug_log( "ftell %d\n", size);
    fseek(fp, 0, SEEK_SET);
    return size;
};

void process_LOAD(uint8_t cmd) {
    debug_log ( "LOAD 0x%02X\n", cmd); 
    char c=0;
    switch (cmd) {
        case 0x0E:  { // open file
            char file_name[15];
            /*
            for (int i=3;i<15;i++) {
                s.append(QChar(data.at(i)));
            }
            file_load.setFileName(s);
            if (!file_load.open(QIODevice::ReadOnly)) {
                emit msgError(tr("ERROR opening file : %1").arg(s));
            }
            */
            strncpy ( file_name, (const char *)(inDataBuf+3), 11 );
            debug_log ( "opening <%s>\n", file_name );
            strcpy ( file_name, "TEST.BAS" ); // TEST ONLY !
            fp = (FILE *)fopen(file_name, "r");
            if ( fp != NULL ) {
                debug_log ( "fopen error\n");
                pc.putc('x');
                break;
            }
            int file_size = getFileSize((FILE *)fp);
            if ( file_size <= 0 ) {
                debug_log ( "getFileSize error %d\n", file_size);
                pc.putc('x');
                break;
            }            
            debug_log ( "file size %d\n", file_size);
            outDataAppend(0x00);
            out_checksum = 0;
            sendString(" ");
            // Send file size : 3 bytes + checksum
            outDataAppend(CheckSum(file_size & 0xff));
            outDataAppend(CheckSum((file_size >> 8) & 0xff));
            outDataAppend(CheckSum((file_size >> 16) & 0xff));
            outDataAppend(out_checksum);
            /* 
            ba_load = file_load.readAll();
            */
            break;
        }
        case 0x17: { // Send first byte   

            //ba_load.remove(0,0x0f); // remove first byte 'ff'
            //ba_load.chop(1);
            c = fgetc((FILE *)fp);
            outDataAppend(0x00);
            debug_log ( "first byte 0x%02X\n", c);
            outDataAppend(CheckSum(c));
            //data_out.append(CheckSum(0x0f));
            outDataAppend(out_checksum);
            //ba_load.remove(0,0x10);
            //wait_data_function = 0xfd;
            break;
        }        
        case 0x12: { // send data
            outDataAppend(0x00);
            // Envoyer une ligne complete
            // Until 0x0D
            // EOF = 0x1A
            // Start at 0x10 ??
            do {
                c=fgetc((FILE *)fp);
                outDataAppend(CheckSum(c));  // 0x1A pour fin de fichier
            } while ((c != EOF) && (c!=0x0d));
            if (c!=0x0d) {
                if (c == EOF) {
                    debug_log ("file complete\n");
                    outDataAppend(CheckSum(0x1A));  // 0x1A pour fin de fichier
                }
            }
            debug_log ("line complete\n");
            outDataAppend(out_checksum);
            outDataAppend(0x00);
            break;
        }
        case 0x0f: { // binary ?
            int file_size = getFileSize((FILE *)fp);
            outDataAppend(0x00);
            for (int i=0;i<file_size;i++) {
                c=fgetc((FILE *)fp);
                outDataAppend(CheckSum(c));
                //debug_log("send to output %1").arg(i))
                if (((i+1)%0x100)==0) {
                    outDataAppend(out_checksum);
                    out_checksum=0;
                    debug_log ("CHECKSUM\n");
                }
                //AddLog(LOG_PRINTER,tr("send to output (%1) : %2 - %3").arg(i+j*0x100).arg(c,2,16).arg(QChar(c)));
            }
            if ((file_size%0x100)) outDataAppend(out_checksum);
            outDataAppend(0x00);
            break;
        }
        default: {
            debug_log ("unknown LOAD sub-command\n");
            pc.putc('x');
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
    /*
    case 0x10: process_SAVE(0x10);break;
    case 0x11: process_SAVE(0x11);break;
    */
    case 0x12: process_LOAD(0x12);break;
        //    case 0x13: process_INPUT(0x13);break;
        //    case 0x14: process_INPUT(0x14);break;
        //    case 0x15: process_PRINT(0x15);break;
    /*
    case 0x16: process_SAVE(0x16);break;       // SAVE ASCII
    */
    case 0x17: process_LOAD(0x17);break;
        //    case 0x1A: process_EOF(0x1A);break;
        //    case 0x1C: process_LOC(0x1C);break;
    case 0x1D: process_DSKF(); break;
        //    case 0x1F: process_INPUT(0x1f);break;
        //    case 0x20: process_INPUT(0x20);break;
    /*
    case 0xFE: process_SAVE(0xfe);break;    // Handle ascii saved data stream
    case 0xFF: process_SAVE(0xff);break;    // Handle saved data stream
    */
    default:
        debug_log ( "Unsupported command (yet...)" ); 
        pc.putc('x');
        break;
    }
        
}
