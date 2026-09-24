// Host reference for peanutgb.c: autoplay script identical to the SameBoy
// tetrisplayer bench, frame CRC at checkpoints, PGM screenshot at the end.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "peanutgb.c"

// Same frames and holds as tetrisplayer.ino keys(): pulse = 6 frames.
static uint32_t script_mask(uint32_t f) {
    static const struct { uint32_t at, hold, mask; } s[] = {
        {600, 6, JOYPAD_START}, {720, 6, JOYPAD_START}, {840, 6, JOYPAD_START},
        {960, 6, JOYPAD_A}, {1020, 12, JOYPAD_LEFT}, {1080, 12, JOYPAD_RIGHT},
        {1140, 60, JOYPAD_DOWN},
    };
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; i++) {
        if (f >= s[i].at && f < s[i].at + s[i].hold) return s[i].mask;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s rom.gb [frames] [out.pgm] [out.wav]\n", argv[0]); return 2; }
    uint32_t frames = argc > 2 ? atoi(argv[2]) : 1800;
    FILE *fp = fopen(argv[1], "rb");
    static uint8_t rom[8 << 20];
    size_t n = fread(rom, 1, sizeof rom, fp); fclose(fp);
    static ctx_t ctx;
    int e = pgb_init(&ctx, rom, n);
    if (e) { printf("init error %d\n", e); return 1; }
    size_t save = 0;
    if (pgb_save_size(&ctx.gb, &save) != 0) { printf("bad save size\n"); return 1; }
    static uint8_t cart[128 << 10];
    ctx.cart_ram = cart; ctx.cart_len = save;
    static int16_t ring[AUDIO_SAMPLES * 2 * 2000];
    ctx.ring = ring; ctx.ring_frames = AUDIO_SAMPLES * 2000; ctx.ring_pos = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t f = 0; f < frames; f++) {
        pgb_set_joypad(&ctx, script_mask(f));
        if (pgb_run_frames(&ctx, 1) != 1) { printf("core error %u at 0x%04x frame %u\n", ctx.err - 1, ctx.err_addr, f); return 1; }
        if ((f + 1) % 300 == 0) printf("frame %u crc %08x\n", f + 1, crc32(ctx.shade, sizeof ctx.shade));
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("BENCH frames=%u crc=%08x save=%zu host=%.3fs (%.0f fps)\n", frames, crc32(ctx.shade, sizeof ctx.shade), save, dt, frames / dt);
    {
        // Audio stats over the run: peak and a checksum, plus WAV if asked.
        uint32_t nsamp = frames < 2000 ? frames * AUDIO_SAMPLES * 2 : AUDIO_SAMPLES * 2 * 2000;
        int peak = 0; uint32_t sum = 0, nz = 0;
        for (uint32_t i = 0; i < nsamp; i++) { int v = ring[i]; if (v < 0) v = -v; if (v > peak) peak = v; if (v) nz++; sum = sum * 31 + (uint16_t)ring[i]; }
        printf("AUDIO rate=%d per_frame=%u samples=%u nonzero=%u peak=%d sum=%08x\n", AUDIO_SAMPLE_RATE, (unsigned)AUDIO_SAMPLES, nsamp / 2, nz, peak, sum);
        if (argc > 4) {
            FILE *w = fopen(argv[4], "wb"); uint32_t bytes = nsamp * 2, rate = AUDIO_SAMPLE_RATE, br = rate * 4; uint16_t ch = 2, ba = 4, bits = 16;
            uint32_t riff = 36 + bytes, fmt = 16; uint16_t pcm = 1;
            fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmt, 4, 1, w);
            fwrite(&pcm, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w); fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
            fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(ring, 2, nsamp, w); fclose(w);
        }
    }
    if (argc > 3) {
        FILE *o = fopen(argv[3], "wb");
#ifdef PGB_GBC
        if (ctx.gb.cgb.cgbMode) {
            // Colour frame from the final CGB palette (RGB555).
            fprintf(o, "P6 %d %d 31\n", GB_W, GB_H);
            for (int i = 0; i < GB_W * GB_H; i++) {
                uint16_t c = ctx.gb.cgb.fixPalette[ctx.shade[i]];
                fputc((c >> 10) & 31, o); fputc((c >> 5) & 31, o); fputc(c & 31, o);
            }
            fclose(o);
            printf("CGB mode frame written\n");
            return 0;
        }
#endif
        fprintf(o, "P5 %d %d 3\n", GB_W, GB_H);
        for (int i = 0; i < GB_W * GB_H; i++) fputc(3 - ctx.shade[i], o);
        fclose(o);
    }
    return 0;
}
