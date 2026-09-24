// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `peanutgb`: the Peanut-GB Game Boy core (deltabeard, MIT) as a
// CircuitPython native .mpy, built turbo-plus style with arm-none-eabi-gcc.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(peanutgb.STATE_SIZE)          # core + context, keep alive
//   peanutgb.init(st, rom)                       # rom: bytes/bytearray, keep alive
//   peanutgb.set_cart_ram(st, bytearray(peanutgb.save_size(st)))  # if non-zero
//   peanutgb.set_output(st, fb, width, bpp, scale, x0, y0)        # optional
//   peanutgb.set_joypad(st, mask)                # 1 = pressed, see JOYPAD_* below
//   peanutgb.run_frames(st, n)                   # -> frames run, stops on core error
//   peanutgb.frame_crc(st)                       # CRC-32 of the last 160x144 shades
//   peanutgb.set_audio(st, ring, start)          # optional, see below
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565.
//
// Sound: minigb_apu (Alex Baines, Mahyar Koshkouei, MIT) at AUDIO_RATE. With a
// ring set, every emulated frame appends AUDIO_SAMPLES_PER_FRAME stereo int16
// frames to it, wrapping. Make the ring the buffer of a looping, double
// buffered audiocore.RawSample and start writing one half ahead of playback.

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#else
#include "py/dynruntime.h"

// dynruntime.h does not map these; Peanut-GB and GCC's struct copies need them.
void *memset(void *s, int c, size_t n) {
    return mp_fun_table.memset_(s, c, n);
}
void *memmove(void *dest, const void *src, size_t n) {
    return mp_fun_table.memmove_(dest, src, n);
}
void *memcpy(void *dest, const void *src, size_t n) {
    return mp_fun_table.memmove_(dest, src, n);
}
#endif

#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#define AUDIO_SAMPLE_RATE 22050
#include "minigb_apu.h"
// minigb_apu.h derives this through double math; use the integer value so it is
// a constant expression (array sizes) and no soft-double code is emitted.
#undef AUDIO_SAMPLES
#define AUDIO_SAMPLES ((unsigned)(AUDIO_SAMPLE_RATE * 70224u / 4194304u))

// Peanut-GB calls audio_read()/audio_write() as free functions from inside
// __gb_read/__gb_write, where `gb` is in scope. Route them to the APU context
// kept in our state buffer (defined below, after ctx_t).
struct gb_s;
static uint8_t pgb_audio_read(struct gb_s *gb, uint16_t addr);
static void pgb_audio_write(struct gb_s *gb, uint16_t addr, uint8_t val);
#define audio_read(a) pgb_audio_read(gb, (a))
#define audio_write(a, v) pgb_audio_write(gb, (a), (v))
#ifdef PGB_GBC
// Game Boy Color: tvecera's gbc-rtc-fix Peanut-GB (MIT), as vendored by
// pico-peanutGB. DMG carts still run in DMG mode. Strip its pico-sdk RAM hint.
#include <stdbool.h>
#define __not_in_flash_func(f) f
#define PEANUT_FULL_GBC_SUPPORT 1
// pico-peanutGB defines these outside the core (gb.h, mytypes.h); same values.
#define RGB555_TO_RGB444(c) (((((c) >> 10) & 0x1F) >> 1) << 8 | ((((c) >> 5) & 0x1F) >> 1) << 4 | (((c) & 0x1F) >> 1))
#define JOYPAD_A 0x01
#define JOYPAD_B 0x02
#define JOYPAD_SELECT 0x04
#define JOYPAD_START 0x08
#define JOYPAD_RIGHT 0x10
#define JOYPAD_LEFT 0x20
#define JOYPAD_UP 0x40
#define JOYPAD_DOWN 0x80
// The fork trips warnings that the natmod build treats as errors.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsequence-point"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "peanut_gb_cgb.h"
#pragma GCC diagnostic pop
#else
#include "peanut_gb.h"
#endif
#undef MIN  // peanut_gb.h / py/misc.h and minigb_apu.c all define these
#undef MAX
#include "minigb_apu.c"

#define GB_W 160
#define GB_H 144

