// units: rp/src/zipread.c rp/src/crc32.c third_party/miniz/miniz_tinfl.c
// cflags: -I ../../third_party/miniz -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME
/* The zip reader (zipread.c) against build/zip_fixture.zip (zip_fixture.py):
 * every member found below the top folder and extracted byte for byte,
 * deflated and stored, one longer than the 32 KB window; a missing path; a
 * damaged member caught by its CRC; a short archive refused. With
 * CHORENSHA_ZIP set to the repository's zip, its members the gate reads are
 * extracted and checked too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "zipread.h"

typedef struct {
  uint8_t *data;
  uint32_t size;
  FILE *file;
} archive_t;

static int32_t read_memory(void *ctx, uint32_t offset, void *buffer, uint32_t length) {
  archive_t *a = ctx;
  if (offset >= a->size) return 0;
  if (length > a->size - offset) length = a->size - offset;
  memcpy(buffer, a->data + offset, length);
  return (int32_t)length;
}

static int32_t read_file(void *ctx, uint32_t offset, void *buffer, uint32_t length) {
  archive_t *a = ctx;
  if (fseek(a->file, (long)offset, SEEK_SET)) return -1;
  return (int32_t)fread(buffer, 1, length, a->file);
}

static uint8_t *load(const char *path, uint32_t *size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc(n ? (size_t)n : 1u);
  *size = (uint32_t)fread(data, 1, (size_t)n, f);
  fclose(f);
  return data;
}

/* The pieces zip_extract hands out, gathered. */
typedef struct {
  uint8_t *data;
  uint32_t size, capacity, pieces, largest;
} gathered_t;

static int gather(void *arg, const uint8_t *data, uint32_t length) {
  gathered_t *g = arg;
  if (g->size + length > g->capacity) {
    g->capacity = (g->size + length) * 2u;
    g->data = realloc(g->data, g->capacity);
  }
  memcpy(g->data + g->size, data, length);
  g->size += length;
  g->pieces++;
  if (length > g->largest) g->largest = length;
  return 0;
}

static int count_only(void *arg, const uint8_t *data, uint32_t length) {
  (void)data;
  *(uint32_t *)arg += length;
  return 0;
}

static int stop_at_once(void *arg, const uint8_t *data, uint32_t length) {
  (void)arg, (void)data, (void)length;
  return 1;
}

static uint32_t s_work[ZIP_WORK_BYTES / 4];

static void check_member(const zip_t *zip, const char *path) {
  char expected_path[256];
  snprintf(expected_path, sizeof expected_path, "build/zip_fixture/%s", path);
  for (char *p = expected_path + strlen("build/zip_fixture/"); *p; p++)
    if (*p == '/') *p = '_';
  uint32_t expected_size = 0;
  uint8_t *expected = load(expected_path, &expected_size);
  CHECK(expected != NULL);
  zip_member_t m;
  CHECK_EQ(zip_find(zip, path, &m), ZIP_OK);
  CHECK_EQ(m.size, expected_size);
  gathered_t g = {0};
  CHECK_EQ(zip_extract(zip, &m, s_work, gather, &g), ZIP_OK);
  CHECK_EQ(g.size, expected_size);
  CHECK(g.largest <= 32768u);
  CHECK(expected_size == 0 || (g.data && memcmp(g.data, expected, expected_size) == 0));
  free(g.data);
  free(expected);
}

