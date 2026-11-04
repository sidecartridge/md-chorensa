#include "zipread.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "miniz_tinfl.h"

#define END_RECORD 0x06054B50u     /* the end of central directory record */
#define END_BYTES 22u
#define END_SEARCH (END_BYTES + 0xFFFFu) /* the record, then at most a 64 KB comment */
#define ENTRY 0x02014B50u          /* a central directory entry */
#define ENTRY_BYTES 46u
#define LOCAL 0x04034B50u          /* a member's local header */
#define LOCAL_BYTES 30u
#define NAME_BYTES 256u
#define INPUT_BYTES 4096u

typedef struct {
  tinfl_decompressor inflater;
  uint8_t window[TINFL_LZ_DICT_SIZE];
  uint8_t input[INPUT_BYTES];
} work_t;

_Static_assert(sizeof(work_t) <= ZIP_WORK_BYTES, "ZIP_WORK_BYTES too small");

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_all(const zip_t *zip, uint32_t offset, void *buffer, uint32_t length) {
  return zip->read(zip->ctx, offset, buffer, length) == (int32_t)length ? ZIP_OK : ZIP_ERR_READ;
}

int zip_open(zip_t *zip, zip_read_fn read, void *ctx, uint32_t bytes) {
  zip->read = read;
  zip->ctx = ctx;
  if (bytes < END_BYTES) return ZIP_ERR_FORMAT;
  /* The end record, searched backwards over a comment of up to 64 KB, a
   * piece at a time (each overlapping the next by a record's length). */
  uint8_t piece[512];
  uint32_t floor = bytes > END_SEARCH ? bytes - END_SEARCH : 0;
  uint32_t end = bytes;
  for (;;) {
    uint32_t start = end - floor > sizeof piece ? end - sizeof piece : floor;
    uint32_t length = end - start;
    if (read_all(zip, start, piece, length) != ZIP_OK) return ZIP_ERR_READ;
    for (int32_t k = (int32_t)length - (int32_t)END_BYTES; k >= 0; k--) {
      if (le32(piece + k) != END_RECORD) continue;
      const uint8_t *e = piece + k;
      if (le16(e + 4) != 0 || le16(e + 6) != 0) return ZIP_ERR_FORMAT; /* one disk only */
      zip->members = le16(e + 10);
      zip->directory_bytes = le32(e + 12);
      zip->directory = le32(e + 16);
      if (zip->members == 0xFFFFu || zip->directory == 0xFFFFFFFFu) return ZIP_ERR_FORMAT;
      if ((uint64_t)zip->directory + zip->directory_bytes > start + (uint32_t)k)
        return ZIP_ERR_FORMAT;
      return ZIP_OK;
    }
    if (start == floor) return ZIP_ERR_FORMAT;
    end = start + END_BYTES - 1u;
  }
}

/* Whether a member's `name` is `path`: the whole name, or what follows its
 * first folder. */
static int same_path(const char *name, uint32_t length, const char *path) {
  size_t want = strlen(path);
  if (length == want && memcmp(name, path, want) == 0) return 1;
  const char *slash = memchr(name, '/', length);
  if (slash == NULL) return 0;
  uint32_t rest = length - (uint32_t)(slash + 1 - name);
  return rest == want && memcmp(slash + 1, path, want) == 0;
}

int zip_find(const zip_t *zip, const char *path, zip_member_t *member) {
  uint32_t at = zip->directory;
  uint32_t end = zip->directory + zip->directory_bytes;
  for (uint32_t n = 0; n < zip->members; n++) {
    uint8_t e[ENTRY_BYTES];
    char name[NAME_BYTES];
    if (at + ENTRY_BYTES > end) return ZIP_ERR_FORMAT;
    if (read_all(zip, at, e, ENTRY_BYTES) != ZIP_OK) return ZIP_ERR_READ;
    if (le32(e) != ENTRY) return ZIP_ERR_FORMAT;
    uint32_t name_length = le16(e + 28);
    uint32_t next = at + ENTRY_BYTES + name_length + le16(e + 30) + le16(e + 32);
    if (next > end) return ZIP_ERR_FORMAT;
    if (name_length <= NAME_BYTES) {
      if (read_all(zip, at + ENTRY_BYTES, name, name_length) != ZIP_OK) return ZIP_ERR_READ;
      if (same_path(name, name_length, path)) {
        member->method = le16(e + 10);
        member->crc = le32(e + 16);
        member->packed = le32(e + 20);
        member->size = le32(e + 24);
        member->header = le32(e + 42);
        if (member->packed == 0xFFFFFFFFu || member->size == 0xFFFFFFFFu ||
            member->header == 0xFFFFFFFFu)
          return ZIP_ERR_FORMAT;
        return ZIP_OK;
      }
    }
    at = next;
  }
  return ZIP_ERR_MISSING;
}

int zip_extract(const zip_t *zip, const zip_member_t *member, void *work, zip_out_fn out,
                void *arg) {
  uint8_t local[LOCAL_BYTES];
  if (read_all(zip, member->header, local, LOCAL_BYTES) != ZIP_OK) return ZIP_ERR_READ;
  if (le32(local) != LOCAL) return ZIP_ERR_FORMAT;
  uint32_t at = member->header + LOCAL_BYTES + le16(local + 26) + le16(local + 28);
  uint32_t left = member->packed;
  uint32_t crc = CRC32_START;
  uint32_t produced = 0;
  work_t *w = work;

  if (member->method == 0) { /* stored */
    if (member->packed != member->size) return ZIP_ERR_FORMAT;
    while (left) {
      uint32_t length = left < sizeof w->window ? left : sizeof w->window;
      if (read_all(zip, at, w->window, length) != ZIP_OK) return ZIP_ERR_READ;
      crc = crc32_update(crc, w->window, length);
      produced += length;
      if (out(arg, w->window, length)) return ZIP_ERR_STOPPED;
      at += length;
      left -= length;
    }
  } else if (member->method == 8) { /* deflated: raw deflate, the window wrapping */
    tinfl_init(&w->inflater);
    size_t window_at = 0;
    uint32_t in_at = 0, in_length = 0;
    for (;;) {
      if (in_at == in_length && left) {
        in_length = left < INPUT_BYTES ? left : INPUT_BYTES;
        if (read_all(zip, at, w->input, in_length) != ZIP_OK) return ZIP_ERR_READ;
        at += in_length;
        left -= in_length;
        in_at = 0;
      }
      size_t in_bytes = in_length - in_at;
      size_t out_bytes = sizeof w->window - window_at;
      tinfl_status status =
          tinfl_decompress(&w->inflater, w->input + in_at, &in_bytes, w->window,
                           w->window + window_at, &out_bytes, left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
      in_at += (uint32_t)in_bytes;
      if (out_bytes) {
        crc = crc32_update(crc, w->window + window_at, (uint32_t)out_bytes);
        produced += (uint32_t)out_bytes;
        if (out(arg, w->window + window_at, (uint32_t)out_bytes)) return ZIP_ERR_STOPPED;
        window_at = (window_at + out_bytes) & (sizeof w->window - 1u);
      }
      if (status == TINFL_STATUS_DONE) break;
      if (status < 0) return ZIP_ERR_DATA;
      if (status == TINFL_STATUS_NEEDS_MORE_INPUT && in_at == in_length && !left)
        return ZIP_ERR_DATA;
    }
  } else {
    return ZIP_ERR_METHOD;
  }
  if (produced != member->size || crc32_end(crc) != member->crc) return ZIP_ERR_CHECK;
  return ZIP_OK;
}
