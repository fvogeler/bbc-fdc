#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <bcm2835.h>
#include <string.h>

#include "pins.h"

// Acorn DFS geometry and layout
#define SECTORSIZE 256
#define SECTORSPERTRACK 10
#define TRACKSIZE (SECTORSIZE*SECTORSPERTRACK)
#define MAXTRACKS 80
#define MAXFILES 31
#define MAXHEADS 2

// SPI read buffer size
#define SPIBUFFSIZE (1024*1024)

// Disk bitstream block size
#define BLOCKSIZE 2048

// Whole disk image
#define SECTORSPERSIDE (SECTORSPERTRACK*MAXTRACKS)
#define WHOLEDISKSIZE (SECTORSPERSIDE*SECTORSIZE)

// For sector status
#define NODATA 0
#define BADDATA 1
#define GOODDATA 2

// For disk/drive status
#define NODRIVE 0
#define NODISK 1
#define HAVEDISK 2

// For type of capture
#define DISKCAT 0
#define DISKIMG 1
#define DISKRAW 2

#define RETRIES 10

int current_track = 0;
int current_head = 0;

int debug=0;
int singlesided=1;
int capturetype=DISKCAT;

unsigned char spibuffer[SPIBUFFSIZE];
unsigned char *ibuffer;
unsigned int datacells;
int bits=0;
int hadAM=0;
int info=0;

// Most recent address mark
int blocktype=0;
int blocksize=0;

// Most recent address mark
unsigned long idpos;
unsigned char track, head, sector;
unsigned int datasize;

int maxtracks=MAXTRACKS;

unsigned char bitstream[BLOCKSIZE];
unsigned int bitlen=0;

// Store the whole disk image in RAM
unsigned char wholedisk[MAXHEADS][WHOLEDISKSIZE];
unsigned char sectorstatus[MAXHEADS][SECTORSPERSIDE];

unsigned long datapos=0;

FILE *diskimage=NULL;
FILE *rawdata=NULL;

// CCITT CRC16 (Floppy Disk Data)
unsigned int calc_crc(unsigned char *data, int datalen)
{
  unsigned int crc=0xffff;
  int i, j;

  for (i=0; i<datalen; i++)
  {
    crc ^= data[i] << 8;
    for (j=0; j<8; j++)
      crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }

  return (crc & 0xffff);
}

// Read nth DFS filename from catalogue
//   but don't add "$."
//   return the "Locked" state of the file
int getfilename(int entry, char *filename)
{
  int i;
  int len;
  unsigned char fchar;
  int locked;

  len=0;

  locked=(ibuffer[(entry*8)+7] & 0x80)?1:0;

  fchar=ibuffer[(entry*8)+7] & 0x7f;

  if (fchar!='$')
  {
    filename[len++]=fchar;
    filename[len++]='.';
  }

  for (i=0; i<7; i++)
  {
    fchar=ibuffer[(entry*8)+i] & 0x7f;

    if (fchar==' ') break;
    filename[len++]=fchar;
  }

  filename[len++]=0;

  return locked;
}

// Return load address for nth entry in DFS catalogue
unsigned long getloadaddress(int entry)
{
  unsigned long loadaddress;

  loadaddress=((((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+6]&0x0c)>>2)<<16) |
               ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+1])<<8) |
               ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)])));

  if (loadaddress & 0x30000) loadaddress |= 0xFF0000;

  return loadaddress;
}

// Return execute address for nth entry in DFS catalogue
unsigned long getexecaddress(int entry)
{
  unsigned long execaddress;

  execaddress=((((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+6]&0xc0)>>6)<<16) |
               ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+3])<<8) |
               ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+2])));

  if (execaddress & 0x30000) execaddress |= 0xFF0000;

  return execaddress;
}

// Return file length for nth entry in DFS catalogue
unsigned long getfilelength(int entry)
{
  return ((((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+6]&0x30)>>4)<<16) |
          ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+5])<<8) |
          ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+4])));
}

