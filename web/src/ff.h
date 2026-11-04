/* FatFs's calls for the install gate in a browser or in Node (crweb.c): the
 * folder's files are the JavaScript host's (the user's zip, the keyed
 * pack, the files made). Only what install.c uses. */
#ifndef CRWEB_FF_H
#define CRWEB_FF_H

#include <stdint.h>

typedef unsigned int UINT;
typedef uint64_t FSIZE_t;
typedef unsigned char BYTE;
typedef char TCHAR;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4, FR_NO_PATH = 5 } FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_APPEND 0x30

typedef struct {
  int handle; /* the host's, -1 closed */
  FSIZE_t fptr;
} FIL;

typedef struct {
  int next;
  char pattern[16];
} DIR;

typedef struct {
  FSIZE_t fsize;
  char fname[256];
} FILINFO;

FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_close(FIL *fp);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_unlink(const char *path);
char *f_gets(char *buff, int len, FIL *fp);
FRESULT f_findfirst(DIR *dp, FILINFO *fno, const char *path, const char *pattern);
FRESULT f_findnext(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);
FSIZE_t f_size(FIL *fp);
#define f_tell(fp) ((fp)->fptr)

#endif
