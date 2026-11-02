/* The runtime interface the translated program is written against: the
 * 68000's registers and flags, a flat 24-bit big-endian memory, and the
 * calls the program makes outside itself (system calls, the hardware the
 * translator hooks). Part of tools/xlat, the translator: generated C includes
 * this header; a runtime (the host's, the RP's) implements the externs. */
#ifndef XL_H
#define XL_H

#include <stdint.h>

#define XL_MEMORY (1u << 24)

typedef struct {
  uint32_t d[8], a[8];
  uint32_t n, z, v, c, x; /* flags, 0 or 1 */
  uint32_t sr_high;        /* the system byte of SR (the interrupt mask) */
} xl_cpu_t;

extern xl_cpu_t xl;
extern uint8_t *xl_mem; /* XL_MEMORY bytes */

#define D(n) xl.d[n]
#define A(n) xl.a[n]

/* A map of the memory the program touches (the host's XL_MEMMAP build):
 * the runtime counts every access by page. */
#ifdef XL_MEMMAP
void xl_memmap_access(uint32_t a, int size, int write);
#define XL_TOUCH(a, size, write) xl_memmap_access(a, size, write)
#else
#define XL_TOUCH(a, size, write) ((void)0)
#endif

/* Where an address's byte is. The host: one flat big-endian array. The RP
 * (XL_PAGED): pages of 256 bytes, each in RAM or in flash, through a table
 * of 256 regions of 64 KB, each a row of its pages (tools/xlat/rp_image.py
 * builds it from a snapshot and a memory map; xl_store.h's
 * XL_STORE_PAGE_BITS). A long that straddles two pages is read and written
 * as two words. */
#ifdef XL_PAGED
#define XL_PAGE_BITS 8
#define XL_PAGE_MASK ((1u << XL_PAGE_BITS) - 1u)
extern uint8_t **xl_regions[256];
/* At -Os the compiler calls the accessors instead of inlining them: less
 * code, fewer misses of the flash's cache, faster (measured on the RP:
 * 1.7 ms a frame against 2.9 ms forced inline). XL_FORCE_INLINE to measure
 * the other way. */
#ifdef XL_FORCE_INLINE
#define XL_INLINE static inline __attribute__((always_inline))
#else
#define XL_INLINE static inline
#endif
/* The lookup is two instructions: always inlined, into the accessors. */
static inline __attribute__((always_inline)) uint8_t *xl_at(uint32_t a) {
  return xl_regions[(a >> 16) & 0xFFu][(a >> XL_PAGE_BITS) & ((1u << (16 - XL_PAGE_BITS)) - 1u)] +
         (a & XL_PAGE_MASK);
}
#define XL_STRADDLES(a) (((a) & XL_PAGE_MASK) > XL_PAGE_MASK - 3u)
#else
#define XL_INLINE static inline
XL_INLINE uint8_t *xl_at(uint32_t a) { return xl_mem + (a & 0xFFFFFFu); }
#define XL_STRADDLES(a) 0
#endif

#ifdef XL_PAGED
/* The RP keeps each 16-bit word in its own byte order (the pages are
 * swapped when rp_image.py packs them): a word is one halfword access, a
 * byte is at its address ^ 1, a long two words. The 68000 accesses words and
 * longs at even addresses only. */