// Return file starting sector for nth entry in DFS catalogue
unsigned long getstartsector(int entry)
{
  return (((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+6]&0x03)<<8) |
          ((ibuffer[(1*SECTORSIZE)+8+((entry-1)*8)+7])));
}

// Display info read from the disk catalogue from sectors 00 and 01
void showinfo(int infohead)
{
  int i, j;
  int numfiles;
  int locked;
  unsigned char bootoption;
  size_t tracks, totalusage, totalsectors, totalsize, sectorusage;
  char filename[10];

  ibuffer=&wholedisk[infohead][0];

  printf("Head: %d\n", infohead);
  printf("Disk title : \"");
  for (i=0; i<8; i++)
  {
    if (ibuffer[i]==0) break;
    printf("%c", ibuffer[i]);
  }
  for (i=0; i<4; i++)
  {
    if (ibuffer[(1*SECTORSIZE)+i]==0) break;
    printf("%c", ibuffer[(1*SECTORSIZE)+i]);
  }
  printf("\"\n");

  totalsectors=(((ibuffer[(1*SECTORSIZE)+6]&0x03)<<8) | (ibuffer[(1*SECTORSIZE)+7]));
  tracks=totalsectors/SECTORSPERTRACK;
  //maxtracks=tracks;
  totalsize=totalsectors*SECTORSIZE;
  printf("Disk size : %d tracks (%d sectors, %d bytes)\n", tracks, totalsectors, totalsize);

  bootoption=(ibuffer[(1*SECTORSIZE)+6]&0x30)>>4;
  printf("Boot option: %d ", bootoption);
  switch (bootoption)
  {
    case 0:
      printf("Nothing");
      break;

    case 1:
      printf("*LOAD !BOOT");
      break;

    case 2:
      printf("*RUN !BOOT");
      break;

    case 3:
      printf("*EXEC !BOOT");
      break;

    default:
      printf("Unknown");
      break;
  }
  printf("\n");

  totalusage=0; sectorusage=2;
  printf("Write operations made to disk : %.2x\n", ibuffer[(1*SECTORSIZE)+4]); // Stored in BCD

  numfiles=ibuffer[(1*SECTORSIZE)+5]/8;
  printf("Catalogue entries : %d\n", numfiles);

  for (i=1; ((i<=numfiles) && (i<MAXFILES)); i++)
  {
    locked=getfilename(i, filename);

    printf("%-9s", filename);

    printf(" %.6lx %.6lx %.6lx %.3lx", getloadaddress(i), getexecaddress(i), getfilelength(i), getstartsector(i));
    totalusage+=getfilelength(i);
    sectorusage+=(getfilelength(i)/SECTORSIZE);
    if (((getfilelength(i)/SECTORSIZE)*SECTORSIZE)!=getfilelength(i))
      sectorusage++;

    if (locked) printf(" L");
    printf("\n");
  }

  printf("Total disk usage : %d bytes (%d%% of disk)\n", totalusage, (totalusage*100)/(totalsize-(2*SECTORSIZE)));
  printf("Remaining catalogue space : %d files, %d unused disk sectors\n", MAXFILES-numfiles, (((ibuffer[(1*SECTORSIZE)+6]&0x03)<<8) | (ibuffer[(1*SECTORSIZE)+7])) - sectorusage);
}

