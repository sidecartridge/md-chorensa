/* Host stand-in for FatFs's ff.h: the types and calls the firmware units
 * under test use. Each test implements the ones it needs: the audio tests
 * over files in memory (data, size, pos), test_install over the host's
 * files (host). */
#ifndef HOST_SHIM_FF_H
#define HOST_SHIM_FF_H

#include <stdint.h>

typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef unsigned char BYTE;
typedef char TCHAR;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_FILE = 4, FR_NO_PATH = 5, FR_DENIED = 7 } FRESULT;
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_APPEND 0x30

typedef struct {
  const BYTE *data;
  FSIZE_t size;
  FSIZE_t pos;
  void *host;
} FIL;

typedef struct {
  void *host;
  char pattern[32];
  unsigned next;
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
FSIZE_t f_tell(FIL *fp);

#endif
