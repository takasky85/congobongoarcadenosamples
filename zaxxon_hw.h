/* Sega Zaxxon (1982) main board, for the Congo Bongo / Zaxxon emulator.
 *
 * Self-contained (no SDL): Z80 program ROMs, memory map, the two control latches, the 8255 that drives the
 * discrete sound circuits, the scrolling skewed background, 32 sprites and the character layer.  The sound
 * itself is generated in zaxxon_synth.h, so no audio samples are needed.
 *
 * Hardware behaviour follows MAME's zaxxon.cpp driver (Nicola Salmoria, Stephane Humbert and the MAME team,
 * BSD-3-Clause), re-implemented here and checked against the Gremlin/Sega Zaxxon owner's manual 420-0724. */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "z80.h"
#include "zaxxon_synth.h"

namespace zx {

static constexpr double MAIN_HZ = 48660000.0 / 16;   /* 3.04125 MHz */
static constexpr int    RATE = 48000;                /* audio output rate */
static constexpr int    FRAME_CYC = 384 * 264 / 2;   /* 50688 main CPU cycles per frame (pixel clock 48.66 MHz / 8) */
static constexpr int    VBLANK_AT = 240 * 192;       /* vertical blank starts on line 240 */
static constexpr int    SCR_W = 256, SCR_H = 224;

/* ----------------------------------------------------------------- */
/*  ROM sets                                                          */
/* ----------------------------------------------------------------- */
struct SetDef {
	const char *name;          /* MAME short name */
	const char *title;
	uint32_t main_crc[3];      /* program ROMs: U27 at 0000, U28 at 2000, U29 (4 KB) at 4000 */
	uint32_t prom_crc;         /* character colour-code PROM */
};
static const SetDef SETS[] = {
	{ "zaxxon", "Zaxxon (set 1, rev D)", { 0x6e2b4a30, 0x1c9ea398, 0x1c123ef9 }, 0xdeaa21f7 },
};
static constexpr int N_SETS = (int)(sizeof SETS / sizeof SETS[0]);

/* sound-circuit test entries for the options menu: drive one 8255 pin low (active) for a while */
struct TestSound { const char *name; uint8_t a, b, c; int frames; };   /* a/b/c: port bits pulled low */
static const TestSound TEST_SOUNDS[] = {
	{ "ENGINE (START)",   0x0c, 0, 0, 150 }, { "ENGINE (FLIGHT)",  0x08, 0, 0, 150 }, { "HOMING MISSILE", 0x10, 0, 0, 100 }, { "BASE MISSILE", 0x20, 0, 0, 3 },
	{ "LASER",            0x40, 0, 0, 3 },   { "BATTLESHIP",     0x80, 0, 0, 100 }, { "SMALL EXPLOSION", 0, 0x10, 0, 3 },
	{ "LARGE EXPLOSION",  0, 0x20, 0, 3 },   { "CANNON",         0, 0x80, 0, 3 },   { "ENEMY SHOT", 0, 0, 0x01, 3 },
	{ "TARGET ALARM",     0, 0, 0x04, 3 },   { "FUEL ALARM",     0, 0, 0x08, 3 },
};
static constexpr int N_TEST_SOUNDS = (int)(sizeof TEST_SOUNDS / sizeof TEST_SOUNDS[0]);

/* graphics and palette ROMs, the same in every set (CRC32, size).  Planes are listed least significant first. */
struct GfxRom { const char *what; uint32_t crc; size_t size; };
static const GfxRom CHAR_ROMS[2] = { { "characters 1 (U68)", 0x07bf8c52, 0x800 }, { "characters 2 (U69)", 0xc215edcb, 0x800 } };
static const GfxRom BG_ROMS[3]   = { { "background 1 (U113)", 0x6e07bb68, 0x2000 }, { "background 2 (U112)", 0x0a5bce6a, 0x2000 }, { "background 3 (U111)", 0xa5bf1465, 0x2000 } };
static const GfxRom SPR_ROMS[3]  = { { "sprites 1 (U79)", 0xab4e8a9a, 0x2000 }, { "sprites 2 (U78)", 0x1c5369c7, 0x2000 }, { "sprites 3 (U77)", 0xeaf0dd4b, 0x2000 } };
static const GfxRom MAP_ROMS[4]  = { { "tile map 1 (U90)", 0x6284c200, 0x2000 }, { "tile map 2 (U91)", 0x28d65063, 0x2000 }, { "tile map 3 (U92)", 0x7e42691f, 0x2000 }, { "tile map 4 (U93)", 0xa95e61fd, 0x2000 } };
static const GfxRom PAL_ROM      = { "palette PROM (U76)", 0x6cc6695b, 0x100 };

/* find the set whose ROMs (program, graphics, PROMs) are all present (files: anything with .crc and .data) */
template <class Files> static int detect_set(const Files &files)
{
	auto has = [&](uint32_t crc) { for (auto &f : files) if (f.crc == crc) return true; return false; };
	for (int i = 0; i < N_SETS; i++) {
		bool all = has(SETS[i].main_crc[0]) && has(SETS[i].main_crc[1]) && has(SETS[i].main_crc[2]) && has(SETS[i].prom_crc) && has(PAL_ROM.crc);
		for (auto &g : CHAR_ROMS) all = all && has(g.crc);
		for (auto &g : BG_ROMS) all = all && has(g.crc);
		for (auto &g : SPR_ROMS) all = all && has(g.crc);
		for (auto &g : MAP_ROMS) all = all && has(g.crc);
		if (all) return i;
	}
	return -1;
}

/* ----------------------------------------------------------------- */
/*  The board                                                         */
/* ----------------------------------------------------------------- */
struct Board {
	Z80 cpu;
	int set = 0;