// Add a bit to the 16-bit accumulator, when full - attempt to process (clock + data)
void addbit(unsigned char bit)
{
  unsigned char clock, data;
  unsigned char dataCRC;

  datacells=((datacells<<1)&0xffff);
  datacells|=bit;
  bits++;

  if (bits>=16)
  {
    // Extract clock byte
    clock=((datacells&0x8000)>>8);
    clock|=((datacells&0x2000)>>7);
    clock|=((datacells&0x0800)>>6);
    clock|=((datacells&0x0200)>>5);
    clock|=((datacells&0x0080)>>4);
    clock|=((datacells&0x0020)>>3);
    clock|=((datacells&0x0008)>>2);
    clock|=((datacells&0x0002)>>1);

    // Extract data byte
    data=((datacells&0x4000)>>7);
    data|=((datacells&0x1000)>>6);
    data|=((datacells&0x0400)>>5);
    data|=((datacells&0x0100)>>4);
    data|=((datacells&0x0040)>>3);
    data|=((datacells&0x0010)>>2);
    data|=((datacells&0x0004)>>1);
    data|=((datacells&0x0001)>>0);

    // Detect standard address marks
    switch (datacells)
    {
      case 0xf77a: // d7 fc
        if (debug)
          printf("\n[%x] Index Address Mark\n", datapos);
        blocktype=data;
        bitlen=0;
        hadAM=1;
        break;

      case 0xf57e: // c7 fe
        if (debug)
          printf("\n[%x] ID Address Mark\n", datapos);
        blocktype=data;
        blocksize=7;
        bitlen=0;
        hadAM=1;
        idpos=datapos;
        break;

      case 0xf56f: // c7 fb
        if (debug)
          printf("\n[%x] Data Address Mark, distance from ID %x\n", datapos, datapos-idpos);
        blocktype=data;
        bitlen=0;
        hadAM=1;
        break;

      case 0xf56a: // c7 f8
        if (debug)
          printf("\n[%x] Deleted Data Address Mark\n", datapos);
        blocktype=data;
        bitlen=0;
        hadAM=1;
        break;

      default:
        break;
    }

    // Process block data depending on type
    switch (blocktype)
    {
      case 0xf8: // Deleted data block
      case 0xfb: // Data block
        // force it to be standard DFS 256 bytes/sector (+blocktype+CRC)
        blocksize=SECTORSIZE+3;

        // Keep reading until we have the whole block in bitsteam[]
        if (bitlen<blocksize)
        {
          //printf(" %.4x %.2x %c %.2x %c\n", datacells, clock, ((clock>=' ')&&(clock<='~'))?clock:'.', data, ((data>=' ')&&(data<='~'))?data:'.');

          if (debug)
          {
            if ((clock!=0xff) && (bitlen>0))
              printf("Invalid CLOCK %.4x %.2x %c %.2x %c\n", datacells, clock, ((clock>=' ')&&(clock<='~'))?clock:'.', data, ((data>=' ')&&(data<='~'))?data:'.');
          }

          bitstream[bitlen++]=data;
        }
        else
        {
          // All the bytes for this "data" block have been read, so process them
          if (debug)
            printf("  %.2x CRC %.2x%.2x", blocktype, bitstream[bitlen-2], bitstream[bitlen-1]);

          dataCRC=(calc_crc(&bitstream[0], bitlen-2)==((bitstream[bitlen-2]<<8)|bitstream[bitlen-1]))?GOODDATA:BADDATA;

          // Report if the CRC matches
          if (debug)
          {
            if (dataCRC==GOODDATA)
              printf(" OK\n");
            else
              printf(" BAD\n");
          }

          // Sanitise the data from the most recent unused address mark
          if ((track<MAXTRACKS) && (track<maxtracks) && (sector<SECTORSPERTRACK))
          {
            unsigned int sectorsize=(blocksize-3);
            unsigned int tracksize=(sectorsize*10);

            // Check the track number matches
            if (track!=current_track)
            {
              if (debug)
                printf("*** Track ID mismatch %d != %d ***\n", track, current_track);
 
              // Override the read track number with the track we should be on
              track=current_track;
            }

            // See if we need this sector, store it if current status is either EMPTY or BAD
            if (sectorstatus[current_head][(SECTORSPERTRACK*track)+sector]!=GOODDATA)
            {
              memcpy(&wholedisk[current_head][(track*tracksize)+(sector*sectorsize)], &bitstream[1], sectorsize);

              sectorstatus[current_head][(SECTORSPERTRACK*track)+sector]=dataCRC;
            }
          }
          else
          {
            if (debug)
              printf("  Invalid ID Track %d Sector %d\n", track, sector);
          }

          // Do a catalogue if we haven't already and sector 00 and 01 have been read correctly for this side
          if ((info==0) && (sectorstatus[current_head][0]==GOODDATA) && (sectorstatus[current_head][1]==GOODDATA))
          {
            showinfo(current_head);
            info++;
          }

          // Require subsequent data blocks to have a valid ID block
          track=0xff;
          head=0xff;
          sector=0xff;
          datasize=0;
          idpos=0;

          blocktype=0;
        }
        break;

      case 0xfe: // ID block
        if (bitlen<blocksize)
        {
          bitstream[bitlen++]=data;
        }
        else
        {
          dataCRC=(calc_crc(&bitstream[0], bitlen-2)==((bitstream[5]<<8)|bitstream[6]))?GOODDATA:BADDATA;

          if (debug)
          {
            printf("Track %d ", bitstream[1]);
            printf("Head %d ", bitstream[2]);
            printf("Sector %d ", bitstream[3]);
            printf("Data size %d ", bitstream[4]);
            printf("CRC %.2x%.2x", bitstream[5], bitstream[6]);

            if (dataCRC==GOODDATA)
              printf(" OK\n");
            else
              printf(" BAD\n");
          }

          if (dataCRC==GOODDATA)
          {
            track=bitstream[1];
            head=bitstream[2];
            sector=bitstream[3];

            switch(bitstream[4])
            {
              case 0x00:
              case 0x01:
              case 0x02:
              case 0x03:
                datasize=(128<<bitstream[4])+3;

              default:
                if (debug)
                printf("Invalid record length %.2x\n", bitstream[4]);

                datasize=256+3;
                break;
            }
          }

          blocktype=0;
        }
        break;

      case 0xfc: // Index block
        if (debug)
          printf("Index address mark\n");
        blocktype=0;
        break;

      default:
        if (blocktype!=0)
        {
          if (debug)
            printf("** Unknown block address mark %.2x **\n", blocktype);
          blocktype=0;
        }
        break;
    }

    // Look for any GAP (outside of the data block/deleted data block) to resync bitstream
    if ((clock==0xff) && (data==0xff))
    {
        if (bitlen>=blocksize)
          hadAM=0;
    }

    if (hadAM==0)
      bits=16;
    else
      bits=0;
  }
}

