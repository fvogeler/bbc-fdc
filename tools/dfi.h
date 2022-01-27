#ifndef _DFI_H_
#define _DFI_H_

#include <stdio.h>

// Magic for new-style DiscFerret images
#define DFI_MAGIC "DFE2"

#define DFI_CARRY 0x7f

extern void dfi_writeheader(FILE *dfifile);

extern void dfi_writetrack(FILE *dfifile, const int track, const int side, const unsigned char *rawtrackdata, const unsigned long rawdatalength, const unsigned int rotations);

#endif