	/* memory */
	uint8_t rom[0x6000];       /* program */
	uint8_t ram[0x1000], vram[0x400], spr[0x100];

	/* decoded graphics */
	uint8_t txpix[256 * 64];
	uint8_t sppix[64 * 1024];
	std::vector<uint16_t> bgpix;       /* 256 x 4096 pens (colour * 8 + pixel) */
	uint8_t ccode[256];                /* character colour codes (per column and 4-row band) */
	uint32_t pal[256];

	/* control latches (two 74LS259) and state */
	uint8_t l0 = 0, l1 = 0;
	bool int_en = false, irq_line = false;
	int coin_status[3] = { 0, 0, 0 };
	bool bg_enable = false, flip = false;
	int bg_color = 0, fg_color = 0, bg_pos = 0;

	/* inputs (active high), as read at C000 / C001 / C100 */
	uint8_t sw00 = 0, sw01 = 0, sw100 = 0;
	uint8_t dsw02 = 0x7f;   /* bonus 10000, 3 lives, sound on, upright */
	uint8_t dsw03 = 0x33;   /* 1 coin / 1 credit */
	bool service = false;   /* service (self-test) switch, edge sensitive: NMI */
	bool service_prev = false;

	/* 8255 (E03C-E03F): the sound board */
	uint8_t pa = 0, pb = 0, pc = 0;
	uint8_t pa_log_prev = 0, pb_log_prev = 0, pc_log_prev = 0;
	uint8_t ta = 0, tb = 0, tc = 0; int test_frames = 0;   /* menu test sound: pins held low on top of what the game drives */
	bool sound_only = false;                               /* run only the sound circuits (game frozen), e.g. in the audio menu */
	zxs::Synth synth;
	void (*ppi_hook)(void *, uint64_t cyc, uint8_t a, uint8_t b, uint8_t c) = nullptr;   /* developer: log port writes */
	void *ppi_hook_ud = nullptr;