void process(int attempt)
{
  int j,k, pos;
  char state,bi=0;
  unsigned char c, clock, data;
  int count, edges;
  unsigned long avg[50];
  int bitwidth=0;

  state=(spibuffer[0]&0x80)>>7;
  bi=state;
  count=0;
  datacells=0; edges=0;

  // Initialise table
  for (j=0; j<50; j++) avg[j]=0;

  // Try to clock the most common width of a "1"
  for (datapos=0;datapos<SPIBUFFSIZE; datapos++)
  {
    c=spibuffer[datapos];

    for (j=0; j<8; j++)
    {
      bi=((c&0x80)>>7);
      if (bi!=state)
      {
        state=1-state;

        edges++;

        if (edges==1)
        {
          if (state==0)
          {
            if ((count>=0) && (count<=50))
              avg[count]++;
          }

          edges=0;
          count=0;
        }
      }
      count++;

      c=c<<1;
    }
  }

  // Find most common width for a bit
  for (j=0; j<50; j++)
  {
    if (avg[j]>avg[bitwidth]) bitwidth=j;
  }

  if (attempt==0)
    printf("Bit-width for a '1' in samples : %d\n", bitwidth);

  // Second pass to extract the data
  state=(spibuffer[0]&0x80)>>7;
  bi=state;
  count=0;
  datacells=0; edges=0;

  // Initialise last sector ID mark to blank
  track=0xff;
  head=0xff;
  sector=0xff;
  datasize=0;
  blocktype=0;
  idpos=0;

  for (datapos=0;datapos<SPIBUFFSIZE; datapos++)
  {
    c=spibuffer[datapos];

    for (j=0; j<8; j++)
    {
      bi=((c&0x80)>>7);
      if (bi!=state)
      {
        state=1-state;

        edges++;

        if ((state==0) && (edges>=2))
        {
          if (count<67)
          {
            addbit(1);
          }
          else
          if (count<112)
          {
            addbit(0);
            addbit(1);
          }
          else
          {
            addbit(0);
            addbit(0);
            addbit(1);
          }

/*
          if ((count>=(bitwidth*4)) && (count<=(bitwidth*5)))
          {
            addbit(1);
          }

          if ((count>=(bitwidth*8)) && (count<=(bitwidth*10)))
          {
            addbit(0);
            addbit(1);
          }
*/
          edges=0;
          count=0;
        }
      }
      count++;

      c=c<<1;
    }
  }
}

