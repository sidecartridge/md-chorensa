/**
 * File: crweb.c
 * Description: The cartridge's install gate (install.c) in a browser or in
 *              Node, over the JavaScript host's files, so that the files it
 *              makes for /CHORENSHA are the bytes the cartridge would make.
 *
 * The host gives the module, when it creates it:
 *   crOpen(name, mode)       a handle (0 or more) for the folder's file
 *                            `name`, or -1: mode 0 to read, 1 to create
 *                            (emptied), 2 to append
 *   crRead(handle, pos, len) a Uint8Array of its bytes from `pos` (fewer at
 *                            its end)
 *   crWrite(handle, pos, b)  its bytes at `pos`: a view of the module's
 *                            memory, to copy; 0, or nonzero on an error
 *   crSize(handle), crClose(handle)
 *   crStat(name)             the file's size, or -1 when there is none
 *   crUnlink(name)
 *   crList(index)            the folder's file `index`'s name, "" past them
 *   crProgress(bar, output)  the gate's bar (0 to 224) and the output it makes
 * and calls cr_install(path, firmware) once for each sound path (0 the DMA
 * chip's, 1 the YM's): the gate's status (install.h), 0 when all is made.
 */

#include <emscripten.h>
#include <stdbool.h>
#include <string.h>

#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "install.h"
#include "palette.h"
#include "select.h"
#include "st_session.h"
#include "zipread.h"

#define FOLDER "C"

EM_JS(int, js_open, (const char *name, int mode), { return Module.crOpen(UTF8ToString(name), mode); });
EM_JS(int, js_read, (int h, double pos, uint8_t *dst, uint32_t len), {
  var data = Module.crRead(h, pos, len);
  HEAPU8.set(data, dst);
  return data.length;
});
EM_JS(int, js_write, (int h, double pos, const uint8_t *src, uint32_t len), {
  return Module.crWrite(h, pos, HEAPU8.subarray(src, src + len));
});
EM_JS(double, js_size, (int h), { return Module.crSize(h); });
EM_JS(void, js_close, (int h), { Module.crClose(h); });
EM_JS(double, js_stat, (const char *name), { return Module.crStat(UTF8ToString(name)); });
EM_JS(void, js_unlink, (const char *name), { Module.crUnlink(UTF8ToString(name)); });
EM_JS(int, js_list, (int index, char *out, int max), {
  var name = Module.crList(index);
  stringToUTF8(name, out, max);
  return name.length;
});
EM_JS(void, js_progress, (int bar, int output), { Module.crProgress(bar, output); });

/* The name within the folder ("C/COMMON.IMG": "COMMON.IMG"). */
static const char *in_folder(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  int how = (mode & FA_CREATE_ALWAYS) ? 1 : (mode & FA_OPEN_APPEND) == FA_OPEN_APPEND ? 2 : 0;
  fp->handle = js_open(in_folder(path), how);
  if (fp->handle < 0) return FR_NO_FILE;
  fp->fptr = how == 2 ? (FSIZE_t)js_size(fp->handle) : 0;
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  int got = js_read(fp->handle, (double)fp->fptr, buff, btr);
  if (got < 0) return FR_DISK_ERR;
  fp->fptr += (FSIZE_t)got;
  *br = (UINT)got;
  return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
  if (js_write(fp->handle, (double)fp->fptr, buff, btw)) return FR_DISK_ERR;
  fp->fptr += btw;
  *bw = btw;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  fp->fptr = ofs;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  if (fp->handle >= 0) js_close(fp->handle);
  fp->handle = -1;
  return FR_OK;
}

FSIZE_t f_size(FIL *fp) { return (FSIZE_t)js_size(fp->handle); }

FRESULT f_stat(const char *path, FILINFO *fno) {
  double size = js_stat(in_folder(path));
  if (size < 0) return FR_NO_FILE;
  fno->fsize = (FSIZE_t)size;
  return FR_OK;
}

FRESULT f_unlink(const char *path) {
  js_unlink(in_folder(path));
  return FR_OK;
}

char *f_gets(char *buff, int len, FIL *fp) {
  int n = 0;
  while (n < len - 1) {
    uint8_t c;
    if (js_read(fp->handle, (double)fp->fptr, &c, 1) != 1) break;
    fp->fptr++;
    buff[n++] = (char)c;
    if (c == '\n') break;
  }
  buff[n] = '\0';
  return n ? buff : NULL;
}

/* The pattern "*.zip", the only one the gate asks for: any name ending .zip. */
static bool matches(const char *name, const char *pattern) {
  size_t n = strlen(name), p = strlen(pattern);
  if (p < 1 || pattern[0] != '*' || n < p - 1) return false;
  for (size_t k = 1; k < p; k++) {
    char a = name[n - (p - k)], b = pattern[k];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

FRESULT f_findnext(DIR *dp, FILINFO *fno) {
  for (;;) {
    int length = js_list(dp->next++, fno->fname, (int)sizeof fno->fname);
    if (length == 0) {
      fno->fname[0] = '\0';
      return FR_OK;
    }
    if (matches(fno->fname, dp->pattern)) return FR_OK;
  }
}

FRESULT f_findfirst(DIR *dp, FILINFO *fno, const char *path, const char *pattern) {
  (void)path;
  dp->next = 0;
  strncpy(dp->pattern, pattern, sizeof dp->pattern - 1u);
  dp->pattern[sizeof dp->pattern - 1u] = '\0';
  return f_findnext(dp, fno);
}

FRESULT f_closedir(DIR *dp) {
  (void)dp;
  return FR_OK;
}

/* --- The cartridge's screen, keyboard and session: none here --------------- */

static const unsigned char k_glyphs[96 * 8];
const struct FB_FONT font8x8 = {8, 8, 32, 96, k_glyphs};

void fb_draw_banded(fb_band_draw_t draw, const void *arg) {
  (void)draw, (void)arg;
  js_progress((int)installStats.bar, (int)installStats.output);
}
void fb_republish(void) {}
void palette_set(const uint16_t entries[PALETTE_ENTRIES]) { (void)entries; }
void ikbd_set_input_mode(int mode) { (void)mode; }
bool ikbd_pop_key(ikbd_key_event_t *key) {
  (void)key;
  return false;
}
void select_poll(void) {}
bool st_session_consume_boot(void) { return false; }
void st_session_return_to_booster(void) {}
static void pump(void) {}

static uint32_t s_work[ZIP_WORK_BYTES / 4u], s_song_work[INSTALL_SONG_WORK_BYTES / 4u];

EMSCRIPTEN_KEEPALIVE int cr_install(int path, const char *firmware) {
  return install_gate(FOLDER, s_work, s_song_work, path, firmware, pump);
}