	/* timing / output */
	double main_target = 0;
	int frame_cyc = 0;
	bool vb_done = false, frame_done = false;
	long frames = 0;
	uint32_t fb[SCR_W * SCR_H];
	std::vector<int16_t> audio;
	float volume = 1.0f;
	double dc_x = 0, dc_y = 0;
	std::vector<std::pair<uint16_t, uint8_t>> pokes;   /* developer: RAM bytes (6000-6FFF) written back every frame */

	/* ------------------------------------------------------------ */
	template <class Files> bool load(const Files &files, int set_index, std::string *err = nullptr)
	{
		set = set_index;
		auto get = [&](uint32_t crc, const char *what, size_t size, std::vector<uint8_t> &out) {
			for (auto &f : files) if (f.crc == crc) { out = f.data; if (out.size() < size) out.resize(size, 0); return true; }
			if (err) { char b[160]; snprintf(b, sizeof b, "missing ROM: %s (CRC %08x)\n", what, crc); *err += b; }
			return false;
		};
		bool ok = true;
		std::vector<uint8_t> d;
		memset(rom, 0xff, sizeof rom);
		static const char *mn[3] = { "program U27", "program U28", "program U29" };
		static const size_t ms[3] = { 0x2000, 0x2000, 0x1000 };
		static const size_t mo[3] = { 0x0000, 0x2000, 0x4000 };
		for (int i = 0; i < 3; i++) {
			if (!get(SETS[set].main_crc[i], mn[i], ms[i], d)) { ok = false; continue; }
			memcpy(rom + mo[i], d.data(), ms[i]);
		}
		memcpy(rom + 0x5000, rom + 0x4000, 0x1000);        /* the 2532 at U29 has no A12: it shows up twice */

		std::vector<uint8_t> ch[2], bg[3], sp[3], mp[4], pal_rom, cc;
		for (int i = 0; i < 2; i++) ok &= get(CHAR_ROMS[i].crc, CHAR_ROMS[i].what, CHAR_ROMS[i].size, ch[i]);
		for (int i = 0; i < 3; i++) ok &= get(BG_ROMS[i].crc, BG_ROMS[i].what, BG_ROMS[i].size, bg[i]);
		for (int i = 0; i < 3; i++) ok &= get(SPR_ROMS[i].crc, SPR_ROMS[i].what, SPR_ROMS[i].size, sp[i]);
		for (int i = 0; i < 4; i++) ok &= get(MAP_ROMS[i].crc, MAP_ROMS[i].what, MAP_ROMS[i].size, mp[i]);
		ok &= get(PAL_ROM.crc, PAL_ROM.what, PAL_ROM.size, pal_rom);
		ok &= get(SETS[set].prom_crc, "colour-code PROM (U72/U41)", 0x100, cc);
		if (!ok) return false;
		decode_gfx(ch, bg, sp, mp, pal_rom, cc);
		return true;
	}