int init()
{
  int i;

  if (!bcm2835_init()) return 0;

  bcm2835_gpio_fsel(DS0_OUT, GPIO_OUT);
  bcm2835_gpio_clr(DS0_OUT);

  bcm2835_gpio_fsel(MOTOR_ON, GPIO_OUT);
  bcm2835_gpio_clr(MOTOR_ON);

  bcm2835_gpio_fsel(DIR_SEL, GPIO_OUT);
  bcm2835_gpio_clr(DIR_SEL);

  bcm2835_gpio_fsel(DIR_STEP, GPIO_OUT);
  bcm2835_gpio_clr(DIR_STEP);

  bcm2835_gpio_fsel(WRITE_GATE, GPIO_OUT);
  bcm2835_gpio_clr(WRITE_GATE);

  bcm2835_gpio_fsel(SIDE_SELECT, GPIO_OUT);
  bcm2835_gpio_clr(SIDE_SELECT);

  bcm2835_gpio_fsel(WRITE_PROTECT, GPIO_IN);
  bcm2835_gpio_set_pud(WRITE_PROTECT, PULL_UP);

  bcm2835_gpio_fsel(TRACK_0, GPIO_IN);
  bcm2835_gpio_set_pud(TRACK_0, PULL_UP);

  bcm2835_gpio_fsel(INDEX_PULSE, GPIO_IN);
  bcm2835_gpio_set_pud(INDEX_PULSE, PULL_UP);

  //bcm2835_gpio_fsel(READ_DATA, GPIO_IN);
  //bcm2835_gpio_set_pud(READ_DATA, PULL_UP);

//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_1024); // 390.625kHz on RPI3
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_512); // 781.25kHz on RPI3
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_256); // 1.562MHz on RPI3
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_128); // 3.125MHz on RPI3
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_64); // 6.250MHz on RPI3
  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_32); // 12.5MHz on RPI3 ***** WORKS
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_16); // 25MHz on RPI3
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_8); // 50MHz on RPI3
//  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_4); // 100MHz on RPI3 - UNRELIABLE

  bcm2835_spi_setDataMode(BCM2835_SPI_MODE2); // CPOL = 1, CPHA = 0 

  bcm2835_spi_begin(); // sets all correct pin modes

  return 1;
}

// stop motor and release drive
void stopMotor()
{
  bcm2835_gpio_clr(MOTOR_ON);
  bcm2835_gpio_clr(DS0_OUT);
  bcm2835_gpio_clr(DIR_SEL);
  bcm2835_gpio_clr(DIR_STEP);
  bcm2835_gpio_clr(SIDE_SELECT);
  printf("Stopped motor\n");
}

void exitFunction()
{
  printf("Exit function\n");
  stopMotor();
  bcm2835_spi_end();
  bcm2835_close();
}

void sig_handler(const int sig)
{
  if (sig==SIGSEGV)
    printf("SEG FAULT\n");

  stopMotor();
  bcm2835_spi_end();
  exit(0);
}

