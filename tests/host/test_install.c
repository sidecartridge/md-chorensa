// units: rp/src/install.c rp/src/install_check.c rp/src/zipread.c rp/src/sound_convert.c rp/src/crc32.c rp/src/sha256.c rp/src/chacha20.c third_party/miniz/miniz_tinfl.c
// cflags: -I shim_install -I ../../tools/xlat -I ../../third_party/miniz -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME
/* The install gate (install.c) over the host's files, through a FatFs
 * stand-in: no zip in the folder; a zip that is not the repository's (the
 * zip reader's fixture); a finished manifest with its files there, ready at
 * once without a zip and without a screen. With CHORENSHA_ZIP set to the
 * repository's zip: the whole conversion (EFFECTS.SE8 compared byte for
 * byte with CHORENSHA_EFFECTS, the songs' sizes with CHORENSHA_SONGS'
 * ffmpeg-made ones, when set; CHORENSHA_REFERENCE: a folder of outputs the
 * whole run must equal byte for byte), a second start that makes nothing,
 * and a deleted output made again alone. */
#define _GNU_SOURCE /* fnmatch's FNM_CASEFOLD, symlink() under -std=c11 */
#include <fnmatch.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "install.h"
#include "palette.h"
#include "pico/time.h"
#include "select.h"
#include "st_session.h"
#include "test.h"
#include "xl_store.h"
#include "zipread.h"

/* --- FatFs over the host's files ------------------------------------------- */

static char s_created[64][32];
static int s_creations;

/* A card taken out: every call fails as the driver's would; counted. */
static int s_card_gone, s_card_calls;
FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  s_card_calls++;
  if (s_card_gone) return FR_DISK_ERR;
  const char *how = (mode & FA_CREATE_ALWAYS) ? "wb" : (mode & 0x30) == FA_OPEN_APPEND ? "ab" : "rb";
  FILE *f = fopen(path, how);
  if (f == NULL) return FR_NO_FILE;
  if (mode & FA_CREATE_ALWAYS && s_creations < 64) {
    const char *slash = strrchr(path, '/');
    snprintf(s_created[s_creations++], 32, "%s", slash ? slash + 1 : path);
  }
  fp->host = f;
  return FR_OK;
}
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  *br = (UINT)fread(buff, 1, btr, fp->host);
  return ferror(fp->host) ? FR_DISK_ERR : FR_OK;
}
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
  *bw = (UINT)fwrite(buff, 1, btw, fp->host);
  return *bw == btw ? FR_OK : FR_DISK_ERR;
}
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) { return fseek(fp->host, (long)ofs, SEEK_SET) ? FR_DISK_ERR : FR_OK; }
FRESULT f_close(FIL *fp) {
  if (fp->host == NULL) return FR_OK;
  int r = fclose(fp->host);
  fp->host = NULL;
  return r ? FR_DISK_ERR : FR_OK;
}
FSIZE_t f_size(FIL *fp) {
  long at = ftell(fp->host);
  fseek(fp->host, 0, SEEK_END);
  long n = ftell(fp->host);
  fseek(fp->host, at, SEEK_SET);
  return (FSIZE_t)n;
}
FSIZE_t f_tell(FIL *fp) { return (FSIZE_t)ftell(fp->host); }
FRESULT f_stat(const char *path, FILINFO *fno) {
  struct stat st;
  s_card_calls++;
  if (s_card_gone) return FR_DISK_ERR;
  if (stat(path, &st)) return FR_NO_FILE;
  fno->fsize = (FSIZE_t)st.st_size;
  return FR_OK;
}
FRESULT f_unlink(const char *path) { return unlink(path) ? FR_NO_FILE : FR_OK; }
char *f_gets(char *buff, int len, FIL *fp) { return fgets(buff, len, fp->host); }
FRESULT f_findnext(DIR *dp, FILINFO *fno) {
  glob_t *g = dp->host;
  while (g && dp->next < g->gl_pathc) {
    const char *path = g->gl_pathv[dp->next++];
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (fnmatch(dp->pattern, name, FNM_CASEFOLD) == 0) {
      snprintf(fno->fname, sizeof fno->fname, "%s", name);
      return FR_OK;
    }
  }
  fno->fname[0] = '\0';
  return FR_OK;
}
FRESULT f_findfirst(DIR *dp, FILINFO *fno, const char *path, const char *pattern) {
  static glob_t g;
  char all[512];
  snprintf(all, sizeof all, "%s/*", path);
  if (glob(all, 0, NULL, &g) != 0) g.gl_pathc = 0;
  dp->host = &g;
  dp->next = 0;
  snprintf(dp->pattern, sizeof dp->pattern, "%s", pattern);
  return f_findnext(dp, fno);
}
FRESULT f_closedir(DIR *dp) {
  if (dp->host) globfree(dp->host);
  dp->host = NULL;
  return FR_OK;
}