typedef struct {
    struct gb_s gb;
    const uint8_t *rom;
    uint32_t rom_len;
    uint8_t *cart_ram;
    uint32_t cart_len;
    uint8_t *fb;
    uint32_t fb_len;
    uint32_t fb_width;
    uint32_t bpp;
    uint32_t scale;
    uint32_t x0;
    uint32_t y0;
    uint32_t err;
    uint32_t err_addr;
    uint8_t pal8[4];
    uint16_t pal16[4];
    int16_t *ring;
    uint32_t ring_frames;
    uint32_t ring_pos;
    struct minigb_apu_ctx apu;
    int16_t frame_audio[AUDIO_SAMPLES_TOTAL];
    uint8_t shade[GB_H * GB_W];
} ctx_t;

static uint8_t pgb_audio_read(struct gb_s *gb, uint16_t addr) {
    return minigb_apu_audio_read(&((ctx_t *)gb->direct.priv)->apu, addr);
}

static void pgb_audio_write(struct gb_s *gb, uint16_t addr, uint8_t val) {
    minigb_apu_audio_write(&((ctx_t *)gb->direct.priv)->apu, addr, val);
}

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    ctx_t *ctx = gb->direct.priv;
    return addr < ctx->rom_len ? ctx->rom[addr] : 0xFF;
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    ctx_t *ctx = gb->direct.priv;
    return addr < ctx->cart_len ? ctx->cart_ram[addr] : 0xFF;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    ctx_t *ctx = gb->direct.priv;
    if (addr < ctx->cart_len) {
        ctx->cart_ram[addr] = val;
    }
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    ctx_t *ctx = gb->direct.priv;
    // Peanut-GB keeps running after calling this; run_frames() checks err
    // after every frame and stops.
    ctx->err = (uint32_t)err + 1;
    ctx->err_addr = addr;
}

// Peanut-GB hands over one finished scanline. The low two bits of each pixel
// are the palette-mapped shade, 0 lightest.
#ifdef PGB_GBC
// CGB mode: pixels index gb->cgb.fixPalette, already RGB555 (r<<10|g<<5|b).
static void draw_line_cgb(ctx_t *ctx, struct gb_s *gb, const uint8_t *pixels, uint_fast8_t line) {
    uint8_t *row = &ctx->shade[line * GB_W];
    for (unsigned x = 0; x < GB_W; x++) {
        row[x] = pixels[x];
    }
    if (ctx->fb == NULL) {
        return;
    }
    const uint16_t *pal = gb->cgb.fixPalette;
    const uint32_t s = ctx->scale;
    const uint32_t w = ctx->fb_width;
    const uint32_t y = ctx->y0 + line * s;
    if (ctx->bpp == 8) {
        uint8_t *dst = ctx->fb + y * w + ctx->x0;
        for (unsigned x = 0; x < GB_W; x++) {
            uint16_t c = pal[row[x]];
            uint8_t c8 = (uint8_t)((((c >> 12) & 7) << 5) | (((c >> 7) & 7) << 2) | ((c >> 3) & 3));
            for (unsigned k = 0; k < s; k++) {
                *dst++ = c8;
            }
        }
        uint8_t *first = ctx->fb + y * w + ctx->x0;
        for (unsigned k = 1; k < s; k++) {
            memcpy(first + k * w, first, GB_W * s);
        }
    } else {
        uint16_t *fb16 = (uint16_t *)ctx->fb;
        uint16_t *dst = fb16 + y * w + ctx->x0;
        for (unsigned x = 0; x < GB_W; x++) {
            uint16_t c = pal[row[x]];
            uint16_t c16 = (uint16_t)(((c & 0x7C00) << 1) | ((c & 0x03E0) << 1) | ((c >> 4) & 0x20) | (c & 0x001F));
            for (unsigned k = 0; k < s; k++) {
                *dst++ = c16;
            }
        }
        uint16_t *first = fb16 + y * w + ctx->x0;
        for (unsigned k = 1; k < s; k++) {
            memcpy(first + k * w, first, GB_W * s * 2);
        }
    }
}
#endif

static void draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    ctx_t *ctx = gb->direct.priv;
#ifdef PGB_GBC
    if (gb->cgb.cgbMode) {
        draw_line_cgb(ctx, gb, pixels, line);
        return;
    }
