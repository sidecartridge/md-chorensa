#include "install_check.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "install_members.h"
#include "xl_store.h"

/* Static: a FIL does not belong on core 0's stack. */
static FIL s_file;
static FILINFO s_info;

_Static_assert(INSTALL_OUTPUTS == 1u + 2u * INSTALL_SONGS, "an output for the effects, two for each song");

const char *const install_images[INSTALL_IMAGES] = {"COMMON.IMG", "MENUS.IMG",  "STAGE1.IMG",
                                                    "STAGE2.IMG", "STAGE3.IMG", "STAGE4.IMG",
                                                    "STAGE5.IMG", "STAGE6.IMG", "STAGE7.IMG",
                                                    XL_STORE_START_FILE};

void install_path_of(char *out, const char *folder, const char *name) {
  snprintf(out, INSTALL_PATH_BYTES, "%s/%s", folder, name);
}

void install_output_name(uint32_t k, char *out) {
  if (k == 0) {
    strcpy(out, INSTALL_EFFECTS_FILE);
    return;
  }
  const char *path = k_install_songs[(k - 1u) / 2u].path;
  const char *name = strrchr(path, '/') + 1;
  size_t stem = strcspn(name, ".");
  snprintf(out, 16, "%.*s.%s", (int)stem, name, (k - 1u) % 2u ? "YM8" : "ST8");
}

int install_needed(uint32_t k, int path) {
  return k == 0 || (int)((k - 1u) % 2u) == path;
}

uint32_t install_file_size(const char *folder, const char *name) {
  char path[INSTALL_PATH_BYTES];
  install_path_of(path, folder, name);
  return f_stat(path, &s_info) == FR_OK ? (uint32_t)s_info.fsize : 0;
}

int install_read_manifest(const char *folder, uint32_t sizes[INSTALL_OUTPUTS]) {
  char path[INSTALL_PATH_BYTES], line[64], header[32];
  memset(sizes, 0, INSTALL_OUTPUTS * sizeof *sizes);
  install_path_of(path, folder, INSTALL_MANIFEST);
  if (f_open(&s_file, path, FA_READ) != FR_OK) return 0;
  snprintf(header, sizeof header, INSTALL_MANIFEST_HEADER, INSTALL_VERSION);
  int current = f_gets(line, sizeof line, &s_file) && strcmp(line, header) == 0;
  while (current && f_gets(line, sizeof line, &s_file)) {
    char name[16];
    unsigned long size;
    if (sscanf(line, "%15s %lu", name, &size) != 2) continue;
    for (uint32_t k = 0; k < INSTALL_OUTPUTS; k++) {
      char want[16];
      install_output_name(k, want);
      if (strcmp(name, want) == 0) sizes[k] = (uint32_t)size;
    }
  }
  f_close(&s_file);
  return current;
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A FatFs result that is the card's, not a file's: no card, or one that
 * does not answer (taken out, put back uninitialised). */
static int card_error(FRESULT r) {
  return r != FR_OK && r != FR_NO_FILE && r != FR_NO_PATH;
}

int install_image_state(const char *folder, uint32_t k, const char *firmware) {
  char path[INSTALL_PATH_BYTES];
  uint8_t h[64];
  UINT got = 0;
  install_path_of(path, folder, install_images[k]);
  FRESULT r = f_open(&s_file, path, FA_READ);
  if (r != FR_OK) return card_error(r) ? INSTALL_NO_CARD : INSTALL_MISSING;
  r = f_read(&s_file, h, sizeof h, &got);
  f_close(&s_file);
  if (r != FR_OK) return INSTALL_NO_CARD;
  int start = k == INSTALL_IMAGES - 1u;
  if (got != sizeof h || le32(h) != (start ? XL_STORE_START_MAGIC : XL_STORE_MAGIC))
    return INSTALL_INCOMPLETE;
  size_t at = start ? offsetof(xl_store_start_t, firmware) : offsetof(xl_store_header_t, firmware);
  char fw[XL_STORE_FIRMWARE_ID + 1] = {0};
  memcpy(fw, h + at, XL_STORE_FIRMWARE_ID);
  return strncmp(fw, firmware, XL_STORE_FIRMWARE_ID - 1u) == 0 ? INSTALL_READY
                                                               : INSTALL_OTHER_FIRMWARE;
}

/* The first problem found, and its file's name in `what`. */
static int problem(int status, const char *name, char *what) {
  if (what) snprintf(what, 16, "%s", name);
  return status;
}

int install_ready(const char *folder, int path, const char *firmware, char *what) {
  if (what) what[0] = 0;
  FRESULT r = f_stat(folder, &s_info);
  if (r != FR_OK) return card_error(r) ? INSTALL_NO_CARD : INSTALL_EMPTY;
  /* The images first, then the manifest, then the sound it lists. */
  int status = INSTALL_READY, present = 0;
  char name[16];
  for (uint32_t k = 0; k < INSTALL_IMAGES; k++) {
    int image = install_image_state(folder, k, firmware);
    if (image == INSTALL_NO_CARD) return INSTALL_NO_CARD;
    if (image != INSTALL_MISSING) present++;
    if (image != INSTALL_READY && status == INSTALL_READY) status = problem(image, install_images[k], what);
  }
  char manifest[INSTALL_PATH_BYTES];
  install_path_of(manifest, folder, INSTALL_MANIFEST);
  r = f_stat(manifest, &s_info);
  if (card_error(r)) return INSTALL_NO_CARD;
  if (r != FR_OK && present == 0) return problem(INSTALL_EMPTY, "", what);
  if (status != INSTALL_READY) return status;
  if (r != FR_OK) return problem(INSTALL_MISSING, INSTALL_MANIFEST, what);
  uint32_t sizes[INSTALL_OUTPUTS];
  if (!install_read_manifest(folder, sizes)) return problem(INSTALL_OTHER_FIRMWARE, INSTALL_MANIFEST, what);
  for (uint32_t k = 0; k < INSTALL_OUTPUTS; k++) {
    if (!install_needed(k, path)) continue;
    char file[INSTALL_PATH_BYTES];
    install_output_name(k, name);
    install_path_of(file, folder, name);
    r = f_stat(file, &s_info);
    if (card_error(r)) return INSTALL_NO_CARD;
    if (r != FR_OK) return problem(INSTALL_MISSING, name, what);
    if (sizes[k] == 0 || (uint32_t)s_info.fsize != sizes[k]) return problem(INSTALL_INCOMPLETE, name, what);
  }
  return INSTALL_READY;
}
