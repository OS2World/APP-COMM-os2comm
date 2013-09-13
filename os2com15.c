/* os2comm.c - a simple os/2 comm program - version 1.5beta
*
* To use this program, type OS2COMM at the OS/2 prompt,
* followed by the port, baud rate, parity, databits, and stopbits,
* and an optional capture file name:
*
*    OS2COMM  COMx:baud,parity,databits,stopbits  capture-filespec
*
* To end this program, press Alt-X.
*
* While a communications session is in progress, data can be transmitted
* from a file by pressing Alt-F or Alt-A.  The program will prompt for the
* filename, the data in the file will be transmitted, then control will be
* returned to the keyboard.  Using Alt-A performs an ASCII upload - replacing
* CRLF pairs with CRs.  Alt-F sends the file without any translation.
*
* This version of OS2Comm has added logic to receive a file using the
* XModem file transfer protocol.  To receive a file using XModem, first
* tell the remote system to begin sending it, then press Alt-R.  OS2Comm
* will prompt you for the receive filespec.  You may optionally specify a
* drive letter and path.
*
* This program will run only in the protected mode of OS/2.
* It will not run under DOS.
*
* The program contains 3 threads:
*
*   1. main() - This thread calls a COM initialization routine, then
*      opens the COM port and creates the next two threads.  After they
*      have been created, it waits for the keytocom() thread to complete,
*      then exits back to OS/2.
*
*   2. keytocom() - This thread reads characters from the keyboard and
*      sends them to the COM device driver.  When a Alt-Z is received
*      from the keyboard, it signals main() to exit.  When Alt-F or Alt-A is
*      received from the keyboard, it prompts for a filename, then sends
*      the data from the file to the COM port.  It also handles the Alt-R
*      XModem protocol for receiving files.
*
*   3. comtodsp() - This thread reads characters from the COM device
*      driver and displays them on the screen.  It also writes data to
*      the optional capture file.
*
* Three semaphores are used in this version of OS2Comm.  The first,
* main_sem, is used to release the main thread when the keytocom thread
* has received an Alt-X from the keyboard.  This allows the program to end
* gracefully.
*
* The second, buf_sem, is used by the ascii upload routine, filetocom,
* which uses the OS/2 DosWriteAsync function to send data to the COM
* device driver.  A semaphore is required by this function to allow the
* program to know when the buffer has been freed.  Using DosWriteAsync for
* this routine allows the file transfer to proceed as fast as possible.
*
* The third semaphore, ctd_sem, is used to control the comtodisp thread.
* Requesting this semaphore causes the comtodisp thread to block waiting
* for it.  Clearing it allows comtodisp to run again.  This method of
* controlling comtodisp is used instead of the OS/2 DosSuspendThread and
* DosResumeThread functions because it allows control over where the
* thread is when it is blocked.  DosSuspendThread will block the thread no
* matter what it is doing at the time.
*
* To compile and link this program, at the OS2 prompt, type:
*
*      set PATH=C:\C\BIN;C:\;C:\OS2
*      set INCLUDE=C:\C\INCLUDE
*      set LIB=C:\C\LIB;C:\OS2
*      set TMP=C:\
*      cl os2comm.c /AS
*
* These settings are dependent on the compiler and version.
*
* This program requires the use of the COM0x.SYS device driver
* in CONFIG.SYS.  Use COM01.SYS for AT class machines.  Use
* COM02.SYS for PS/2 machines.
*
* OS2Comm.c - Copyright 1988 by Jim Gilliland.
*/
 
#define   LINT_ARGS
#define   INCL_BASE
#define   INCL_DOSDEVICES
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <ctype.h>
#include <process.h>
#include <malloc.h>
#include <io.h>
#include <fcntl.h>
#include <sys\types.h>
#include <sys\stat.h>
#define   STK_SIZE    2048
#define   MAXDIAL     11
 
typedef struct _TW_ROW
    {
    char *prompt, *text;
    int prow, pcol, trow, tcol, tmax;
    } TW_ROW;
 
static char dialdata[MAXDIAL+1][41] =
            {
            "",
            "ATDT",
            "|",
            "CONNECT",
            "",
            "BUSY",
            "NO ANSWER",
            "NO DIAL TONE",
            "UNSUCCESSFUL",
            "VOICE",
            "ABORT",
            "Time Out"
            };
 
static char dialdataprompt[MAXDIAL+1][21] =
            {
            "Modem Init String",
            "Dial Prefix",
            "Dial Suffix",
            "Connected message 1",
            "Connected message 2",
            "Failed message 1",
            "Failed message 2",
            "Failed message 3",
            "Failed message 4",
            "Failed message 5",
            "Failed message 6",
            ""
            };
 
TW_ROW dialsetup[12] =
    {
    dialdataprompt[0], dialdata[0], 2, 5, 2, 27, 40,
    dialdataprompt[1], dialdata[1], 4, 5, 4, 27, 40,
    dialdataprompt[2], dialdata[2], 6, 5, 6, 27, 40,
    dialdataprompt[3], dialdata[3], 8, 5, 8, 27, 40,
    dialdataprompt[4], dialdata[4], 10, 5, 10, 27, 40,
    dialdataprompt[5], dialdata[5], 12, 5, 12, 27, 40,
    dialdataprompt[6], dialdata[6], 14, 5, 14, 27, 40,
    dialdataprompt[7], dialdata[7], 16, 5, 16, 27, 40,
    dialdataprompt[8], dialdata[8], 18, 5, 18, 27, 40,
    dialdataprompt[9], dialdata[9], 20, 5, 20, 27, 40,
    dialdataprompt[10], dialdata[10], 22, 5, 22, 27, 40,
    NULL, NULL, 0, 0, 0, 0, 0
    };
 
typedef struct _COMDEVICECTRL {
      unsigned int wtime;
      unsigned int rtime;
      unsigned char flags1;
      unsigned char flags2;
      unsigned char flags3;
      unsigned char errchar;
      unsigned char brkchar;
      unsigned char xonchar;
      unsigned char xoffchar;
      } COMDEVICECTRL;
 