#endif
    uint8_t *row = &ctx->shade[line * GB_W];
    for (unsigned x = 0; x < GB_W; x++) {
        row[x] = pixels[x] & 3;
    }
    if (ctx->fb == NULL) {
        return;
    }
    const uint32_t s = ctx->scale;
    const uint32_t w = ctx->fb_width;
    const uint32_t y = ctx->y0 + line * s;
    if (ctx->bpp == 8) {
        uint8_t *dst = ctx->fb + y * w + ctx->x0;
        if (s == 3) {
            for (unsigned x = 0; x < GB_W; x++) {
                uint8_t c = ctx->pal8[row[x]];
                dst[0] = c;
                dst[1] = c;
                dst[2] = c;
                dst += 3;
            }
        } else {
            for (unsigned x = 0; x < GB_W; x++) {
                uint8_t c = ctx->pal8[row[x]];
                for (unsigned k = 0; k < s; k++) {
                    *dst++ = c;
                }
            }
        }
        // Duplicate the finished row for vertical scaling.
        uint8_t *first = ctx->fb + y * w + ctx->x0;
        for (unsigned k = 1; k < s; k++) {
            memcpy(first + k * w, first, GB_W * s);
        }
    } else {
        uint16_t *fb16 = (uint16_t *)ctx->fb;
        uint16_t *dst = fb16 + y * w + ctx->x0;
        for (unsigned x = 0; x < GB_W; x++) {
            uint16_t c = ctx->pal16[row[x]];
            for (unsigned k = 0; k < s; k++) {
                *dst++ = c;
            }
        }
        uint16_t *first = fb16 + y * w + ctx->x0;
        for (unsigned k = 1; k < s; k++) {
            memcpy(first + k * w, first, GB_W * s * 2);
        }
    }
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
        }
    }
    return ~c;
}

// Core entry points, shared by the host harness and the native module.
static int pgb_init(ctx_t *ctx, const uint8_t *rom, uint32_t rom_len) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rom = rom;
    ctx->rom_len = rom_len;
    // DMG green-free greys: white, light, dark, black.
    static const uint8_t p8[4] = {0xFF, 0xB6, 0x49, 0x00};
    static const uint16_t p16[4] = {0xFFFF, 0xAD55, 0x52AA, 0x0000};
    for (int i = 0; i < 4; i++) {
        ctx->pal8[i] = p8[i];
        ctx->pal16[i] = p16[i];
    }
    minigb_apu_audio_init(&ctx->apu);
    enum gb_init_error_e e = gb_init(&ctx->gb, rom_read, cart_ram_read, cart_ram_write, gb_error_cb, ctx);
    if (e != GB_INIT_NO_ERROR) {
        return (int)e;
    }
    gb_init_lcd(&ctx->gb, draw_line);
    return 0;
}

static uint32_t pgb_run_frames(ctx_t *ctx, uint32_t n) {
    uint32_t i = 0;
    while (i < n && ctx->err == 0) {
        gb_run_frame(&ctx->gb);
        if (ctx->ring != NULL) {
            // One VSYNC worth of stereo samples, copied into the ring with wrap.
            minigb_apu_audio_callback(&ctx->apu, ctx->frame_audio);
            const int16_t *src = ctx->frame_audio;
            uint32_t left = AUDIO_SAMPLES;
            while (left) {
                uint32_t room = ctx->ring_frames - ctx->ring_pos;
                uint32_t k = left < room ? left : room;
                memcpy(ctx->ring + ctx->ring_pos * 2, src, k * 4);
                src += k * 2;
                left -= k;
                ctx->ring_pos += k;
                if (ctx->ring_pos == ctx->ring_frames) {
                    ctx->ring_pos = 0;
                }
            }
        }
        i++;
    }
    return i;
}

// Cartridge RAM size. Upstream has the checked _s form; the CGB fork only has
// the older call, which returns 0 for an unknown size code.
static int pgb_save_size(struct gb_s *gb, size_t *size) {
#ifdef PGB_GBC
    *size = gb_get_save_size(gb);
    return 0;
#else
    return gb_get_save_size_s(gb, size);
#endif
}

// Joypad mask uses 1 = pressed; Peanut-GB's register is active low.
static void pgb_set_joypad(ctx_t *ctx, uint32_t mask) {
    ctx->gb.direct.joypad = (uint8_t)~mask;
}

#ifndef HOST_TEST

static ctx_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(ctx_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (ctx_t *)buf.buf;
}