	void decode_gfx(const std::vector<uint8_t> *ch, const std::vector<uint8_t> *bg, const std::vector<uint8_t> *sp,
	                const std::vector<uint8_t> *mp, const std::vector<uint8_t> &pal_rom, const std::vector<uint8_t> &cc)
	{
		/* characters: 256 tiles of 8x8, 2 bits (planar, second ROM is the high bit) */
		for (int c = 0; c < 256; c++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) {
					int b0 = (ch[0][c * 8 + y] >> (7 - x)) & 1, b1 = (ch[1][c * 8 + y] >> (7 - x)) & 1;
					txpix[c * 64 + y * 8 + x] = (uint8_t)(b1 << 1 | b0);
				}
		/* background tiles: 1024 tiles of 8x8, 3 bits planar */
		static uint8_t bgtile[1024 * 64];
		for (int c = 0; c < 1024; c++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) {
					int b0 = (bg[0][c * 8 + y] >> (7 - x)) & 1, b1 = (bg[1][c * 8 + y] >> (7 - x)) & 1, b2 = (bg[2][c * 8 + y] >> (7 - x)) & 1;
					bgtile[c * 64 + y * 8 + x] = (uint8_t)(b2 << 2 | b1 << 1 | b0);
				}
		/* sprites: 64 of 32x32, 3 bits planar */
		for (int c = 0; c < 64; c++)
			for (int y = 0; y < 32; y++)
				for (int x = 0; x < 32; x++) {
					int byte = c * 128 + (x >> 3) * 8 + (y & 7) + (y >> 3) * 32, bit = 7 - (x & 7);
					int p0 = (sp[0][byte] >> bit) & 1, p1 = (sp[1][byte] >> bit) & 1, p2 = (sp[2][byte] >> bit) & 1;
					sppix[c * 1024 + y * 32 + x] = (uint8_t)(p2 << 2 | p1 << 1 | p0);
				}
		/* background pixmap: 32 x 512 tiles of 8x8.  The tile map ROMs hold the tile codes (first half) and an attribute
		 * byte (second half): bits 0-1 = code bits 8-9, bits 4-7 = colour */
		static uint8_t tmap[0x8000];
		for (int i = 0; i < 4; i++) memcpy(tmap + i * 0x2000, mp[i].data(), 0x2000);
		bgpix.assign(256 * 4096, 0);
		for (int ti = 0; ti < 32 * 512; ti++) {
			int row = ti / 32, col = ti % 32;
			int code = tmap[ti] + 256 * (tmap[ti + 0x4000] & 3), color = tmap[ti + 0x4000] >> 4;
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					bgpix[(row * 8 + y) * 256 + col * 8 + x] = (uint16_t)(color * 8 + bgtile[code * 64 + y * 8 + x]);
		}
		memcpy(ccode, cc.data(), 256);

		/* palette: resistor weights 1000/470/220 ohms (R,G: 3 bits, B: 2 bits) with a 470 ohm pulldown */
		auto weights = [](const double *r, int n, double *w, double scale) {
			double sum = 1.0 / 470.0;
			for (int i = 0; i < n; i++) sum += 1.0 / r[i];
			for (int i = 0; i < n; i++) w[i] = scale * (1.0 / r[i]) / sum;
		};
		const double rr[3] = { 1000, 470, 220 }, rb[2] = { 470, 220 };
		double full = 0; { double sum = 1.0 / 470.0, on = 0; for (double r : rr) { sum += 1.0 / r; on += 1.0 / r; } full = on / sum; }
		double rw[3], gw[3], bw[2];
		weights(rr, 3, rw, 255.0 / full); weights(rr, 3, gw, 255.0 / full); weights(rb, 2, bw, 255.0 / full);
		for (int i = 0; i < 256; i++) {
			int v = pal_rom[i];
			auto bit = [&](int b) { return (v >> b) & 1; };
			int r = (int)(rw[0] * bit(0) + rw[1] * bit(1) + rw[2] * bit(2) + 0.5);
			int g = (int)(gw[0] * bit(3) + gw[1] * bit(4) + gw[2] * bit(5) + 0.5);
			int b = (int)(bw[0] * bit(6) + bw[1] * bit(7) + 0.5);
			pal[i] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
	}

	/* ------------------------------------------------------------ */
	void reset()
	{
		memset(ram, 0, sizeof ram); memset(vram, 0, sizeof vram); memset(spr, 0, sizeof spr);
		l0 = l1 = 0; int_en = irq_line = false;
		coin_status[0] = coin_status[1] = coin_status[2] = 0;
		bg_enable = flip = false; bg_color = fg_color = bg_pos = 0;
		pa = pb = pc = 0; service_prev = false;
		cpu.ud = this; cpu.rd = bus_rd; cpu.wr = bus_wr; cpu.in = io_rd; cpu.out = io_wr;
		z80_reset(&cpu);
		synth.reset(RATE);
		synth.ports(0xff, 0xff, 0xff);
		main_target = 0; frame_cyc = 0; vb_done = false; frame_done = false; dc_x = dc_y = 0;
	}

	static uint8_t io_rd(void *, uint16_t) { return 0xff; }
	static void io_wr(void *, uint16_t, uint8_t) {}

	/* ------------------------------------------------------------ */
	/*  CPU bus                                                       */
	/* ------------------------------------------------------------ */
	static uint8_t bus_rd(void *ud, uint16_t a)
	{
		Board *b = (Board *)ud;
		switch (a >> 13) {
		case 0: case 1: case 2: return b->rom[a];
		case 3: return b->ram[a & 0xfff];                    /* 6000-7FFF: 4 KB RAM, mirrored */
		case 4: return b->vram[a & 0x3ff];                   /* 8000-9FFF: video RAM, mirrored */
		case 5: return b->spr[a & 0xff];                     /* A000-BFFF: sprite RAM, mirrored */
		case 6:                                              /* C000-DFFF: inputs */
			switch ((a >> 8) & 7) {
			case 0:
				switch (a & 3) {
				case 0: return b->sw00;
				case 1: return b->sw01;
				case 2: return b->dsw02;
				default: return b->dsw03;
				}
			case 1: return (uint8_t)(b->sw100 | (b->coin_status[0] << 5) | (b->coin_status[1] << 6) | (b->coin_status[2] << 7));
			}
			return 0xff;
		case 7:                                              /* E000-FFFF: 8255 */
			if ((a & 0xff) >= 0x3c && (a & 0xff) <= 0x3f) {
				switch (a & 3) { case 0: return b->pa; case 1: return b->pb; case 2: return b->pc; }
				return 0;
			}
			return 0xff;
		}
		return 0xff;
	}

	void latch0_write(int bit, int val)     /* U55: coin enables, counters, flip */
	{
		if (val) l0 |= (uint8_t)(1 << bit); else l0 &= (uint8_t)~(1 << bit);
		switch (bit) {
		case 0: case 1: case 2:
			for (int n = 0; n < 3; n++) if (!((l0 >> n) & 1)) coin_status[n] = 0;   /* clearing an enable clears its coin status */
			break;
		case 6: flip = !val; break;
		}
	}
	void latch1_write(int bit, int val)     /* U56: interrupt enable, colour banks, background enable */
	{
		if (val) l1 |= (uint8_t)(1 << bit); else l1 &= (uint8_t)~(1 << bit);
		switch (bit) {
		case 0: int_en = val; if (!val) irq_line = false; break;
		case 1: fg_color = val * 0x80; break;
		case 6: bg_color = val * 0x80; break;
		case 7: bg_enable = val; break;
		}
	}

	void push_ports() { synth.ports((uint8_t)(pa & ~ta), (uint8_t)(pb & ~tb), (uint8_t)(pc & ~tc)); }
	void play_test(int i)
	{
		if (i < 0 || i >= N_TEST_SOUNDS) return;
		ta = TEST_SOUNDS[i].a; tb = TEST_SOUNDS[i].b; tc = TEST_SOUNDS[i].c; test_frames = TEST_SOUNDS[i].frames; push_ports();
	}
	void stop_test() { ta = tb = tc = 0; test_frames = 0; push_ports(); }

	void ppi_write(int reg, uint8_t v)
	{
		switch (reg) {
		case 0: pa = v; break;
		case 1: pb = v; break;
		case 2: pc = v; break;
		default:
			if (v & 0x80) pa = pb = pc = 0;                  /* mode set: the outputs are cleared; all ports stay outputs */
			else { int bit = (v >> 1) & 7; if (v & 1) pc |= (uint8_t)(1 << bit); else pc &= (uint8_t)~(1 << bit); }
		}
		push_ports();
		if (ppi_hook) ppi_hook(ppi_hook_ud, cpu.cycles, pa, pb, pc);
	}

	static void bus_wr(void *ud, uint16_t a, uint8_t v)
	{
		Board *b = (Board *)ud;
		switch (a >> 13) {
		case 3: b->ram[a & 0xfff] = v; return;
		case 4: b->vram[a & 0x3ff] = v; return;
		case 5: b->spr[a & 0xff] = v; return;
		case 6:
			if (((a >> 8) & 7) == 0) b->latch0_write(a & 7, v & 1);
			return;
		case 7: {
			const int lo = a & 0xff;
			if (lo >= 0x3c && lo <= 0x3f) { b->ppi_write(a & 3, v); return; }
			if (lo >= 0xf0 && lo <= 0xfb && !(lo & 4)) {
				const int out = (lo & 3) | ((lo & 8) ? 4 : 0);
				b->latch1_write(out, v & 1);
				if ((lo & 8) && !(lo & 2)) {                 /* E0F8 / E0F9: background scroll position, low / high byte */
					if (lo & 1) b->bg_pos = (b->bg_pos & 0x0ff) | ((v << 8) & 0x700);
					else b->bg_pos = (b->bg_pos & 0x700) | v;
				}
			}
			return; }
		}
	}

	/* ------------------------------------------------------------ */
	/*  video                                                         */
	/* ------------------------------------------------------------ */
	static int find_minimum_y(uint8_t value, int flip_)
	{
		int flipmask = flip_ ? 0xff : 0x00, flipconst = flip_ ? 0xef : 0xf1, y;
		for (y = 0; y < 256; y += 16) {
			int sum = (value + flipconst + 1) + (y ^ flipmask);
			if ((sum & 0xe0) == 0xe0) break;
		}
		while (true) {
			int sum = (value + flipconst + 1) + ((y - 1) ^ flipmask);
			if ((sum & 0xe0) != 0xe0) break;
			y--;
		}
		return (y + 1) & 0xff;
	}
	static int find_minimum_x(uint8_t value, int flip_)
	{
		int flipmask = flip_ ? 0xff : 0x00;
		int x = (value + 0xef + 1) ^ flipmask;
		if (flipmask) x -= 31;
		return x & 0xff;
	}

	void render()
	{
		static uint16_t pen[SCR_W * SCR_H];
		const int flipmask = flip ? 0xff : 0x00;

		/* 1. scrolling, skewed background */
		if (bg_enable) {
			const int flipoffs = 0x3f;                      /* 0x40 - 1 upright, 0x38 + 7 flipped: the same column either way */
			for (int y = 16; y < 16 + SCR_H; y++) {
				uint16_t *dst = &pen[(y - 16) * SCR_W];
				int vf = y ^ flipmask;
				int srcy = vf + ((bg_pos << 1) ^ 0xfff) + 1;
				const uint16_t *src = &bgpix[(srcy & 4095) * 256];
				int add = ((vf >> 1) ^ 0xff) + 1 + flipoffs;
				for (int x = 0; x < SCR_W; x++) {
					int srcx = (x ^ flipmask) + add;
					dst[x] = (uint16_t)(src[srcx & 255] + bg_color);
				}
			}
		} else {
			for (int i = 0; i < SCR_W * SCR_H; i++) pen[i] = 0xffff;
		}

		/* 2. sprites: only the lower half of sprite RAM is read while drawing, 32 entries, from the last to the first */
		for (int offs = 0x7c; offs >= 0; offs -= 4) {
			int sy = find_minimum_y(spr[offs], flip);
			int flipy = ((spr[offs + 1] ^ flipmask) & 0x80) != 0;
			int flipx = ((spr[offs + 1] ^ flipmask) & 0x40) != 0;
			int code = spr[offs + 1] & 0x3f;
			int color = spr[offs + 2] & 0x1f;
			int sx = find_minimum_x(spr[offs + 3], flip);
			const uint8_t *src = &sppix[code * 1024];
			for (int copy = 0; copy < 4; copy++) {
				int ox = sx - ((copy & 1) ? 0x100 : 0);
				int oy = sy - ((copy & 2) ? 0x100 : 0);
				for (int y = 0; y < 32; y++) {
					int Y = oy + y;
					if (Y < 16 || Y >= 16 + SCR_H) continue;
					const uint8_t *row = src + (flipy ? 31 - y : y) * 32;
					uint16_t *dst = &pen[(Y - 16) * SCR_W];
					for (int x = 0; x < 32; x++) {
						int X = ox + x;
						if (X < 0 || X >= SCR_W) continue;
						int p = row[flipx ? 31 - x : x];
						if (p) dst[X] = (uint16_t)(color * 8 + p);
					}
				}
			}
		}

		/* 3. 32x32 character layer: pen 0 is transparent, colour comes from the colour-code PROM (per column, per 4 rows) */
		for (int y = 16; y < 16 + SCR_H; y++) {
			uint16_t *dst = &pen[(y - 16) * SCR_W];
			int ty = flip ? 255 - y : y;
			for (int x = 0; x < SCR_W; x++) {
				int tx = flip ? 255 - x : x;
				int sxx = tx >> 3, syy = ty >> 3;
				int code = vram[syy * 32 + sxx];
				int p = txpix[code * 64 + (ty & 7) * 8 + (tx & 7)];
				if (p) dst[x] = (uint16_t)((ccode[sxx + 32 * (syy / 4)] & 0x0f) * 8 + p + fg_color);
			}
		}

		for (int i = 0; i < SCR_W * SCR_H; i++)
			fb[i] = pen[i] == 0xffff ? 0xff000000u : pal[pen[i] & 255];
	}

	/* ------------------------------------------------------------ */
	/*  one output sample (1/48000 s)                                 */
	/* ------------------------------------------------------------ */
	void step_sample()
	{
		main_target += MAIN_HZ / RATE;
		while ((double)cpu.cycles < main_target) {
			cpu.irq = irq_line;
			uint64_t c0 = cpu.cycles;
			z80_step(&cpu);
			frame_cyc += (int)(cpu.cycles - c0);
			if (!vb_done && frame_cyc >= VBLANK_AT) {
				vb_done = true;
				if (int_en) irq_line = true;
				render();
			}
			if (frame_cyc >= FRAME_CYC) { frame_cyc -= FRAME_CYC; vb_done = false; frame_done = true; frames++; }
		}
		double x = synth.tick();
		/* AC coupling, gain, soft clip */
		double y = x - dc_x + 0.9995 * dc_y;
		dc_x = x; dc_y = y;
		audio.push_back((int16_t)std::lrint(std::tanh(y * volume * 1.4) * 32000.0));
	}

	void run_frame()
	{
		frame_done = false;
		audio.clear();
		if (test_frames > 0 && --test_frames == 0) stop_test();
		if (sound_only) {                                      /* game frozen: only the sound circuits run */
			for (int i = 0; i < RATE / 60; i++) {
				double x = synth.tick();
				double y = x - dc_x + 0.9995 * dc_y;
				dc_x = x; dc_y = y;
				audio.push_back((int16_t)std::lrint(std::tanh(y * volume * 1.4) * 32000.0));
			}
			return;
		}
		if (service && !service_prev) cpu.nmi = 1;            /* the service switch pulses NMI (self-test) */
		service_prev = service;
		while (!frame_done) step_sample();
		for (auto &p : pokes) if (p.first >= 0x6000 && p.first < 0x7000) ram[p.first & 0xfff] = p.second;
	}

	/* a coin dropped into slot n (0 = A, 1 = B, 2 = service credit): counted only while its enable is on */
	void insert_coin(int n) { if ((l0 >> n) & 1) coin_status[n] = 1; }
};

}  // namespace zx