typedef struct _COMLINECHAR {
      unsigned char databits;
      unsigned char parity;
      unsigned char stopbits;
      } COMLINECHAR;
 
typedef struct _COMMODEMCTRL{
      unsigned char onmask;
      unsigned char offmask;
      } COMMODEMCTRL;
 
 
void main(int, char**, char**);
int      initcomm(int, char, int, int);
void far keytocom(void);
int      savesetup(void);
int      getsetup(void);
int      gettext(char *, char *, int, int, int, int, int, int, char);
int      window(void *, int, int, int, int, char, char);
int      dial(char *);
int      sendstring(char *);
unsigned chkesc(void);
int      filetocom(char);
int      xmodemr(char *, char);
void far comtodsp(void);
int      parsarg(char*, char*, int*,
                 char*, int*, int*);
void far * tw_open(int,int,int,int,int,int,char,char);
int tw_close(void far *);
unsigned short com, cfile;
unsigned long far main_sem, far buf_sem, far ctd_sem;
unsigned ktcid, ctdid, cflag;
 
char *usage ="\nOS2Comm - usage is:\r\n"
"\n\tOS2Comm COMx:baudrate,parity,"
                "databits,stopbits"
"\n\twhere:"
"\n\t\t COMx     = COM1, COM2, or COM3"
"\n\t\t baudrate = 300,1200,2400,"
                  "4800,9600, or 19200"
"\n\t\t parity   = N,O,E,M, or S"
"\n\t\t databits = 5,6,7, or 8"
"\n\t\t stopbits = 1 or 2\r\n";
 
void main(argc, argv, envp)
int argc;
char **argv;
char **envp;
{
 char far *ctdstack, far *ktcstack;
 int act, baud, dbits, sbits;
 unsigned int cftype, cfattr;
 unsigned long cfptr;
 char parity, comport[8];
 
 puts("OS2Comm.c version 1.5beta");
 puts("Copyright 1988 by Jim Gilliland");
 
 if (argc < 2 || argc > 3)
    {
     puts(usage);
     exit(1);
    }
 
 if (parsarg(argv[1],comport,&baud,
             &parity,&dbits,&sbits))
    {
     puts(usage);
     exit(1);
    }
 
 /* Open com device driver: */
 if (DosOpen(comport,&com,&act,
             0L,0,0x01,0x0012,0L))
     {
     fprintf(stderr,"\nError opening port");
     exit(1);
     }
 
 /* Initialize com device driver: */
 if (initcomm(baud, parity, dbits, sbits))
    {
    fprintf(stderr,"\nPort setup error");
    exit(1);
    }
 
 /* Open capture file, if specified: */
 if (argc > 2)
   if (DosOpen(argv[2],&cfile,&act,
               0L,0,0x11,0x0022,0L))
     {
     fprintf(stderr,"\nErr: %s\n",argv[2]);
     exit(1);
     }
    else
        {
        cflag = 1;
        DosQHandType(cfile,&cftype,&cfattr);
        if (cftype == 0 && act == 1)
            DosChgFilePtr(cfile,0L,2,&cfptr);
        }
 else cflag = 0;
 
 /* allocate stack for threads: */
 ctdstack = malloc(STK_SIZE);
 ktcstack = malloc(STK_SIZE);
 
 if (ctdstack == NULL || ktcstack == NULL)
    {
    puts ("Unable to allocate stacks");
    exit(2);
    }
 
 /* Create receive and display thread: */
 if (DosCreateThread(comtodsp,&ctdid,
                     ctdstack+STK_SIZE))
    {
    puts("Can't create COM receive thread");
    exit(1);
    }
 
 /* Set semaphore to block main thread: */
 DosSemSet(&main_sem);
 
 /* Create transmit thread: */
 if (DosCreateThread(keytocom,&ktcid,
                     ktcstack+STK_SIZE))
    {
    puts("Can't create COM transmit thread");
    exit(1);
    }
 
 puts("Alt-X will end this program");
 
 /* Set high priority for COM threads */
 DosSetPrty(2,3,1,ctdid); /* time-critical + 1  */
 DosSetPrty(2,3,2,ktcid); /* time-critical + 2  */
 
/* Wait for clear semaphore (see keytocom) */
 DosSemWait(&main_sem,-1L);
 
/* Suspend the other threads before ending */
 DosSuspendThread(ktcid);
 DosSuspendThread(ctdid);
 
 /* Close com driver and capture file: */
 DosClose(com);
 if (cflag==1) DosClose(cfile);
 
 /* Give the DosClose calls time to finish: */
 DosSleep(100L);
 
 DosExit(1,0); /* exit: end all threads */
}
/*******************************************/
 
void far comtodsp()
/* This routine is run as a separate thread */
{
 static char comchar[512];
 unsigned int bytes, readerr, cnt;
 
 while ( -1 )  /* Do forever: */
 {
    DosSemRequest(&ctd_sem,-1L);
    /* read character(s) from COMport: */
    readerr = DosRead(com,comchar,512,&bytes);
 
    if (readerr == 0 && bytes > 0)
       {
        /* Write to screen: */
        VioWrtTTY(comchar,bytes,0);
 
        /* write to capture file: */
        if (cflag == 1)
            DosWrite(cfile,comchar,bytes,&cnt);
       }
    DosSemClear(&ctd_sem);
 }
}
/*******************************************/
 