/* --- The screen, the keyboard, the clock --------------------------------- */

static int s_publishes, s_pumps;
static const unsigned char k_glyphs[96 * 8] = {0};
const struct FB_FONT font8x8 = {8, 8, 32, 96, k_glyphs};
/* Each screen drawn in bands of 16 lines, as the RP draws it; the bar
 * (installStats.bar) never goes back within a gate's run. */
static int s_bar = -1, s_bar_backwards;
static uint8_t s_frame[FB_CHUNKED_W * FB_CHUNKED_H];
void fb_draw_banded(fb_band_draw_t draw, const void *arg) {
  for (int y = 0; y < FB_CHUNKED_H; y += 16) draw(s_frame + y * FB_CHUNKED_W, arg, y, y + 16);
  s_publishes++;
  if ((int)installStats.bar < s_bar) s_bar_backwards++;
  s_bar = (int)installStats.bar;
}
void palette_set(const uint16_t entries[PALETTE_ENTRIES]) { (void)entries; }
void ikbd_set_input_mode(int mode) { (void)mode; }
bool ikbd_pop_key(ikbd_key_event_t *key) { (void)key; return false; }
void ikbd_pump(void) {}
void fb_pump_rom3(void) {}
void select_poll(void) {}
void st_session_return_to_booster(void) {}
bool st_session_consume_boot(void) { return false; }
void fb_republish(void) {}
uint32_t time_us_32(void) { return (uint32_t)((uint64_t)clock() * 1000000u / CLOCKS_PER_SEC); }
static void pump(void) { s_pumps++; }

static uint32_t s_work[ZIP_WORK_BYTES / 4], s_song_work[INSTALL_SONG_WORK_BYTES / 4];

static const char *s_firmware = "test-firmware";

/* The ten images, as if made for s_firmware: their headers only. */
static void fake_images(const char *folder) {
  static const char *const names[] = {"COMMON.IMG", "MENUS.IMG",  "STAGE1.IMG", "STAGE2.IMG",
                                      "STAGE3.IMG", "STAGE4.IMG", "STAGE5.IMG", "STAGE6.IMG",
                                      "STAGE7.IMG", "START.STA"};
  for (int k = 0; k < 10; k++) {
    uint8_t h[200] = {0};
    uint32_t magic = k == 9 ? XL_STORE_START_MAGIC : XL_STORE_MAGIC;
    memcpy(h, &magic, 4);
    strcpy((char *)h + (k == 9 ? 8 : 20), s_firmware);
    char path[256];
    snprintf(path, sizeof path, "%s/%s", folder, names[k]);
    FILE *f = fopen(path, "wb");
    fwrite(h, 1, sizeof h, f);
    fclose(f);
  }
}

static void fresh_folder(const char *folder) {
  char command[512];
  snprintf(command, sizeof command, "rm -rf %s && mkdir -p %s", folder, folder);
  CHECK_EQ(system(command), 0);
}

