#include "commands.h"
#include "SDFileSystem.h"

volatile char     inDataBuf[BUF_SIZE];
volatile char     outDataBuf[BUF_SIZE];
volatile uint16_t inBufPosition;
volatile uint16_t outBufPosition;
volatile uint8_t  checksum = 0;

extern void debug_log(const char *fmt, ...);
extern RawSerial         pc;

int fileCount;

// SD Card
SDFileSystem sd(PB_5, PB_4, PB_3, PA_10, "sd"); // mosi, miso, sclk, cs


char CheckSum(char b) {
    checksum = (checksum + b) & 0xff;
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


void process_FILES_LIST(int cmd) {
    // QString fname;
    
    pc.putc('l');
    pc.putc(0x30+cmd);
    debug_log ("FILES_LIST %d\n", cmd);
    outDataAppend(0x00);
    checksum=0;

    switch (cmd) {
    case 0:
            fileCount++;
            //fname = fileList.at(fileCount);
            //debug_log ("*"+cleanFileName(fname)+"*\n");
            // Send (cleaned-up) filenames
            //sendString(cleanFileName(fname));
            sendString("X:TEST    .BAS ");
            sendString(" ");
            // Send CheckSum
            outDataAppend(checksum);
            break;

    case 1:
            fileCount--;
            //fname = fileList.at(fileCount);
            //debug_log ("*"+cleanFileName(fname)+"*\n");
            // Send filenames
            //sendString(cleanFileName(fname));
            sendString("X:TEST    .BAS ");
            sendString(" ");
            // Send CheckSum
            outDataAppend(checksum);
            break;
    }
}


void process_FILES(void) {
    
    struct dirent* ent;
    DIR *dir;
    char n_files = 0x00;
    char *ext_pos;
    
    debug_log ( "FILES\n" ); 
    pc.putc('f');
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
    if ((dir = opendir ("/sd/")) != NULL) { // ignore files other than BASIC
        // print all the files and directories within directory
        while ((ent = readdir (dir)) != NULL
            && n_files < 0xFF ) { // max 255 files
            debug_log("<%s>\n", ent->d_name);
            // ignore files other than BASIC (for the timebeing...)
            if ( (ext_pos = strstr (ent->d_name, ".BAS")) != 0 ) {
                n_files++; 
            }
        }
        closedir (dir);
        fileCount = -1;
        debug_log("%d files\n", n_files);
        outDataAppend(CheckSum(n_files));
        outDataAppend(checksum);
        
        process_FILES_LIST ( 0x00 ); // append first file name too
        
    } else {
        // could not open directory
        pc.putc('x');
        pc.printf("Could not open SD directory\n");
    }
}


void ProcessCommand ( void ) {

    checksum=0;
    outBufPosition = 0;

    switch (inDataBuf[0]) {
        
    case 0x05: process_FILES();break;
    case 0x06: process_FILES_LIST(0);break;
    case 0x07: process_FILES_LIST(1);break;
            
    case 0x1D:
        // case 0x1D: process_DSKF();
        debug_log ( "DSKF\n" ); 
        outDataAppend(CheckSum(0x00));
        outDataAppend(CheckSum(0x02));  // number of byte
        outDataAppend(CheckSum(0x50));  // number of 256Bytes free sectors
        outDataAppend(CheckSum(0x00));
        outDataAppend(0x52);  // don't know yet. Perhaps a checksum ?     
        break;
    
    default:
        debug_log ( "Unsupported command (yet...)" ); 
        break;
    }
        
}