void far keytocom()
/* This routine is run as a separate thread */
{
 KBDKEYINFO keyinfo;
 unsigned char charcode, scancode, mode, wincolor;
 unsigned int written, ioctlerr, xretcode;
 static char comerr, xname[48], YNresponse[4], number[11];
 void far *twp;
 
 getsetup();
 
 wincolor = 62;
 mode = 'C';
 
 sendstring(dialdata[0]);
 
 while ( -1 )  /* Do forever: */
 {
   /* Get character from keyboard: */
   KbdCharIn (&keyinfo,0,0);
   charcode = keyinfo.chChar;
   scancode = keyinfo.chScan;
 
   /* Alt-X indicates End-Of-Processing: */
   if (charcode == 0x00 && scancode == 0x2D)
      {
      DosSemRequest(&ctd_sem,-1L);
      strcpy(YNresponse,"Yes");
      twp=tw_open(8,10,5,25,10,15,wincolor,1);
      gettext("Exit to OS/2? ",YNresponse,10,15,10,29,3,0,wincolor);
      tw_close(twp);
      DosSemClear(&ctd_sem);
      YNresponse[0] = toupper(YNresponse[0]);
      if (YNresponse[0] == 'Y')
          /* Clear Main semaphore:        */
             DosSemClear(&main_sem);
      continue;
      }
 
   /* Alt-D: get phone number and dial */
   if (charcode == 0x00 && scancode == 0x20)
       {
       DosSemRequest(&ctd_sem,-1L);
       strcpy(number,"");
       twp=tw_open(15,20,5,45,18,25,wincolor,1);
       gettext("Enter phone number: ",number,18,25,18,47,11,0,wincolor);
       tw_close(twp);
       if (strlen(number) == 0)
           {
           DosSemClear(&ctd_sem);
           continue;
           }
       if (dial(number))
           puts("Dial command failed");
       DosSemClear(&ctd_sem);
       continue;
       }
 
   /* Alt-A: get filename and process ASCII file */
   if (charcode == 0x00 && scancode == 0x1E)
      filetocom('a');
 
   /* Alt-F: get filename and process file */
   if (charcode == 0x00 && scancode == 0x21)
      filetocom('f');
 
   /* Alt-S: get setup data */
   if (charcode == 0x00 && scancode == 0x1F)
      window(dialsetup,0,0,25,80,30,121);
 
   /* Alt-P: save setup data */
   if (charcode == 0x00 && scancode == 0x19)
      savesetup();
 
   /* PgDn: get filename and receive XModem file */
   if ((charcode == 0x00 || charcode ==0xE0) && scancode == 0x51)
      {
       DosSemRequest(&ctd_sem,-1L);
       strcpy(xname,"");
       twp=tw_open(9,10,11,60,18,5,wincolor,1);
 
       VioWrtCharStrAtt("X - XModem  -   Checksum",24,10,20,&wincolor,0);
       VioWrtCharStrAtt("C - XModem  -   CRC     ",24,11,20,&wincolor,0);
       VioWrtCharStrAtt("F - XModemG -   CRC     ",24,12,20,&wincolor,0);
       VioWrtCharStrAtt("Y - YModem batch  - CRC ",24,13,20,&wincolor,0);
       VioWrtCharStrAtt("G - YModemG batch - CRC ",24,14,20,&wincolor,0);
 
       gettext("Protocol type? (X,C,F,Y,G): ",&mode,16,15,16,44,1,0,wincolor);
       mode = toupper(mode);
       if (mode != 'X' && mode != 'C' && mode != 'F' &&
           mode != 'Y' && mode != 'G')
           {
           DosSemClear(&ctd_sem);
           tw_close(twp);
           continue;
           }
       if (mode == 'Y' || mode == 'G')
           gettext("Enter receive pathname: ",xname,17,15,18,20,48,0,wincolor);
       else
           gettext("Enter receive filename: ",xname,17,15,18,20,48,0,wincolor);
       tw_close(twp);
       if (strlen(xname) == 0)
           {
           DosSemClear(&ctd_sem);
           continue;
           }
       do  {
           xretcode = xmodemr(xname,mode);
           }
           while (xretcode == 2);
 
       if (xretcode)
           puts("Transfer failed");
       DosSemClear(&ctd_sem);
       continue;
      }
 
   if ((charcode == 0x00 || charcode == 0xE0) && scancode != 0x00)
      continue; /* skip Alt-keys & F-Keys */
 
   /* Write character(s) to com port: */
   DosWrite(com,&charcode,1,&written);
 
   if (written == 0)
      {
       DosDevIOCtl(&comerr,NULL,0x64,01,com);
       printf("\r\nCOM Driver reports error %u\r\n", comerr);
      }
 }
}
 
int sendstring(string)
char *string;
{
    register char *c;
    char sendchar;
    int ctrlflag = 0;
    int written;
    for (c=string; *c; c++)
        {
        if (ctrlflag == 1)
            {
            sendchar = *c & 0x1F;
            ctrlflag = 0;
            }
        else
            {
            switch (*c)
                {
                case '^':
                    ctrlflag = 1;
                    sendchar = NULL;
                    break;
                case '~':
                    DosSleep(500L);
                    sendchar = NULL;
                    break;
                case '|':
                    sendchar = '\r';
                    break;
                case '`':
                    sendchar = ' ';
                    break;
                default:
                    sendchar = *c;
                }
            }
        if (sendchar)
            DosWrite(com,&sendchar,1,&written);
        }
}
 
int dial(number)
/* This routine dials until it connects */
char number[];
 