static int same_file(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
  int same = fa && fb;
  while (same) {
    int ca = fgetc(fa), cb = fgetc(fb);
    if (ca != cb) same = 0;
    if (ca == EOF || cb == EOF) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return same;
}

static long size_of(const char *path) {
  struct stat st;
  return stat(path, &st) ? -1 : (long)st.st_size;
}

/* The outputs `path` makes, against CHORENSHA_REFERENCE's when set. */
static void compare(const char *folder, int path) {
  const char *reference = getenv("CHORENSHA_REFERENCE");
  if (!reference) return;
  static const char *const all[] = {"EFFECTS.SE8", "SZ2_B1", "SZ2_B2", "SZ2_B3", "SZ2_NM",
                                    "SZ2_S0", "SZ2_S1", "SZ2_S2", "SZ2_S3", "SZ2_S4",
                                    "SZ2_S5", "SZ2_S6", "SZ2_TMP"};
  for (int k = 0; k < 13; k++) {
    char ours[512], theirs[512];
    const char *ext = path == INSTALL_FOR_DMA ? "ST8" : "YM8";
    snprintf(ours, sizeof ours, k ? "%s/%s.%s" : "%s/%s", folder, all[k], ext);
    snprintf(theirs, sizeof theirs, k ? "%s/%s.%s" : "%s/%s", reference, all[k], ext);
    if (!same_file(ours, theirs)) printf("  differs from the reference: %s\n", ours);
    CHECK(same_file(ours, theirs));
  }
}

static void test_repository(const char *zip) {
  const char *folder = "build/install_real";
  char path[512];
  fresh_folder(folder);
  snprintf(path, sizeof path, "%s/repository.zip", folder);
  CHECK_EQ(symlink(zip, path), 0);
  /* The images: unlocked from CHORENSHA_PACK (keyed.py's output for the
   * images in CHORENSHA_IMAGES, made for CHORENSHA_PACK_FIRMWARE), compared
   * with them; else fake ones. */
  const char *pack = getenv("CHORENSHA_PACK"), *images = getenv("CHORENSHA_IMAGES");
  const char *pack_firmware = getenv("CHORENSHA_PACK_FIRMWARE");
  if (pack && images && pack_firmware) {
    char command[1024];
    snprintf(command, sizeof command, "cp %s/*.IMK %s/*.STK %s/", pack, pack, folder);
    CHECK_EQ(system(command), 0);
    s_firmware = pack_firmware;
    static const char *const names[] = {"COMMON.IMG", "MENUS.IMG",  "STAGE1.IMG", "STAGE2.IMG",
                                        "STAGE3.IMG", "STAGE4.IMG", "STAGE5.IMG", "STAGE6.IMG",
                                        "STAGE7.IMG", "START.STA"};
    (void)names;
  } else {
    fake_images(folder);
  }
  /* An STE's: the effects and the .ST8s only. */
  clock_t t = clock();
  s_bar = -1, s_bar_backwards = 0;
  CHECK_EQ(install_gate(folder, s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_READY);
  CHECK_EQ(s_bar_backwards, 0);
  CHECK(s_bar >= 220); /* full, or nearly, at the end (of 224) */
  printf("  the DMA chip's: %.1f s of CPU, %d files created, %d screens\n",
         (double)(clock() - t) / CLOCKS_PER_SEC, s_creations, s_publishes);
  int unlocked = pack && images && pack_firmware ? 10 : 0;
  CHECK_EQ(s_creations, unlocked + 1 + 13); /* the images, the manifest, the effects, the .ST8s */
  if (unlocked) {
    static const char *const names[] = {"COMMON.IMG", "MENUS.IMG",  "STAGE1.IMG", "STAGE2.IMG",
                                        "STAGE3.IMG", "STAGE4.IMG", "STAGE5.IMG", "STAGE6.IMG",
                                        "STAGE7.IMG", "START.STA"};
    for (int k = 0; k < 10; k++) {
      char ours[512], theirs[512];
      snprintf(ours, sizeof ours, "%s/%s", folder, names[k]);
      snprintf(theirs, sizeof theirs, "%s/%s", images, names[k]);
      if (!same_file(ours, theirs)) printf("  differs from the plain image: %s\n", ours);
      CHECK(same_file(ours, theirs));
    }
  }
  snprintf(path, sizeof path, "%s/SZ2_S1.YM8", folder);
  CHECK_EQ(size_of(path), -1);
  const char *effects = getenv("CHORENSHA_EFFECTS");
  if (effects) {
    snprintf(path, sizeof path, "%s/EFFECTS.SE8", folder);
    CHECK(same_file(path, effects));
  }
  compare(folder, INSTALL_FOR_DMA);
  /* The card moved to a plain ST: the .YM8s are added, nothing else. */
  s_creations = 0, s_bar = -1, s_bar_backwards = 0;
  CHECK_EQ(install_gate(folder, s_work, s_song_work, INSTALL_FOR_YM, s_firmware, pump), INSTALL_READY);
  CHECK_EQ(s_creations, 12);
  CHECK_EQ(s_bar_backwards, 0);
  compare(folder, INSTALL_FOR_YM);
  /* Started again on either: nothing made, no screen. */
  for (int p = 0; p < 2; p++) {
    s_creations = 0, s_publishes = 0;
    CHECK_EQ(install_gate(folder, s_work, s_song_work, p, s_firmware, pump), INSTALL_READY);
    CHECK_EQ(s_creations, 0);
    CHECK_EQ(s_publishes, 0);
  }
  /* An output deleted: made again alone, on its path. */
  snprintf(path, sizeof path, "%s/SZ2_S3.YM8", folder);
  long was = size_of(path);
  CHECK_EQ(unlink(path), 0);
  s_creations = 0;
  CHECK_EQ(install_gate(folder, s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_READY);
  CHECK_EQ(s_creations, 0); /* the STE needs no .YM8 */
  CHECK_EQ(install_gate(folder, s_work, s_song_work, INSTALL_FOR_YM, s_firmware, pump), INSTALL_READY);
  CHECK_EQ(s_creations, 1);
  CHECK(strcmp(s_created[0], "SZ2_S3.YM8") == 0);
  CHECK_EQ(size_of(path), was);
  /* Without the zip, everything already made: ready. */
  snprintf(path, sizeof path, "%s/repository.zip", folder);
  CHECK_EQ(unlink(path), 0);
  CHECK_EQ(install_gate(folder, s_work, s_song_work, INSTALL_FOR_YM, s_firmware, pump), INSTALL_READY);
}

int main(void) {
  /* Nothing at all: the images are missing first. */
  fresh_folder("build/install_empty");
  char what[16];
  CHECK_EQ(install_ready("build/install_empty", INSTALL_FOR_DMA, s_firmware, what), INSTALL_EMPTY);
  CHECK_EQ(install_ready("build/no_such_folder", INSTALL_FOR_DMA, s_firmware, what), INSTALL_EMPTY);
  CHECK_EQ(install_gate("build/install_empty", s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_NO_PACK);
  /* The images there, no zip. */
  fake_images("build/install_empty");
  CHECK_EQ(install_ready("build/install_empty", INSTALL_FOR_DMA, s_firmware, what), INSTALL_MISSING);
  CHECK(strcmp(what, "INSTALL.TXT") == 0);
  CHECK_EQ(install_gate("build/install_empty", s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_NO_ZIP);
  /* Images for another firmware, and a keyed copy for another again. */
  CHECK_EQ(install_gate("build/install_empty", s_work, s_song_work, INSTALL_FOR_DMA, "other-firmware", pump), INSTALL_NO_PACK);
  FILE *k = fopen("build/install_empty/COMMON.IMK", "wb");
  uint8_t kh[64] = {'C', 'R', 'S', 'K', 1};
  strcpy((char *)kh + 16, "COMMON.IMG");
  strcpy((char *)kh + 32, "a-third-firmware");
  fwrite(kh, 1, sizeof kh, k);
  fclose(k);
  CHECK_EQ(install_gate("build/install_empty", s_work, s_song_work, INSTALL_FOR_DMA, "other-firmware", pump), INSTALL_OLD_PACK);
  /* A zip that holds a program but not the tested members. */
  fresh_folder("build/install_wrong");
  fake_images("build/install_wrong");
  CHECK_EQ(system("cp build/zip_fixture.zip build/install_wrong/repo.ZIP"), 0);
  CHECK_EQ(install_gate("build/install_wrong", s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_BAD_MEMBER);
  /* A finished manifest, its files there: ready without a zip or a screen. */
  fresh_folder("build/install_done");
  fake_images("build/install_done");
  FILE *m = fopen("build/install_done/INSTALL.TXT", "w");
  fprintf(m, "CHORENSHA INSTALL %u\n", INSTALL_VERSION);
  static const char *const names[] = {"EFFECTS.SE8", "SZ2_B1", "SZ2_B2", "SZ2_B3", "SZ2_NM",
                                      "SZ2_S0", "SZ2_S1", "SZ2_S2", "SZ2_S3", "SZ2_S4",
                                      "SZ2_S5", "SZ2_S6", "SZ2_TMP"};
  for (int k = 0; k < 13; k++)
    for (int ym = 0; ym < (k ? 2 : 1); ym++) {
      char name[32], path[64];
      snprintf(name, sizeof name, k ? "%s.%s" : "%s", names[k], ym ? "YM8" : "ST8");
      snprintf(path, sizeof path, "build/install_done/%s", name);
      FILE *f = fopen(path, "wb");
      fwrite("xyz", 1, 3, f);
      fclose(f);
      fprintf(m, "%s 3\n", name);
    }
  fclose(m);
  s_publishes = 0;
  CHECK_EQ(install_gate("build/install_done", s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_READY);
  CHECK_EQ(s_publishes, 0);
  /* The cartridge's check: ready on both paths, for this firmware only. */
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_DMA, s_firmware, what), INSTALL_READY);
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_YM, s_firmware, NULL), INSTALL_READY);
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_YM, "other-firmware", what), INSTALL_OTHER_FIRMWARE);
  CHECK(strcmp(what, "COMMON.IMG") == 0);
  /* The card taken out: said at the first call. */
  s_card_gone = 1, s_card_calls = 0;
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_YM, s_firmware, what), INSTALL_NO_CARD);
  CHECK_EQ(s_card_calls, 1);
  s_card_gone = 0;
  /* One of them changed size: the gate wants the zip again; the check
   * fails on its path only. */
  FILE *f = fopen("build/install_done/SZ2_S4.ST8", "ab");
  fputc(0, f);
  fclose(f);
  CHECK_EQ(install_gate("build/install_done", s_work, s_song_work, INSTALL_FOR_DMA, s_firmware, pump), INSTALL_NO_ZIP);
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_DMA, s_firmware, what), INSTALL_INCOMPLETE);
  CHECK(strcmp(what, "SZ2_S4.ST8") == 0);
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_YM, s_firmware, what), INSTALL_READY);
  /* A damaged image, and one gone. */
  f = fopen("build/install_done/STAGE3.IMG", "wb");
  fputs("not an image", f);
  fclose(f);
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_YM, s_firmware, what), INSTALL_INCOMPLETE);
  CHECK(strcmp(what, "STAGE3.IMG") == 0);
  CHECK_EQ(unlink("build/install_done/STAGE3.IMG"), 0);
  CHECK_EQ(install_ready("build/install_done", INSTALL_FOR_YM, s_firmware, what), INSTALL_MISSING);
  CHECK(strcmp(what, "STAGE3.IMG") == 0);

  const char *zip = getenv("CHORENSHA_ZIP");
  if (zip) {
    s_creations = 0, s_publishes = 0;
    test_repository(zip);
  }
  TEST_END();
}
