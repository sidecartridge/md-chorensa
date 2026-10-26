/* The translated game in the RP's flash store: where its images go and the
 * header each starts with. The firmware carries no game code: the images
 * are made on the host from the user's copy (tools/xlat/rp_build.py), kept
 * on the SD card, and copied into the store when the store does not hold
 * them already (rp/src/store.c). Each is linked at its region's address
 * against one firmware build, whose build ID it names.
 *
 * The store: the common code (written once) and the slot of the stage
 * being played (rewritten when another stage starts). */
#ifndef XL_STORE_H
#define XL_STORE_H

#include <stdint.h>

#define XL_STORE_BASE 0x10060000u        /* after the firmware's 384 KB */
#define XL_STORE_COMMON XL_STORE_BASE    /* the common code */
#define XL_STORE_COMMON_SIZE 0x60000u    /* 384 KB */
#define XL_STORE_STAGE 0x100C0000u       /* the stage being played */
#define XL_STORE_STAGE_SIZE 0x60000u     /* 384 KB, up to the Booster's area */

#define XL_STORE_MAGIC 0x58535432u   /* 'XST2' */
#define XL_STORE_VERSION 1u
#define XL_STORE_KIND_COMMON 0u      /* else the stage's number */
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

/* The first bytes of an image, at its region's address. The loader programs
 * the page that holds it last, so a store cut short has no valid magic. */
typedef struct {
  uint32_t magic;   /* XL_STORE_MAGIC */
  uint32_t version; /* XL_STORE_VERSION */
  uint32_t kind;    /* XL_STORE_KIND_COMMON or the stage */
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
  const uint8_t *ram_init;            /* the written pages' contents */
  uint32_t ram_pages;                 /* how many (1 KB each) */
  uint8_t *const *const *regions;     /* 256 rows of 64 page pointers */
  const uint8_t *checkpoints;         /* the host's, 32 bytes each */
  uint32_t checkpoint_count;
  uint32_t layout[XL_LAYOUT_WORDS]; /* the common image's: XL_LAYOUT_* */
} xl_store_header_t;

#endif /* XL_STORE_H */