{
    unsigned rsub[MAXDIAL+1];
    int bytes, written, retcode;
    int i, timecount;
    char comchar;
    char comstring[20];
    unsigned int callnumber = 1;
 
    static COMDEVICECTRL olddcb, newdcb;
 
    /* get current settings: */
    DosDevIOCtl((char *)&olddcb,NULL,0x73,01,com);
 
    /* Set com device processing parameters:  */
    newdcb.wtime = 10; /* .1sec transmit timeout */
    newdcb.rtime = 10; /* .1sec receive  timeout */
    newdcb.flags1 = 0x01;  /* enable DTR */
    newdcb.flags2 = 0x40;  /* enable RTS, disable XON/XOFF */
    newdcb.flags3 = 0x02;  /* recv timeout mode  */
    newdcb.errchar = olddcb.errchar;
    newdcb.brkchar = olddcb.brkchar;
    newdcb.xonchar = olddcb.xonchar;
    newdcb.xoffchar = olddcb.xoffchar;
    DosDevIOCtl(NULL,(char *)&newdcb,0x53,01,com);
 
    printf("\n");
    retcode = 0;
 
    while (-1)
        {
        for (bytes = 1; bytes == 1; ) /* wait for quiet */
             DosRead(com,&comchar,1,&bytes);
 
        DosSleep(250L); /* Wait 1 quarter second to hang up phone */
 
        if (chkesc())  /* check keyboard for ESC character */
           {
            fprintf(stderr,"\r\nDial command aborted.\r\n");
            retcode=1;
            break;
           }
 
        sendstring(dialdata[1]);
        sendstring(number);
        sendstring(dialdata[2]);
 
        timecount = 0;
 
        for ( i = 0; i < MAXDIAL+1; )
              rsub[i++] = 0;
 
        while (-1)
            {
            if (chkesc())  /* check keyboard for ESC character */
               {
                fprintf(stderr,"\r\nDial command aborted.\r\n");
                retcode = 1;
                break;
               }
 
            DosRead(com,&comchar,1,&bytes);
            if (bytes == 0)
                {
                if (timecount++ > 200)
                    {
                    i = MAXDIAL;
                    break;
                    }
                else
                    continue;
                }
 
            for (i=3; i < MAXDIAL; i++ )
                {
                if (comchar == dialdata[i][rsub[i]])
                    {
                    rsub[i]++;
                    if (dialdata[i][rsub[i]] == NULL)
                        break;
                    }
                else
                    rsub[i] = 0;
                }
 
            if (i < MAXDIAL)
                break;
            }
 
        if (retcode == 0)
            printf("\rResult %u: %s                 \r",
                      callnumber++,dialdata[i]);
        else
            break;
 
        if (i == 3 || i == 4)   /* 3 and 4 are success codes */
            {
            DosBeep(880,100);
            DosBeep(660,100);
            break;
            }
        }
    DosDevIOCtl(NULL,(char *)&olddcb,0x53,01,com);
    printf("\n");
    return retcode;
}
 
savesetup()
{
    FILE *parm;
    static char parmname[64];
    int i;
 
    strcpy(parmname,"os2comm.prm");
    parm = fopen(parmname,"w+");
    fwrite(dialdata,1,sizeof(dialdata),parm);
    fclose(parm);
}
 
getsetup()
{
    FILE *parm;
    static char parmname[64];
    int i, j;
 
    strcpy(parmname,"os2comm.prm");
    parm = fopen(parmname,"r");
    fread(dialdata,1,sizeof(dialdata),parm);
    fclose(parm);
}
 
int filetocom(type)
/* this routine transmits an ASCII file to the remote system */
char type;
{
 static char buffer[64], work[64];
 register char *cptr;
 unsigned written, writerr, bytes, inhandle, inoflag;
 static char infile[48];
 void far *twp;
 
    DosSemRequest(&ctd_sem,-1L);
    strcpy(infile,"");
    twp=tw_open(15,0,5,80,18,5,62,1);
    gettext("Enter file name: ",infile,18,5,18,23,48,0,62);
    tw_close(twp);
    if (strlen(infile) == 0)
       {
       DosSemClear(&ctd_sem);
       return 0;
       }
    if (type == 'a')
        inoflag = O_RDONLY | O_TEXT;
    else
        inoflag = O_RDONLY | O_BINARY;
    if ((inhandle=open(infile,inoflag)) == -1)
       {
       fprintf(stderr,"\nOpen error: %s\r\n",infile);
       DosSemClear(&ctd_sem);
       return 1;
       }
 
    DosSemClear(&ctd_sem);
    /* Initialize write semaphore: */
    DosSemClear(&buf_sem);
 
    while ( -1 )   /* Do forever: */
    {
      bytes=read(inhandle,work,64);
      if (bytes == 0)  /* end of file ? */
         {
         close(inhandle); /* close file */
         DosSemWait(&buf_sem, -1L);
         return 0;
         }
 
      /* Translate newline chars to carriage returns: */
      if (type == 'a')
         for ( cptr=work ; cptr < work+bytes ; cptr++ )
                if (*cptr == '\n') *cptr = '\r';
 
      /* Wait for last write to complete: */
      DosSemWait(&buf_sem, -1L);
 
      strncpy(buffer,work,bytes); /* copy to buffer */
 
      if (chkesc())  /* check keyboard for ESC character */
         {
          fprintf(stderr,"\r\nText upload aborted.\r\n");
          return 1;
         }
 
      DosSemSet(&buf_sem);
      DosWriteAsync(com,&buf_sem,&writerr,
                    buffer,bytes,&written);
 
    }
}
 
int gettext(prompt,text,prow,pcol,trow,tcol,max,mode,tcolor)
 
char prompt[], text[];
int prow, pcol, trow, tcol, max, mode;
char tcolor;
 
/* mode 0 - one entry only
   mode 1 - one of a group of entries
   mode 2 - display and return without accepting any input
*/
 