void seekToTrackZero(int newDrive)
{
  if (newDrive) // wait for a few milliseconds for track_zero to be set/reset
    delay(10);

  int track0 = bcm2835_gpio_lev(TRACK_0);

  if (track0 == LOW)
    printf("Seeking to track zero\r\n");

  while (track0 == LOW)
  {
    bcm2835_gpio_clr(DIR_SEL);
    bcm2835_gpio_set(DIR_STEP);
    delayMicroseconds(8);
    bcm2835_gpio_clr(DIR_STEP);
    delay(40); // wait maximum time for step
    track0 = bcm2835_gpio_lev(TRACK_0);
  }

  printf("At track zero\n");
  current_track = 0;
}

void seekToTrack(int track)
{
  // Sanity check the requested track is within range
  if ((track<0) || (track>=MAXTRACKS))
    exitFunction();

  // Sanity check our "current" track is within range
  if ((current_track<0) || (current_track>=MAXTRACKS))
    exitFunction();

  // Do nothing if we're already at the requested track
  if (track == current_track)
    return;

  // For seek to track 00, seek until TRACK00 signal
  if (track == 0)
  {
    seekToTrackZero(0);
    return;
  }

  printf("Seeking from track %d to track %d\n", current_track, track);

  // Seek towards inside of disk
  while (current_track < track)
  {
    bcm2835_gpio_set(DIR_SEL);
    bcm2835_gpio_set(DIR_STEP);
    delayMicroseconds(8);
    bcm2835_gpio_clr(DIR_STEP);
    delay(40); // wait maximum time for step
    current_track++;
  }

  // Seek towards outside of disk
  while (current_track > track)
  {
    // Prevent stepping past track 00
    if (bcm2835_gpio_lev(TRACK_0)==LOW)
      break;

    bcm2835_gpio_clr(DIR_SEL);
    bcm2835_gpio_set(DIR_STEP);
    delayMicroseconds(8);
    bcm2835_gpio_clr(DIR_STEP);
    delay(40); // wait maximum time for step
    current_track--;
  }
}

// Request data from side 0 = upper (label), or side 1 = lower side of disk
void sideSelect(int side)
{
  // First check the requested side is within range
  if ((side==0) || (side==(MAXHEADS-1)))
  {
    if (side==0)
      bcm2835_gpio_clr(SIDE_SELECT);
    else
      bcm2835_gpio_set(SIDE_SELECT);

    current_head=side;
    printf("Accessing side %d\n", current_head);
  }
}

int detect_disk()
{
  int retval=NODISK;
  unsigned long i;

  // Select drive
  bcm2835_gpio_set(DS0_OUT);

  // Start MOTOR
  bcm2835_gpio_set(MOTOR_ON);

  delay(500);

  // We need to see the index pulse go high to prove there is a drive with a disk in it
  for (i=0; i<200; i++)
  {
    // A drive with no disk will have an index pulse "stuck" low, so make sure it goes high within timeout
    if (bcm2835_gpio_lev(INDEX_PULSE)!=LOW)
      break;

    delay(2);
  }

  if (i<200)
  {
    for (i=0; i<200; i++)
    {
      // Make sure index pulse goes low again, i.e. it's pulsing (disk going round)
      if (bcm2835_gpio_lev(INDEX_PULSE)==LOW)
        break;

      delay(2);
    }

    if (i<200) retval=HAVEDISK;
  }

  // Test to see if there is no drive
  if ((retval!=HAVEDISK) && (bcm2835_gpio_lev(TRACK_0)==LOW) && (bcm2835_gpio_lev(WRITE_PROTECT)==LOW) && (bcm2835_gpio_lev(INDEX_PULSE)==LOW))
  {
    // Likely no drive
    retval=NODRIVE;
  }

  // If we have a disk and drive, then seek to track 00
  if (retval==HAVEDISK)
  {
    seekToTrackZero(1);
  }

  delay(1000);

  // Stop MOTOR
  bcm2835_gpio_clr(MOTOR_ON);

  // De-select drive
  bcm2835_gpio_clr(DS0_OUT);

  return retval;
}


