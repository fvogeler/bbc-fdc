#ifndef _DISKSTORE_H_
#define _DISKSTORE_H_

#include <stdint.h>

// For sector status
#define NODATA 0
#define BADDATA 1
#define GOODDATA 2

// Modulation types
#define MODFM 0
#define MODMFM 1
#define MODGCR 2
#define MODAPPLEGCR 3

// Head interlacing types
#define SEQUENCED 0
#define INTERLEAVED 1

// Sector sorting criteria
#define SORTBYID 0
#define SORTBYPOS 1

typedef struct DiskSector
{
  // Physical position of sector on disk
  uint8_t physical_track;
  uint8_t physical_head;
  uint8_t physical_sector;

  // Logical position of sector from IDAM
  unsigned long id_pos;
  uint8_t logical_track;
  uint8_t logical_head;
  uint8_t logical_sector;
  uint8_t logical_size;
  unsigned int idcrc;

  // Sector data
  unsigned char modulation;
  unsigned long data_pos;
  unsigned long data_endpos;
  unsigned int datatype;
  unsigned int datasize;
  unsigned char *data;
  unsigned int datacrc;

  struct DiskSector *next;
} Disk_Sector;

// Linked list
extern Disk_Sector *Disk_SectorsRoot;

// Summary information
extern int diskstore_mintrack;
extern int diskstore_maxtrack;
extern int diskstore_minhead;
extern int diskstore_maxhead;
extern int diskstore_minsectorsize;
extern int diskstore_maxsectorsize;
extern int diskstore_minsectorid;
extern int diskstore_maxsectorid;

// For absolute disk access
extern int diskstore_abstrack;
extern int diskstore_abshead;
extern int diskstore_abssector;
extern int diskstore_abssecoffs;
extern unsigned long diskstore_absoffset;

// Initialise disk storage
extern void diskstore_init(const int debug, const int usepll);

// Add a sector to the disk storage
extern int diskstore_addsector(const unsigned char modulation, const uint8_t physical_track, const uint8_t physical_head, const uint8_t logical_track, const uint8_t logical_head, const uint8_t logical_sector, const uint8_t logical_size, const long id_pos, const unsigned int idcrc, const long data_pos, const unsigned int datatype, const unsigned int datasize, const unsigned char *data, const unsigned int datacrc);

// Search for a sector within the disk storage
extern Disk_Sector *diskstore_findexactsector(const uint8_t physical_track, const uint8_t physical_head, const uint8_t logical_track, const uint8_t logical_head, const uint8_t logical_sector, const uint8_t logical_size, const unsigned int idcrc, const unsigned int datatype, const unsigned int datasize, const unsigned int datacrc);
extern Disk_Sector *diskstore_findlogicalsector(const uint8_t logical_track, const uint8_t logical_head, const uint8_t logical_sector);
extern Disk_Sector *diskstore_findhybridsector(const uint8_t physical_track, const uint8_t physical_head, const uint8_t logical_sector);
extern Disk_Sector *diskstore_findnthsector(const uint8_t physical_track, const uint8_t physical_head, const unsigned char nth_sector);

// Processing of sectors
extern unsigned char diskstore_countsectors(const uint8_t physical_track, const uint8_t physical_head);
extern unsigned int diskstore_countsectormod(const unsigned char modulation);
extern void diskstore_sortsectors(const int sortmethod, const int rotations);

// Dump the contents of the disk storage for debug purposes
extern void diskstore_dumpsectorlist();
extern void diskstore_dumpbadsectors(FILE* fh);
extern void diskstore_dumplayoutmap(const int rotations);

// Absolute data access
extern unsigned long diskstore_absoffset;
extern void diskstore_absoluteseek(const unsigned long offset, const int interlacing, const int maxtracks);
extern unsigned long diskstore_absoluteread(char *buffer, const unsigned long bufflen, const int interlacing, const int maxtracks);

// Calculate disk CRCs
extern uint32_t diskstore_calcdiskcrc(const uint8_t physical_head);

#endif