{
        KBDKEYINFO keyinfo;
        VIOCURSORINFO curhold, curinfo;
        register unsigned char keystroke, scancode;
        register int p, i;
        int len, retcode;
        unsigned char insert;
        char blankcell[2];
 
        text[max] = '\0'; /* force string length to be max or less */
 
        VioGetCurType(&curhold,0);
        curinfo.cEnd = curhold.cEnd;
        curinfo.cx   = curhold.cx;
        curinfo.attr = curhold.attr;
        VioWrtCharStr(prompt,strlen(prompt),prow,pcol,0);
        p = 0;
        insert = 0x00;
        blankcell[0] = ' ';
        blankcell[1] = tcolor;
 
        while( -1 )
        {
            VioSetCurPos(trow,tcol+p,0);
            len = strlen(text);
 
            VioWrtCharStrAtt(text,len,trow,tcol,&tcolor,0);
            VioWrtNCell(blankcell,max-len,trow,tcol+len,0);
 
            if (mode == 2)
                return 0;
 
            if (KbdCharIn(&keyinfo,0,0))
                continue;
            keystroke = keyinfo.chChar;
            scancode  = keyinfo.chScan;
 
            if  (keystroke == 0x00 || keystroke == 0xE0)
                if (scancode == 0x4b && p > 0)
                    {
                    p--;
                    continue;
                    }
                else if ((scancode == 0x4d) && (p < len) && (p < max))
                    {
                    p++;
                    continue;
                    }
                else if (scancode == 0x47)
                    {
                    p=0;
                    continue;
                    }
                else if (scancode == 0x4f)
                    {
                    p=((len < max)? len : max);
                    continue;
                    }
                else if (scancode == 0x52)
                    {
                    insert ^= 0x01;
                    if (insert)
                        curinfo.yStart = curhold.yStart / 2;
                    else
                        curinfo.yStart = curhold.yStart;
                    VioSetCurType(&curinfo,0);
                    continue;
                    }
                else if (scancode == 0x53 && p < len)
                    {
                    for (i=p; i<len+1; i++)
                        text[i] = text[i+1];
                    continue;
                    }
                else if (scancode == 0x75 && p < len)
                    {
                    text[p] = '\0';
                    continue;
                    }
                else if (scancode == 0x48 && mode == 1)
                         {
                         retcode = -1;
                         break;
                         }
                else if (scancode == 0x50 && mode == 1)
                         {
                         retcode = +1;
                         break;
                         }
                else continue;
 
            if  (keystroke == '\r')
                {
                retcode = 0;
                break;
                }
 
            if  (keystroke == 0x1b)
                if (mode == 0)
                    if (len == 0)
                        {
                        retcode = 0x1b;
                        break;
                        }
                    else
                        {
                        p=0;
                        text[p] = '\0';
                        }
                else
                    {
                    retcode = 0x1b;
                    break;
                    }
 
            if  ((keystroke == '\b') && (p > 0))
                {
                p--;
                for (i=p; i<len+1; i++)
                    text[i] = text[i+1];
                }
            else
            if  (isprint(keystroke))
                if (insert)
                    {
                    if (len < max)
                        {
                        for (i=len+1; i>=p; i--)
                            text[i+1] = text[i];
                        text[p++] = keystroke;
                        }
                    }
                else
                    if (p < max)
                        {
                        text[p++] = keystroke;
                        if  (p > len)
                            text[p] = '\0';
                        }
        }
        if (len > 0)   /* eliminate trailing blanks */
            for (i=len-1; (text[i] == ' ') && (i >= 0); i--)
                text[i] = '\0';
        VioSetCurType(&curhold,0);
        return retcode;
}
 
unsigned chkesc()
/* this function checks the keyboard for the Escape character */
{
      KBDKEYINFO keyinfo;
      while ( -1 )
        {
         if (KbdCharIn(&keyinfo,1,0))
              return 0;   /* return if keyboard error */
         if ((keyinfo.fbStatus & 0xC0) == 0)
              return 0;   /* return if no key pressed */
         if (((keyinfo.fbStatus & 0x40) != 0) &&
              keyinfo.chChar == 0x1B)
             {
              KbdFlushBuffer(0);
              return 1;   /* if escape was pressed  */
             }
        }
}
 
int waitforquiet()
{
        static COMDEVICECTRL dcb;
        unsigned char dummy, oldflags3;
        unsigned int oldrtime, bytes;
 
        DosDevIOCtl((char *)&dcb,NULL,0x73,01,com);
        oldflags3 = dcb.flags3;
        oldrtime  = dcb.rtime;
 
        dcb.flags3 &= 0xF9; /* turn off both rtime bits */
        dcb.flags3 |= 0x04; /* turn on "wait" rtime bit */
        dcb.rtime = 10;     /* .1 second time out */
        DosDevIOCtl(NULL,(char *)&dcb,0x53,01,com);
 
        /* Read chars until no more available */
        for (bytes = 1; bytes == 1; )
             DosRead(com,&dummy,1,&bytes);
 
        dcb.flags3 = oldflags3;
        dcb.rtime = oldrtime;
        DosDevIOCtl(NULL,(char *)&dcb,0x53,01,com);
}
 
#define  NAK  "\x15"
#define  ACK  "\x06"
#define  SOH   0x01
#define  STX   0x02
#define  CAN   0x18
#define  EOT   0x04
#define  BS    0x08
 
int xmodemr(xname,mode)
char *xname;
char mode;
/* this function processes the XModem file receive */
 