static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(rom_in, &rom, MP_BUFFER_READ);
    return mp_obj_new_int(pgb_init(ctx, rom.buf, rom.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

static mp_obj_t mod_save_size(mp_obj_t st_in) {
    size_t size = 0;
    if (pgb_save_size(&get_ctx(st_in)->gb, &size) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad cartridge RAM size"));
    }
    return mp_obj_new_int(size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_save_size_obj, mod_save_size);

static mp_obj_t mod_set_cart_ram(mp_obj_t st_in, mp_obj_t ram_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t ram;
    mp_get_buffer_raise(ram_in, &ram, MP_BUFFER_RW);
    ctx->cart_ram = ram.buf;
    ctx->cart_len = ram.len;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_cart_ram_obj, mod_set_cart_ram);

// set_output(st, fb, width, bpp, scale, x0, y0); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    ctx_t *ctx = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        ctx->fb = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t fb;
    mp_get_buffer_raise(args[1], &fb, MP_BUFFER_WRITE);
    uint32_t width = mp_obj_get_int(args[2]);
    uint32_t bpp = mp_obj_get_int(args[3]);
    uint32_t scale = mp_obj_get_int(args[4]);
    uint32_t x0 = mp_obj_get_int(args[5]);
    uint32_t y0 = mp_obj_get_int(args[6]);
    uint32_t bytes = bpp / 8;
    if ((bpp != 8 && bpp != 16) || scale < 1 || scale > 4 ||
        x0 + GB_W * scale > width ||
        (y0 + GB_H * scale) * width * bytes > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    ctx->fb = fb.buf;
    ctx->fb_len = fb.len;
    ctx->fb_width = width;
    ctx->bpp = bpp;
    ctx->scale = scale;
    ctx->x0 = x0;
    ctx->y0 = y0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 7, 7, mod_set_output);

// set_audio(st, ring, start): ring is int16 stereo (array('h') / bytearray),
// start is the stereo frame to write next. ring=None stops audio output.
static mp_obj_t mod_set_audio(mp_obj_t st_in, mp_obj_t ring_in, mp_obj_t start_in) {
    ctx_t *ctx = get_ctx(st_in);
    if (ring_in == mp_const_none) {
        ctx->ring = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t ring;
    mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
    uint32_t frames = ring.len / 4;
    uint32_t start = mp_obj_get_int(start_in);
    if (frames == 0 || start >= frames) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad audio ring"));
    }
    ctx->ring = ring.buf;
    ctx->ring_frames = frames;
    ctx->ring_pos = start;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_audio_obj, mod_set_audio);

static mp_obj_t mod_set_joypad(mp_obj_t st_in, mp_obj_t mask_in) {
    pgb_set_joypad(get_ctx(st_in), mp_obj_get_int(mask_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_joypad_obj, mod_set_joypad);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(pgb_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    return mp_obj_new_int_from_uint(crc32(ctx->shade, sizeof(ctx->shade)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

// error(st) -> (code, addr); code 0 means no error, else gb_error_e + 1.
static mp_obj_t mod_error(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(ctx->err), mp_obj_new_int(ctx->err_addr)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(ctx_t)));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(AUDIO_SAMPLE_RATE));
    mp_store_global(MP_QSTR_AUDIO_SAMPLES_PER_FRAME, MP_OBJ_NEW_SMALL_INT(AUDIO_SAMPLES));
    mp_store_global(MP_QSTR_JOYPAD_A, MP_OBJ_NEW_SMALL_INT(JOYPAD_A));
    mp_store_global(MP_QSTR_JOYPAD_B, MP_OBJ_NEW_SMALL_INT(JOYPAD_B));
    mp_store_global(MP_QSTR_JOYPAD_SELECT, MP_OBJ_NEW_SMALL_INT(JOYPAD_SELECT));
    mp_store_global(MP_QSTR_JOYPAD_START, MP_OBJ_NEW_SMALL_INT(JOYPAD_START));
    mp_store_global(MP_QSTR_JOYPAD_RIGHT, MP_OBJ_NEW_SMALL_INT(JOYPAD_RIGHT));
    mp_store_global(MP_QSTR_JOYPAD_LEFT, MP_OBJ_NEW_SMALL_INT(JOYPAD_LEFT));
    mp_store_global(MP_QSTR_JOYPAD_UP, MP_OBJ_NEW_SMALL_INT(JOYPAD_UP));
    mp_store_global(MP_QSTR_JOYPAD_DOWN, MP_OBJ_NEW_SMALL_INT(JOYPAD_DOWN));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_save_size, MP_OBJ_FROM_PTR(&mod_save_size_obj));
    mp_store_global(MP_QSTR_set_cart_ram, MP_OBJ_FROM_PTR(&mod_set_cart_ram_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_joypad, MP_OBJ_FROM_PTR(&mod_set_joypad_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
