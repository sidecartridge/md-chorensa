/* The translated game in the RP's flash store: where its images go and the
 * header each starts with. The firmware carries no game code: the images
 * are made on the host from the user's copy (tools/xlat/rp_store.py), kept
 * on the SD card, and copied into the store when the store does not hold
 * them already (rp/src/store.c). Each is linked at its region's address
 * against one firmware build, whose build ID it names.
 *
 * The store: COMMON (written once: the code, the read-only data and the
 * sprite patterns more than one scene uses) and the slot of the scene being
 * played, rewritten when another starts: a stage (STAGE1..7) or the menus
 * (MENUS: the title, CONFIG, KAKUSI), each with its own code, data,
 * patterns, background and page table. */
#ifndef XL_STORE_H
#define XL_STORE_H

#include <stdint.h>

#define XL_STORE_BASE 0x10040000u        /* after the firmware's 256 KB */
#define XL_STORE_COMMON XL_STORE_BASE    /* the common image */
#define XL_STORE_COMMON_SIZE 0x90000u    /* 576 KB */
#define XL_STORE_SLOT 0x100D0000u        /* the scene being played */
#define XL_STORE_SLOT_SIZE 0x50000u      /* 320 KB, up to the Booster's area */

/* The game's memory on the RP: pages of 256 bytes (xl.h's XL_PAGE_BITS),
 * the ones it writes in RAM, the others in the images. The RAM pages
 * fill the chunky buffer's part a frame drawn in bands leaves first
 * (fb_chunked.h's FB_BANDED_BYTES on: the game draws only in bands while it
 * plays), then the firmware's array (xl_rp_ram), then the cartridge
 * window's free arena (APP_FREE, which neither the ST nor the firmware
 * uses). */
#define XL_STORE_PAGE_BITS 8u
#define XL_STORE_RAM_BYTES 0xC000u            /* 48 KB: 192 pages */
#define XL_STORE_WINDOW_FREE 0x5180u          /* CART_APP_FREE_OFFSET */
#define XL_STORE_WINDOW_FREE_BYTES 0x2E00u    /* 46 pages, below the framebuffer */
#define XL_STORE_FB_FREE 0x9000u              /* FB_BANDED_BYTES */
#define XL_STORE_FB_FREE_BYTES 0x7000u        /* 112 pages, to the buffer's end */

/* The text plane (four planes of 1024 lines, 128 bytes a line, at $E00000)
 * in a layout of its own: the game shows it unscrolled, writes only its
 * first 32 bytes a line (the screen's 256 pixels) and never reads it, so the
 * runtime keeps the screen's lines' first 32 bytes only. Each page (two
 * lines) points into a packed area: four pages to 256 bytes, page j of a
 * plane at 32 * (j % 4) + 256 * (j / 4), its first line's 32 bytes there and
 * its second line's 128 bytes on, so the pages' bytes past 32 a line overlap
 * the next pages' (never written). The pages of the lines past the screen
 * point at the spare page; the last page's overlap runs 96 bytes past the
 * last plane (XL_STORE_TEXT_BYTES keeps them, so a stray write stays in it). */
#define XL_STORE_TEXT_LINES 0x100u          /* the screen's lines */
#define XL_STORE_TEXT_PLANE_BYTES 0x2000u   /* 256 lines of 32 bytes */
#define XL_STORE_TEXT_BYTES 0x8060u         /* four planes and the overlap */

#define XL_STORE_MAGIC 0x58535432u   /* 'XST2' */
#define XL_STORE_VERSION 7u          /* 6: the scenes' slot; 7: the scene change */
#define XL_STORE_KIND_COMMON 0u      /* 1..7 a stage's */
#define XL_STORE_KIND_MENUS 8u
#define XL_STORE_FIRMWARE_ID 32u     /* bytes of the build ID kept */

/* The program's variables the runtime reads and writes (the listing's
 * names, in xl_layout.h), by index in the common image's header. */
enum {
  XL_LAYOUT_SPRITE_DATA_TABLE,
  XL_LAYOUT_CURRENT_SPRITE_DATA_ENTRY,
  XL_LAYOUT_VBL_VWAIT_COUNTER,
  XL_LAYOUT_PLAYER_SCORE,
  XL_LAYOUT_WORD_00088E6C,
  XL_LAYOUT_BACKGROUND_SCROLL_COUNTER,
  XL_LAYOUT_PLAYER_INFO_STRUCT,
  XL_LAYOUT_SIN_COS_TABLES_START,
  XL_LAYOUT_WORDS
};

typedef uint32_t (*xl_store_fn_t)(void);

typedef struct {
  uint32_t address; /* the 68000 address the function starts at */
  xl_store_fn_t fn;
} xl_store_entry_t;

/* A page of a scene kept in RAM and its slot of the pool (xl_store.h's
 * pool, in rp_image.py's order). Each page has one slot in every scene; two
 * pages share a slot only when no scene keeps both. */
typedef struct {
  uint16_t page; /* the address >> XL_STORE_PAGE_BITS */
  uint16_t slot;
} xl_store_ram_page_t;