{
    register unsigned int crc, count, c;
    register unsigned char *cbp, chksum;
    unsigned char chksum_in, fileopenflag;
    unsigned int crc_in;
    unsigned int fact, ferr, writ, bytes, blksize,
                 checksize, retcode, nak, ioctlerr;
    unsigned long int totsofar;
    unsigned long int filelength_in;
    unsigned short fhand;
    unsigned char ident, expected, comerr, initnak;
 
    static unsigned char filename_in[96];
    static unsigned char filename[160];
    static unsigned char pathname[64];
    static struct {
        unsigned char packet;
        unsigned char xpacket;
        unsigned char data[1030];
        } combuff;
 
    static COMDEVICECTRL olddcb, newdcb;
 
    static COMLINECHAR oldlc, newlc;
 
    if (mode != 'Y' && mode != 'G')
        {
        strcpy(filename,xname);
        ferr=DosOpen(filename,&fhand,&fact,0L,0,0x12,0x0022,0L);
        if (ferr)
           {
           printf("\r\n\nError Opening file: %s\r\n", filename);
           return 1;
           }
        fileopenflag = 1;
        }
    else
        {
        fileopenflag = 0;
        strcpy(pathname,xname);
        if (*(pathname+strlen(pathname)-1) != '\\')
            strcat(pathname,"\\");
        }
 
    if (mode == NULL || mode == 'X')
        {
        initnak = 0x15;
        checksize = 1;
        }
    else if (mode == 'Y' || mode == 'C')
        {
        initnak = 'C';
        checksize = 2;
        }
    else if (mode == 'G' || mode == 'F')
        {
        initnak = 'G';
        checksize = 2;
        }
    else
        {
        initnak = 0x15;
        checksize = 1;
        }
 
    /* get current settings: */
    DosDevIOCtl((char *)&olddcb,NULL,0x73,01,com);
    DosDevIOCtl((char *)&oldlc,NULL,0x62,01,com);
 
    /* Set com device processing parameters:  */
    newdcb.wtime = 10; /* .1sec transmit timeout */
    newdcb.rtime = 300; /* 3sec receive  timeout */
    newdcb.flags1 = 0x01;  /* enable DTR */
    newdcb.flags2 = 0x40;  /* enable RTS, disable XON/XOFF */
    newdcb.flags3 = 0x02;  /* recv timeout mode  */
    newdcb.errchar = olddcb.errchar;
    newdcb.brkchar = olddcb.brkchar;
    newdcb.xonchar = olddcb.xonchar;
    newdcb.xoffchar = olddcb.xoffchar;
    DosDevIOCtl(NULL,(char *)&newdcb,0x53,01,com);
 
    /* Set databits, stopbits, parity: */
    newlc.parity = 0;
    newlc.stopbits = 0;
    newlc.databits = 8;
    DosDevIOCtl(NULL,(char *)&newlc,0x42,01,com);
 
    /* Make sure line is quiet before starting: */
    waitforquiet();
 
    printf("\r\n");
    expected = 1;
    nak = 0;
    totsofar = 0;
    filelength_in = 0;
    retcode = 0;
 
    DosWrite(com,&initnak,1,&writ);
 
    while ( -1 )
    {
        if (chkesc()) /* check keyboard for ESC character */
           {
            printf("\rDownload aborted.             \r\n");
            waitforquiet();
            DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
            retcode = 1;
            break;
           }
 
        /* Set com device processing parameters:  */
        newdcb.rtime = 300; /* 3sec receive  timeout */
        DosDevIOCtl(NULL,(char *)&newdcb,0x53,01,com);
 
        if (nak == 3 && totsofar == 0)
            {
            initnak = 0x15;
            checksize = 1;
            }
 
        if (nak == 8)
           {
            printf("\rError: timed out or too many errors  \r\n");
            waitforquiet();
            DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
            retcode = 1;
            break;
           }
 
        /* Read lead character; should be SOH, STX, CAN, or EOT: */
        DosRead(com,&ident,1,&bytes);
 
        /* Send NAKs until first packet is started: */
        if (bytes == 0)
           {
            DosWrite(com,&initnak,1,&writ);
            nak++;
            continue;
           }
 
        if (ident != SOH && ident != EOT &&
            ident != STX && ident != CAN)
            continue;
 
        if (ident == EOT) /* EOT=End Of Transmission */
           {
            printf("\r\nReceived EOT\r\n");
            waitforquiet();
            DosWrite(com,ACK,1,&writ);
            if (mode == 'Y' || mode == 'G')
                retcode = 2;
            else
                retcode = 0;
            break;
           }
 
        if (ident == CAN) /* CAN=Cancel XModem process */
           {
            printf("\rReceived CAN                         \r\n");
            waitforquiet();
            retcode = 1;
            break;
           }
 
        /* if not EOT and not CAN, then receive the rest of the packet: */
 
        if (ident == STX)
            blksize = 1024;
        else
            blksize = 128;
 
        /* Set com device processing parameters:  */
        newdcb.rtime = 100; /* 1sec receive  timeout */
        DosDevIOCtl(NULL,(char *)&newdcb,0x53,01,com);
 
        DosRead(com,(char *)&combuff,2+blksize+checksize,&bytes);
 
        if ((bytes != 2+blksize+checksize) ||
           ((combuff.packet & combuff.xpacket) != 0x00))
           {
            printf("\rPacket Error: bytes %i, packet %x, xpacket %x\r\n",
                                    bytes, combuff.packet, combuff.xpacket);
            if (mode == 'G' || mode == 'F')
                {
                printf("\rDownload aborted.             \r\n");
                DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
                retcode = 1;
                break;
                }
            waitforquiet();
            DosWrite(com,NAK,1,&writ);
            nak++;
            continue;
           }
 
        if (checksize == 1)
            {
            chksum = 0;
            /* Compute checksum: */
            for (cbp = combuff.data ; cbp < combuff.data+blksize ; cbp++)
                chksum += *cbp;
 
            chksum_in = *(combuff.data+blksize);
 
            if (chksum != chksum_in)
               {
                printf("\rChecksum error: received %#2.2x, computed %#2.2x\r\n",
                                          chksum_in,       chksum);
                if (mode == 'G' || mode == 'F')
                    {
                    printf("\rDownload aborted.             \r\n");
                    DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
                    retcode = 1;
                    break;
                    }
                waitforquiet();
                DosWrite(com,NAK,1,&writ);
                nak++;
                continue;
               }
            }
        else
            {
            crc_in = (((unsigned int) *(combuff.data+blksize)) << 8)
                    + *(combuff.data+blksize+1);
            crc = 0;
            /* Compute cyclic redundancy check: */
            for (cbp = combuff.data ; cbp < combuff.data+blksize+2 ; cbp++)
                {
                c = *cbp;
                for (count=8; count>0; count--)
                    {
                    if (crc & 0x8000)
                        {
                        crc <<= 1;
                        crc += (((c<<=1) & 0x0100)  !=  0);
                        crc ^= 0x1021;
                        }
                    else
                        {
                        crc <<= 1;
                        crc += (((c<<=1) & 0x0100)  !=  0);
                        }
                    }
                }
 
            if (crc != 0)
               {
                printf("\rCRC error: received %#4.4x, computed %#4.4x\r\n",
                                              crc_in,      crc);
                if (mode == 'G' || mode == 'F')
                    {
                    printf("\rDownload aborted.             \r\n");
                    DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
                    retcode = 1;
                    break;
                    }
                waitforquiet();
                DosWrite(com,NAK,1,&writ);
                nak++;
                continue;
               }
            }
 
        if ((combuff.packet != expected) && (combuff.packet != expected-1))
           {
            printf("\rSequence failure: expected %u, received %u\r\n",
                                        expected,    combuff.packet);
            waitforquiet();
            DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
            retcode = 1;
            break;
           }
        else if (combuff.packet == expected-1)
             /* either this is the Ymodem zero packet, or */
             /* we've already received this packet, so ignore */
           {
            if (mode != 'G' && mode != 'F')
                DosWrite(com,ACK,1,&writ);
            nak = 0;
            if (combuff.packet == 0 && totsofar == 0)
                {
                if (strlen(combuff.data) > 0)
                    printf("\rFilename header: %s\r\n", combuff.data);
                if (mode == 'Y' || mode == 'G')
                    {
                    if (strlen(combuff.data) == 0)
                        {
                        printf("\rEnd of Batch\r\n");
                        retcode = 0;
                        break;
                        }
                    strcpy(filename,pathname);
                    strcat(filename,combuff.data);
                    strlwr(filename);
                    while ((cbp=strchr(filename,'/')) != NULL)
                        *cbp = '\\';
                    ferr=DosOpen(filename,&fhand,&fact,0L,0,0x12,0x0022,0L);
                    if (ferr)
                       {
                       printf("\r\n\nError Opening file: %s\r\n", filename);
                       waitforquiet();
                       DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
                       retcode = 1;
                       break;
                       }
                    else
                       fileopenflag = 1;
                    }
                cbp = combuff.data+strlen(combuff.data)+1;
                filelength_in = strtol(cbp,NULL,10);
                printf("\rFile length:    %lu\r\n", filelength_in);
                }
            continue;
           }
        else
           {
             /* good packet, write it to disk, increment bytecount */
            if (totsofar == 0)
                {
                if (checksize == 1)
                    printf("\rChecksum protocol in use\r\n");
                else
                    printf("\rCRC protocol in use\r\n");
                }
            totsofar += blksize;
            printf("\rBytes received: %lu\r", totsofar);
            if (mode != 'G' && mode != 'F')
                DosWrite(com,ACK,1,&writ);
            if (fileopenflag == 1)
                ferr=DosWrite(fhand,combuff.data,blksize,&writ);
            else
               {
                printf("\r\nProtocol error: No file specified\r\n");
                waitforquiet();
                DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
                retcode = 1;
                break;
               }
 
            if (ferr || writ < blksize)
               {
                printf("\r\nFile write error: %u, %u bytes written\r\n",
                                            ferr, writ);
                waitforquiet();
                DosWrite(com,"\x18\x18\x18\x08\x08\x08",6,&writ);
                retcode = 1;
                break;
               }
            expected++;
            nak = 0;
            continue;
           }
    }
 
    if (fileopenflag == 1)
        if (filelength_in > 0 && totsofar > filelength_in)
            {
            DosNewSize(fhand,filelength_in);
            printf("\r%s received: %lu bytes           \r\n",
                         filename, filelength_in);
            }
        DosClose(fhand);
    if (retcode == 2)
        printf("\r%s received successfully.           \r\n", filename);
    if (retcode == 0)
        {
        if (mode == 'Y' || mode == 'G')
            printf("\rBatch received successfully         \r\n");
        else
            printf("\r%s received successfully.           \r\n", filename);
        }
 
    ioctlerr=DosDevIOCtl(NULL,(char *)&olddcb,0x53,01,com);
    if (ioctlerr) printf("\r\nDevIOCtl 53h Error: %x\r\n",ioctlerr);
    ioctlerr=DosDevIOCtl(NULL,(char *)&oldlc,0x42,01,com);
    if (ioctlerr) printf("\r\nDevIOCtl 42h Error: %x\r\n",ioctlerr);
    return retcode;
}
/*******************************************/
 