/* The gate's members in the repository's zip, extracted and checked. */
static void check_repository(const char *path) {
  archive_t a = {0};
  a.file = fopen(path, "rb");
  if (a.file == NULL) {
    printf("CHORENSHA_ZIP: %s not found, skipped\n", path);
    return;
  }
  fseek(a.file, 0, SEEK_END);
  uint32_t bytes = (uint32_t)ftell(a.file);
  zip_t zip;
  CHECK_EQ(zip_open(&zip, read_file, &a, bytes), ZIP_OK);
  static const char *const members[] = {
      "binaries/x68000/sz2.hex", "sources/sincos.dat", "binaries/amiga/MUSIC_DAT/SZ2_S1.crm",
      "binaries/amiga/MUSIC_DAT/SZ2_TMP.crm", "binaries/amiga/PCM_DAT/SZ2_PCM.CNF"};
  for (size_t k = 0; k < sizeof members / sizeof *members; k++) {
    zip_member_t m;
    int found = zip_find(&zip, members[k], &m);
    CHECK_EQ(found, ZIP_OK);
    if (found != ZIP_OK) continue;
    uint32_t got = 0;
    clock_t t = clock();
    CHECK_EQ(zip_extract(&zip, &m, s_work, count_only, &got), ZIP_OK);
    CHECK_EQ(got, m.size);
    printf("  %s: %u bytes (%u packed) in %.0f ms\n", members[k], m.size, m.packed,
           1000.0 * (double)(clock() - t) / CLOCKS_PER_SEC);
  }
  fclose(a.file);
}

int main(void) {
  archive_t a = {0};
  a.data = load("build/zip_fixture.zip", &a.size);
  CHECK(a.data != NULL);
  if (a.data == NULL) TEST_END();

  zip_t zip;
  CHECK_EQ(zip_open(&zip, read_memory, &a, a.size), ZIP_OK);
  CHECK_EQ(zip.members, 5u);
  check_member(&zip, "binaries/x68000/sz2.hex");
  check_member(&zip, "binaries/amiga/MUSIC_DAT/SONG.crm");
  check_member(&zip, "sources/sincos.dat");
  check_member(&zip, "empty.txt");

  zip_member_t m;
  CHECK_EQ(zip_find(&zip, "binaries/x68000/SZ2.HEX", &m), ZIP_ERR_MISSING);
  CHECK_EQ(zip_find(&zip, "sz2.hex", &m), ZIP_ERR_MISSING);

  /* The caller stops; a damaged byte inside a member's data is caught (by
   * the inflater or by the CRC). */
  CHECK_EQ(zip_find(&zip, "binaries/amiga/MUSIC_DAT/SONG.crm", &m), ZIP_OK);
  CHECK_EQ(zip_extract(&zip, &m, s_work, stop_at_once, NULL), ZIP_ERR_STOPPED);
  uint8_t *damaged = malloc(a.size);
  memcpy(damaged, a.data, a.size);
  archive_t d = {damaged, a.size, NULL};
  zip_t dz;
  CHECK_EQ(zip_open(&dz, read_memory, &d, d.size), ZIP_OK);
  CHECK_EQ(zip_find(&dz, "sources/sincos.dat", &m), ZIP_OK);
  damaged[m.header + 30u + 37u + 100u] ^= 0x55u; /* local header, name "repo-1234/sources/sincos.dat" */
  uint32_t got = 0;
  CHECK_EQ(zip_extract(&dz, &m, s_work, count_only, &got), ZIP_ERR_CHECK);
  CHECK_EQ(zip_find(&dz, "binaries/amiga/MUSIC_DAT/SONG.crm", &m), ZIP_OK);
  damaged[m.header + 30u + 47u + m.packed / 2u] ^= 0xFFu;
  int r = zip_extract(&dz, &m, s_work, count_only, &got);
  CHECK(r == ZIP_ERR_DATA || r == ZIP_ERR_CHECK);
  free(damaged);

  /* Not a zip: too short, and no end record. */
  archive_t s = {a.data, 10, NULL};
  CHECK_EQ(zip_open(&zip, read_memory, &s, s.size), ZIP_ERR_FORMAT);
  archive_t n = {a.data, 1000, NULL};
  CHECK_EQ(zip_open(&zip, read_memory, &n, n.size), ZIP_ERR_FORMAT);
  free(a.data);

  const char *repository = getenv("CHORENSHA_ZIP");
  if (repository) check_repository(repository);
  TEST_END();
}