/* A function the slot's scene holds, and which scene (its kind). */
typedef struct {
  uint32_t address;
  uint32_t kind;
} xl_store_scene_entry_t;

/* A song the game has loaded (MCDRV names it by its data's address) and the
 * file on the card that plays it (NAME.ST8, tools/xlat/music_convert.py). */
typedef struct {
  uint32_t address;
  char name[12]; /* "SZ2_S1": the song's base name */
} xl_store_song_t;

#define XL_STORE_COMMON_PATTERN 0x8000u
#define XL_STORE_NO_PATTERN 0xFFFFu

/* A scene's pictures for the renderer, in the ST's 16 colours as the follow
 * scheme maps the program's palette entries (tools/xlat/palette_fit.py):
 * the two graphics pages and the sprite patterns converted once, and the
 * palette worked out each frame from the entries' current colours. */
typedef struct {
  const uint8_t *page1;          /* the far layer: 4-bit, 128 bytes a line, opaque
                                    (the backdrop where the page is clear) */
  const uint8_t *page0_runs;     /* the near layer's opaque runs: per line their
                                    number n, n pairs (x, length - 1), then each
                                    run's pixels, 4-bit, from a byte boundary */
  const uint32_t *page0_index;   /* each line's record in page0_runs */
  uint32_t page_lines;           /* both pages' lines (512) */
  const uint8_t *patterns;       /* 16 x 16, 4-bit, 8 bytes a row, the left pixel
                                    in the high nibble; the program's colour numbers */
  const uint8_t *common_patterns; /* the same, in COMMON: the ones more than one
                                     scene draws */
  const uint16_t *pattern_index; /* per program pattern: its place in patterns, or
                                    XL_STORE_COMMON_PATTERN + its place in
                                    common_patterns, or XL_STORE_NO_PATTERN */
  uint32_t pattern_count;        /* entries of pattern_index */
  const uint8_t *entry_colour;   /* the 512 palette entries' ST colour numbers
                                    (0-255 graphics, 256 + n sprite and text) */
  const uint8_t *entry_weight;   /* their weights in a colour's mean, 0 unused */
  const uint16_t *linear;        /* 256: an 8-bit level in linear light (0-4095) */
  const uint16_t *thresholds;    /* 7: between the ST's eight levels, in linear light */
} xl_store_art_t;

/* The first bytes of an image, at its region's address. The loader programs
 * the page that holds it last, so a store cut short has no valid magic. */
typedef struct {
  uint32_t magic;   /* XL_STORE_MAGIC */
  uint32_t version; /* XL_STORE_VERSION */
  uint32_t kind;    /* XL_STORE_KIND_COMMON, a stage's number or XL_STORE_KIND_MENUS */
  uint32_t size;    /* bytes of the image, this header included */
  uint32_t crc;     /* CRC-32 of the bytes after the header */
  char firmware[XL_STORE_FIRMWARE_ID]; /* the build ID it links against */
  const xl_store_entry_t *entries;     /* by address, ascending */
  uint32_t entry_count;
  /* A stage image made for the logic bench: the snapshot it resumes from
   * (tools/xlat/rp_image.py), or zeros. */
  uint32_t resume;                    /* where the run resumes */
  const uint32_t *state;              /* the runtime's state words */
  const uint8_t *sprites;             /* the sprite list */
  const uint8_t *ram_init;            /* the RAM pages' contents, in ram_list's order */
  /* A scene's pages in RAM and their slots. The first entry_pages are the
   * ones whose slot some other scene's page takes: entering the scene, each
   * whose slot changed hands since gets entry_init's content (the game's
   * memory after its boot), the others keep theirs. */
  const xl_store_ram_page_t *ram_list;
  uint32_t ram_count;
  uint8_t *const *const *regions;     /* 256 rows of 256 page pointers */
  const uint8_t *checkpoints;         /* the host's, 32 bytes each */
  uint32_t checkpoint_count;
  uint32_t layout[XL_LAYOUT_WORDS]; /* the common image's: XL_LAYOUT_* */
  const xl_store_art_t *art;          /* a scene's pictures, or 0 */
  const xl_store_song_t *songs;       /* the common image's: the songs loaded */
  uint32_t song_count;
  uint32_t song_playing;              /* a stage's snapshot: the song playing (in the
                                         common image's table), or 0xFFFFFFFF */
  uint32_t song_frames;               /* the frames it had played then */
  /* A stage's snapshot: the text plane's rows (32 bytes, a line's shown
   * part) that are not clear, the others clear. */
  const uint16_t *text_rows;          /* each row's offset in the packed area */
  const uint8_t *text_init;           /* their bytes, 32 a row */
  uint32_t text_row_count;
  uint32_t entry_pages;               /* a scene's: see ram_list */
  const uint8_t *entry_init;
  /* The common image's: which scene holds each function of the slot's, by
   * address, ascending (the scene change loads it). */
  const xl_store_scene_entry_t *scenes;
  uint32_t scene_count;
} xl_store_header_t;

#endif /* XL_STORE_H */