int window(win_data,top,left,height,width,color,tcolor)
 
TW_ROW win_data[];
int top,left,height,width;
char color, tcolor;
 
{
    void far *twp;
    register int i, keycode;
    int imax;
 
    twp=tw_open(top,left,height,width,0,0,color,1);
 
    for (i=0; win_data[i].prompt != NULL; i++)
        gettext(win_data[i].prompt, win_data[i].text,
                win_data[i].prow, win_data[i].pcol,
                win_data[i].trow, win_data[i].tcol,
                win_data[i].tmax, 2, tcolor);
 
    imax = i-1;
    i = 0;
 
    while (-1)
        {
        keycode = gettext(win_data[i].prompt, win_data[i].text,
                          win_data[i].prow, win_data[i].pcol,
                          win_data[i].trow, win_data[i].tcol,
                          win_data[i].tmax, 1, tcolor);
 
        if (keycode == -1 && i > 0)
            i--;
 
        if ((keycode == +1 || keycode == 0) && i < imax)
            i++;
 
        if (keycode == 0x1b)
            break;
        }
 
    tw_close(twp);
}
 
 
typedef struct _TW
    {
    int top, left, height, width, currow, curcol;
    char restore; /* first character of video-hold buffer */
    } TW;
 
void far *tw_open(top,left,height,width,currow,curcol,color,border)
 
int top, left, height, width, currow, curcol;
char color, border;
 