int main(int argc,char **argv)
{
  unsigned int i, j, trackpos;
  unsigned char retry, side, drivestatus;

  if (geteuid() != 0)
  {
    fprintf(stderr,"Must be run as root\n");
    exit(1);
  }

  printf("Start\n");

  if (!init())
  {
    printf("Failed init\n");
    return 1;
  }

  // Install signal handlers to make sure motor is stopped
  atexit(exitFunction);
  signal(SIGINT, sig_handler); // Ctrl-C
  signal(SIGSEGV, sig_handler); // Seg fault
  signal(SIGTERM, sig_handler); // Termination request

  drivestatus=detect_disk();

  if (drivestatus==NODRIVE)
  {
    fprintf(stderr, "Failed to detect drive\n");
    return 2;
  }

  if (drivestatus==NODISK)
  {
    fprintf(stderr, "Failed to detect disk in drive\n");
    return 3;
  }

  // Select drive
  bcm2835_gpio_set(DS0_OUT);

  // Start MOTOR
  bcm2835_gpio_set(MOTOR_ON);

  sleep(1);

  // Determine if head is at track 00
  if (bcm2835_gpio_lev(TRACK_0)==LOW)
    printf("Starting not at track zero\n");
  else
    printf("Starting at track zero\n");

  // Determine if disk in drive is write protected
  if (bcm2835_gpio_lev(WRITE_PROTECT)==LOW)
    printf("Disk is writeable\n");
  else
    printf("Disk is write-protected\n");

  // Work out what type of capture we are doing
  if (argc==2)
  {
    if (strstr(argv[1], ".ssd")!=NULL)
    {
      singlesided=1;

      diskimage=fopen(argv[1], "w+");
      if (diskimage==NULL)
        printf("Unable to save disk image\n");
      else
        capturetype=DISKIMG;
    }
    else
    if (strstr(argv[1], ".dsd")!=NULL)
    {
      diskimage=fopen(argv[1], "w+");
      if (diskimage==NULL)
        printf("Unable to save disk image\n");
      else
        capturetype=DISKIMG;
    }
    else
    if (strstr(argv[1], ".raw")!=NULL)
    {
      rawdata=fopen(argv[1], "w+");
      if (rawdata==NULL)
        printf("Unable to save rawdata\n");
      else
        capturetype=DISKRAW;
    }
  }

  // Mark each sector as being unread
  for (i=0; i<SECTORSPERSIDE; i++)
  {
    for (j=0; j<MAXHEADS; j++)
      sectorstatus[j][i]=NODATA;
  }

  maxtracks=80; // TODO

  for (side=0; side<MAXHEADS; side++)
  {
    info=0; // Request a directory listing for this side of the disk
    sideSelect(side);

    seekToTrackZero(1);

    for (i=0; i<maxtracks; i++)
    {

      for (retry=0; retry<RETRIES; retry++)
      {
        seekToTrack(i);

        // Wait for a bit after seek to allow drive speed to settle
        sleep(1);

        if ((retry==0) && (debug))
        {
          printf("Sampling data\n");
        }

        // Wait for transition from LOW to HIGH prior to sampling to align as much as possible with index
        // Ratio seems to be about 45.6 LOW to 1 HIGH
        while (bcm2835_gpio_lev(INDEX_PULSE)!=LOW)
        {
        }

        while (bcm2835_gpio_lev(INDEX_PULSE)==LOW)
        {
        }

        // Sampling data
        bcm2835_spi_transfern((char *)spibuffer, sizeof(spibuffer));

        // Process the raw sample data to extract FM encoded data
        if (capturetype!=DISKRAW)
        {
          process(retry);

          // Determine if we have successfully read the whole track
          trackpos=(SECTORSPERTRACK*i);
          if ((sectorstatus[side][trackpos+0]==GOODDATA) &&
              (sectorstatus[side][trackpos+1]==GOODDATA) &&
              (sectorstatus[side][trackpos+2]==GOODDATA) &&
              (sectorstatus[side][trackpos+3]==GOODDATA) &&
              (sectorstatus[side][trackpos+4]==GOODDATA) &&
              (sectorstatus[side][trackpos+5]==GOODDATA) &&
              (sectorstatus[side][trackpos+6]==GOODDATA) &&
              (sectorstatus[side][trackpos+7]==GOODDATA) &&
              (sectorstatus[side][trackpos+8]==GOODDATA) &&
              (sectorstatus[side][trackpos+9]==GOODDATA))
            break;

          printf("Retry attempt %d, sectors ", retry+1);
          for (j=0; j<SECTORSPERTRACK; j++)
            if (sectorstatus[side][trackpos+j]!=GOODDATA) printf("%.2d ", j);
          printf("\n");
        }
        else
          break; // No retries in RAW mode
      }

      if (capturetype!=DISKRAW)
      {
        // If we're on side 1 track 1 and no second catalogue found, then assume single sided
        if ((side==1) && (i==1))
        {
          if (info==0)
          {
            singlesided=1;

            printf("Single sided disk\n");
          }
          else
            printf("Double sided disk\n");
        }

        if (retry>=RETRIES)
          printf("I/O error reading head %d track %d\n", current_head, i);
      }
      else
      {
        // Write the raw sample data if required
        if (rawdata!=NULL)
          fwrite(spibuffer, 1, sizeof(spibuffer), rawdata);
      }

      // If we're only doing a catalogue, then don't read any more tracks from this side
      if (capturetype==DISKCAT)
        break;
    } // track loop

    // If disk image is being written but we only have a single sided disk, then stop here
    if (singlesided)
      break;

    printf("\n");
  } // side loop

  // Return the disk head to track 0 following disk imaging
  seekToTrackZero(0);

  printf("Finished\n");

  // Output list of good/bad tracks, but only if we're doing a whole disk image
  if (diskimage!=NULL)
  {
    for (i=0; i<SECTORSPERSIDE; i++)
    {
      if ((i%10)==0) printf("%d:%.2d ", 0, i/SECTORSPERTRACK);

      switch (sectorstatus[0][i])
      {
        case NODATA:
          printf("_");
          break;

        case BADDATA:
          printf("/");
          break;

        case GOODDATA:
          printf("*");
          break;

        default:
          break;
      }
      if ((i%10)==9) printf("\n");
    }
    printf("\n");

    if (!singlesided)
    {
      for (i=0; i<SECTORSPERSIDE; i++)
      {
        if ((i%10)==0) printf("%d:%.2d ", 1, i/SECTORSPERTRACK);

        switch (sectorstatus[1][i])
        {
          case NODATA:
            printf("_");
            break;

          case BADDATA:
            printf("/");
            break;

          case GOODDATA:
            printf("*");
            break;

          default:
            break;
        }
        if ((i%10)==9) printf("\n");
      }
      printf("\n");
    }
  }

  // Write the data to disk image file (if required)
  if (diskimage!=NULL)
  {
    for (i=0; ((i<MAXTRACKS) && (i<maxtracks)); i++)
    {
      for (j=0; j<SECTORSPERTRACK; j++)
      {
        // Write
        fwrite(&wholedisk[0][(i*TRACKSIZE)+(j*SECTORSIZE)], 1, SECTORSIZE, diskimage);
      }

      // Write DSD interlaced as per BeebEm
      if (!singlesided)
      {
        for (j=0; j<SECTORSPERTRACK; j++)
          fwrite(&wholedisk[1][(i*TRACKSIZE)+(j*SECTORSIZE)], 1, SECTORSIZE, diskimage);
      }
    }
  }

  // Close disk image files (if open)
  if (diskimage!=NULL) fclose(diskimage);
  if (rawdata!=NULL) fclose(rawdata);

  stopMotor();

  return 0;
}