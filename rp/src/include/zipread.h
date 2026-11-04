#ifndef ZIPREAD_H
#define ZIPREAD_H

#include <stdint.h>

/* Members of a zip archive on the card, read without unpacking the rest:
 * the central directory, then one member at a time, inflated in pieces
 * (miniz's tinfl) and handed to the caller. Plain C over a read callback,
 * no SDK and no allocation, so the host tests it. Classic zip only (no
 * zip64): an archive and its members under 4 GB, under 65,536 members. */

/* Read `length` bytes at `offset`: the bytes read, fewer only at the end. */
typedef int32_t (*zip_read_fn)(void *ctx, uint32_t offset, void *buffer, uint32_t length);
/* A piece of a member's content, in order: 0 to go on, else stop. */
typedef int (*zip_out_fn)(void *arg, const uint8_t *data, uint32_t length);

enum {
  ZIP_OK = 0,
  ZIP_ERR_READ = -1,      /* the read callback failed or came up short */
  ZIP_ERR_FORMAT = -2,    /* not a classic zip, or a damaged directory */
  ZIP_ERR_MISSING = -3,   /* no member at that path */
  ZIP_ERR_METHOD = -4,    /* neither stored nor deflated */
  ZIP_ERR_DATA = -5,      /* the deflated data does not inflate */
  ZIP_ERR_CHECK = -6,     /* the content's size or CRC-32 is not the directory's */
  ZIP_ERR_STOPPED = -7,   /* the caller's zip_out_fn asked to stop */
};

typedef struct {
  zip_read_fn read;
  void *ctx;
  uint32_t directory;     /* the central directory's offset */
  uint32_t directory_bytes;
  uint32_t members;
} zip_t;

typedef struct {
  uint32_t header;        /* the member's local header, its offset */
  uint32_t packed;        /* bytes in the archive */
  uint32_t size;          /* bytes of content */
  uint32_t crc;           /* the content's CRC-32 */
  uint16_t method;        /* 0 stored, 8 deflated */
} zip_member_t;

/* The work buffer zip_extract needs: the decompressor, its 32 KB window and
 * a buffer for what it reads. */
#define ZIP_WORK_BYTES (48u * 1024u)

/* The archive of `bytes` bytes behind `read`: its end record found and its
 * directory located. */
int zip_open(zip_t *zip, zip_read_fn read, void *ctx, uint32_t bytes);

/* The member at `path`, below the archive's top folder when every member
 * is inside one (GitHub's archives: "<repository>-<commit>/path"), else
 * from the archive's root. */
int zip_find(const zip_t *zip, const char *path, zip_member_t *member);

/* The member's content to `out`, in pieces of up to 32 KB, then its size
 * and CRC-32 checked against the directory's. `work`: ZIP_WORK_BYTES,
 * aligned to 4. */
int zip_extract(const zip_t *zip, const zip_member_t *member, void *work, zip_out_fn out,
                void *arg);

#endif