{
    char cell[2];
    int i, j;
    char far *ibuff;
    TW far *twp;
    SEL seg;
    DosAllocSeg(2*height*width + sizeof(TW), &seg, 0);
    twp = MAKEP(seg,0);
    twp->top = top;
    twp->left = left;
    twp->height = height;
    twp->width = width;
 
    VioGetCurPos(&(twp->currow),&(twp->curcol),0);
 
    cell[0] = ' ';
    cell[1] = color;
    for (i=0 ; i < height ; i++)
        {
        j = width*2;
        ibuff = &(twp->restore) + i*j;
        VioReadCellStr(ibuff, &j, top+i, left, 0);
        if (border)
            {
            if (i == 0)
                {
                cell[0] = 201;
                VioWrtNCell(cell, 1, top+i, left, 0);
                cell[0] = 187;
                VioWrtNCell(cell, 1, top+i, left+width-1, 0);
                cell[0] = 205;
                VioWrtNCell(cell, width-2, top+i, left+1, 0);
                }
            else if (i == height-1)
                {
                cell[0] = 200;
                VioWrtNCell(cell, 1, top+i, left, 0);
                cell[0] = 188;
                VioWrtNCell(cell, 1, top+i, left+width-1, 0);
                cell[0] = 205;
                VioWrtNCell(cell, width-2, top+i, left+1, 0);
                }
            else
                {
                cell[0] = 186;
                VioWrtNCell(cell, 1, top+i, left, 0);
                cell[0] = 186;
                VioWrtNCell(cell, 1, top+i, left+width-1, 0);
                cell[0] = 32;
                VioWrtNCell(cell, width-2, top+i, left+1, 0);
                }
            }
        else
            {
            cell[0] = 32;
            VioWrtNCell(cell, width, top+i, left, 0);
            }
        }
 
    VioSetCurPos(currow,curcol,0);
    return twp;
 
}
 
tw_close(twp)
 
TW far *twp;
 
{
    int i, j;
    char far *ibuff;
    j = (twp->width)*2;
    for (i=0 ; i < (twp->height) ; i++)
        {
        ibuff = &(twp->restore) + i*j;
        VioWrtCellStr(ibuff, j, (twp->top)+i, (twp->left), 0);
        }
    VioSetCurPos(twp->currow,twp->curcol,0);
 
    DosFreeSeg(SELECTOROF(twp));
}
 
int initcomm(baud,parity,dbits,sbits)
char parity;
int baud,dbits,sbits;
   /* this routine used by main thread */
{
static COMLINECHAR linechar;
 
static COMMODEMCTRL modemctrl;
 
static COMDEVICECTRL dcb;
 
int comerr,act;
 
 /* Set bitrate: */
 if (DosDevIOCtl(NULL,(char *)&baud,
                     0x41,01,com))
     {
     fprintf(stderr,"\nBitrate error");
     return(1);
     }
 
 /* Set databits, stopbits, parity: */
 if (parity == 'N') linechar.parity = 0;
 if (parity == 'O') linechar.parity = 1;
 if (parity == 'E') linechar.parity = 2;
 if (parity == 'M') linechar.parity = 3;
 if (parity == 'S') linechar.parity = 4;
 if (sbits == 2) linechar.stopbits = 2;
 if (sbits == 1) linechar.stopbits = 0;
 linechar.databits = dbits;
 if (DosDevIOCtl(NULL,(char *)&linechar,
                     0x42,01,com))
     {
     puts("Line characteristics error");
     return(1);
     }
 
 /* Set modem control signals: */
 modemctrl.onmask = 0x03;  /* DTR & RTS on */
 modemctrl.offmask = 0xff; /* nothing off */
 if (DosDevIOCtl((char *)&comerr,
       (char *)&modemctrl,0x46,01,com))
     {
     puts("Modem control error");
     return(1);
     }
 
 /* Set com device processing parameters:  */
 dcb.wtime = 100; /* 1sec transmit timeout */
 dcb.rtime = 100; /* 1sec receive  timeout */
 dcb.flags1 = 0x01;  /* enable DTR,        */
 dcb.flags2 = 0x40;  /* enable RTS, disable XON/XOFF  */
 dcb.flags3 = 0x04;  /* recv timeout mode  */
 dcb.errchar = 0x00; /* no error translate */
 dcb.brkchar = 0x00; /* no break translate */
 dcb.xonchar = 0x11;  /* standard XON  */
 dcb.xoffchar = 0x13; /* standard XOFF */
 if (DosDevIOCtl(NULL,(char *)&dcb,
                       0x53,01,com))
     {
     puts("Device control block error");
     return(1);
     }
 
 return(0);
}
 
int parsarg(arg,port,baud,parity,dbits,sbits)
char *arg,*port;
char *parity;
int *baud,*dbits,*sbits;
    /* this routine used by main thread */
{
  register unsigned int strptr;
  char strhold[8];
  strupr(arg);   /* cvt to uppercase */
 
  /* Parse cmdline for COM port: */
  if ((strptr=strcspn(arg,":")) == 0)
                                  return(1);
  if (strptr > 8) return(1);
  strncpy(port,arg,strptr);
  *(port+strptr) = '\0';
  arg = arg+strptr+1;
 
  /* Parse for cmdline baudrate: */
  if ((strptr=strcspn(arg,",")) == 0)
                                  return(2);
  strncpy(strhold,arg,strptr);
  *(strhold+strptr) = '\0';
  *baud = atoi(strhold);
  if (*baud != 300 &&  *baud != 1200 &&
      *baud != 2400 && *baud != 4800 &&
      *baud != 9600 && *baud != 19200)
                                  return(2);
  arg = arg+strptr+1;
 
  /* Parse cmdline for parity: */
  if ((strptr = strcspn(arg,",")) == 0)
                                  return(3);
  *parity = *(arg+strptr-1);
  if (*parity != 'N' && *parity != 'O' &&
      *parity != 'E' && *parity != 'M' &&
      *parity != 'S')             return(3);
  arg = arg+strptr+1;
 
  /* Parse cmdline for databits: */
  if ((strptr = strcspn(arg,",")) == 0)
                                  return(4);
  *dbits = *(arg+strptr-1) - '0';
  if (*dbits != 5 &&   *dbits != 6 &&
      *dbits != 7 &&   *dbits != 8)
      return(4);
  arg = arg+strptr+1;
 
  /* Parse for stopbit value: */
  if ((strptr = strcspn(arg,",")) == 0)
                                  return(5);
  *sbits = *(arg+strptr-1) - '0';
  if (*sbits != 1 && *sbits != 2) return(5);
 
  return(0);
}