XL_INLINE uint32_t rd8(uint32_t a) { return *xl_at(a ^ 1u); }
XL_INLINE uint32_t rd16(uint32_t a) { return *(const uint16_t *)xl_at(a); }
XL_INLINE uint32_t rd32(uint32_t a) {
  if (XL_STRADDLES(a)) return (rd16(a) << 16) | rd16(a + 2u);
  const uint16_t *p = (const uint16_t *)xl_at(a);
  return ((uint32_t)p[0] << 16) | p[1];
}
XL_INLINE void wr8(uint32_t a, uint32_t v) { *xl_at(a ^ 1u) = (uint8_t)v; }
XL_INLINE void wr16(uint32_t a, uint32_t v) { *(uint16_t *)xl_at(a) = (uint16_t)v; }
XL_INLINE void wr32(uint32_t a, uint32_t v) {
  if (XL_STRADDLES(a)) {
    wr16(a, v >> 16);
    wr16(a + 2u, v);
    return;
  }
  uint16_t *p = (uint16_t *)xl_at(a);
  p[0] = (uint16_t)(v >> 16);
  p[1] = (uint16_t)v;
}
#else
XL_INLINE uint32_t rd8(uint32_t a) {
  XL_TOUCH(a, 1, 0);
  return *xl_at(a);
}
XL_INLINE uint32_t rd16(uint32_t a) {
  XL_TOUCH(a, 2, 0);
  const uint8_t *p = xl_at(a);
  return ((uint32_t)p[0] << 8) | p[1];
}
XL_INLINE uint32_t rd32(uint32_t a) {
  XL_TOUCH(a, 4, 0);
  const uint8_t *p = xl_at(a);
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
XL_INLINE void wr8(uint32_t a, uint32_t v) {
  XL_TOUCH(a, 1, 1);
  *xl_at(a) = (uint8_t)v;
}
XL_INLINE void wr16(uint32_t a, uint32_t v) {
  XL_TOUCH(a, 2, 1);
  uint8_t *p = xl_at(a);
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
XL_INLINE void wr32(uint32_t a, uint32_t v) {
  XL_TOUCH(a, 4, 1);
  uint8_t *p = xl_at(a);
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
#endif

/* Hardware the translator hooks: reads and writes at absolute addresses the
 * runtime must see (the VBL's GPIP bit, the joysticks, ...). */
uint32_t xl_hw_rd(uint32_t a, int size);
void xl_hw_wr(uint32_t a, uint32_t v, int size);

/* System calls: TRAP #n, and the line-F words ($FFxx DOS, $FExx FLOAT). */
void xl_trap(uint32_t n, uint32_t pc);
void xl_linef(uint32_t word, uint32_t pc);
/* SR as the program reads it, and written. */
uint32_t xl_get_sr(void);
void xl_set_sr(uint32_t v);
void xl_stop(uint32_t sr, uint32_t pc);

/* The runtime's code in place of the program's at the patched addresses
 * (xlat.py PATCHES, PATCHES_BEFORE). */
void xl_patch_xsp_vsync(void);
void xl_patch_xsp_out(void);
void xl_patch_clear_text_planes(void);
void xl_patch_wait_vbl(void);
void xl_patch_sincos(void);
void xl_patch_samples_loaded(void);
/* Live input in the attract demo (xlat.py PATCHES_BEFORE): whether the
 * player has taken over the demo's replay, and whether a read of the stick
 * on the player's behalf is in progress (the runtime's _JOYGET answers it
 * with the live stick, every other read with nothing pressed). */
int xl_live_input(void);
void xl_live_read(int on);
/* Whether the game records the player's game over the demo's replay (the
 * buffer the attract demo plays, 128 KB): the RP never does (it keeps the
 * buffer in flash); the host when it saves a recording (XL_SAVE_REPLAY_AT). */
int xl_record(void);

/* Enter the code at a 68000 address: the translated function that starts
 * there; returns the address its RTS (or RTE) popped. Unknown: fatal. */
uint32_t xl_call(uint32_t address);
/* The same, for a function of the stage in the store's slot (the common
 * image's stubs: tools/xlat/xl_store.h). */
uint32_t xl_call_stage(uint32_t address);
void xl_fatal(const char *what, uint32_t address);
void xl_divide_by_zero(uint32_t pc);

/* A trace of entries and returns, for finding where a translation goes
 * wrong: the runtime defines XL_TRACE and the two functions. */
#ifdef XL_TRACE
void xl_trace_enter(uint32_t entry);
void xl_trace_return(uint32_t at, uint32_t to);
#define XL_ENTER(e) xl_trace_enter(e)
#define XL_RETURN(at, to) xl_trace_return(at, to)
#else
#define XL_ENTER(e) ((void)0)
#define XL_RETURN(at, to) ((void)0)
#endif

/* The size masks and sign bits. */
#define M8 0xFFu
#define M16 0xFFFFu
#define M32 0xFFFFFFFFu

XL_INLINE uint32_t sx8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
XL_INLINE uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

/* Shifts and rotates: the result, with every flag set as the 68000 does. */
static inline uint32_t xl_shift(int kind, int left, uint32_t v, uint32_t count, int bits) {
  /* kind: 0 as, 1 ls, 2 rox, 3 ro */
  uint32_t m = bits == 32 ? M32 : ((1u << bits) - 1u);
  uint32_t sign = 1u << (bits - 1);
  count &= 63u;
  v &= m;
  xl.v = 0;
  if (count == 0) {
    xl.c = kind == 2 ? xl.x : 0;
  } else if (kind == 0 || kind == 1) {
    if (left) {
      uint32_t r = v;
      uint32_t vchg = 0;
      for (uint32_t i = 0; i < count; i++) {
        uint32_t out = (r & sign) ? 1u : 0u;
        r = (r << 1) & m;
        xl.c = out;
        if (kind == 0 && ((r & sign) ? 1u : 0u) != out) vchg = 1;
      }
      xl.x = xl.c;
      if (kind == 0) {
        /* V: the sign changed at any point during the shift */
        xl.v = vchg;
      }
      v = r;
    } else {
      uint32_t r = v;
      for (uint32_t i = 0; i < count; i++) {
        xl.c = r & 1u;
        if (kind == 0) {
          r = (r >> 1) | (r & sign);
        } else {
          r >>= 1;
        }
      }
      xl.x = xl.c;
      v = r & m;
    }
  } else if (kind == 3) {
    uint32_t n = count % (uint32_t)bits;
    if (left) {
      v = n ? (((v << n) | (v >> (bits - n))) & m) : v;
      xl.c = v & 1u;
    } else {
      v = n ? (((v >> n) | (v << (bits - n))) & m) : v;
      xl.c = (v & sign) ? 1u : 0u;
    }
  } else { /* rox: through X */
    for (uint32_t i = 0; i < count; i++) {
      if (left) {
        uint32_t out = (v & sign) ? 1u : 0u;
        v = ((v << 1) | xl.x) & m;
        xl.x = out;
      } else {
        uint32_t out = v & 1u;
        v = (v >> 1) | (xl.x ? sign : 0u);
        xl.x = out;
      }
    }
    xl.c = xl.x;
  }
  xl.n = (v & sign) ? 1u : 0u;
  xl.z = v == 0;
  return v;
}

#endif /* XL_H */
