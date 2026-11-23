/*
 * Congo Bongo / Tip Top (Sega, 1983) - small standalone emulator for SDL3.
 *
 * Hardware, memory map, video and input behaviour follow MAME's zaxxon driver
 * (Nicola Salmoria, Stephane Humbert et al., BSD-3-Clause).  The sound board
 * (Z80 + 2x SN76489A + 8255) runs the real sound ROM; the analog bass drum,
 * congas, rim shot and gorilla circuits are synthesized from circuit models
 * (congo_perc_synth.h / congo_gorilla_synth.h), so NO audio samples are needed.
 *
 * You must supply your own ROM set (MAME "congo" or "tiptop" zip, or loose files).
 *
 *   congobongo [options] [rom-zip-or-folder ...]
 *
 * See README.md for keys and options.  Third-party code, data and licenses: README.md ("Licenses and
 * credits") and THIRD-PARTY-NOTICES.txt (MAME BSD-3-Clause, puff, Press Start 2P OFL, SDL3 zlib).
 */
#ifndef CB_LIBRETRO   /* the libretro core (libretro.cpp) uses only the emulation, not SDL */
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "z80.c"      /* the Z80 core (compiled as part of this file) */
#undef A
#undef F
#undef B
#undef C
#undef D
#undef E
#undef H
#undef L

#include "congo_perc_synth.h"
#include "congo_gorilla_synth.h"

namespace cb {

/* ===================================================================== */
/*  Constants                                                            */
/* ===================================================================== */
static constexpr double MAIN_HZ   = 48660000.0 / 16;   /* 3.04125 MHz */
static constexpr double SND_HZ    = 4000000.0;
static constexpr int    RATE      = 48000;             /* audio output rate */
static constexpr int    FRAME_CYC = 384 * 264 / 2;     /* 50688 main CPU cycles / frame */
static constexpr int    VBLANK_AT = 240 * 192;         /* vblank starts on line 240 */
static constexpr double BIN       = SND_HZ / RATE;     /* sound CPU cycles per output sample */
static constexpr int    SCR_W = 256, SCR_H = 224;

/* ===================================================================== */
/*  CRC32, inflate, zip                                                  */
/* ===================================================================== */
static uint32_t crc32(const uint8_t *p, size_t n)
{
	static uint32_t tab[256];
	if (!tab[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
	}
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return ~c;
}

/* small inflate after Mark Adler's public-domain "puff" */
struct Inflater {
	const uint8_t *in; size_t inlen, incnt = 0;
	uint32_t bitbuf = 0; int bitcnt = 0;
	uint8_t *out; size_t outlen, outcnt = 0;
	bool err = false;

	struct Huff { uint16_t count[16]; uint16_t symbol[288]; };

	int bits(int need)
	{
		uint32_t val = bitbuf;
		while (bitcnt < need) {
			if (incnt >= inlen) { err = true; return 0; }
			val |= (uint32_t)in[incnt++] << bitcnt;
			bitcnt += 8;
		}
		bitbuf = val >> need;
		bitcnt -= need;
		return (int)(val & ((1u << need) - 1));
	}

	int decode(const Huff &h)
	{
		int code = 0, first = 0, index = 0;
		for (int len = 1; len <= 15; len++) {
			code |= bits(1);
			if (err) return -1;
			int count = h.count[len];
			if (code - count < first) return h.symbol[index + (code - first)];
			index += count; first += count; first <<= 1; code <<= 1;
		}
		return -1;
	}

	static int construct(Huff &h, const uint8_t *length, int n)
	{
		memset(h.count, 0, sizeof h.count);
		for (int s = 0; s < n; s++) h.count[length[s]]++;
		if (h.count[0] == n) return 0;
		int left = 1;
		for (int len = 1; len <= 15; len++) {
			left <<= 1; left -= h.count[len];
			if (left < 0) return left;
		}
		uint16_t offs[16]; offs[1] = 0;
		for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
		for (int s = 0; s < n; s++) if (length[s]) h.symbol[offs[length[s]]++] = (uint16_t)s;
		return left;
	}

	bool codes(const Huff &lc, const Huff &dc)
	{
		static const uint16_t lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
		static const uint16_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
		static const uint16_t dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
		static const uint16_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
		int sym;
		do {
			sym = decode(lc);
			if (sym < 0 || err) return false;
			if (sym < 256) {
				if (outcnt >= outlen) return false;
				out[outcnt++] = (uint8_t)sym;
			} else if (sym > 256) {
				sym -= 257;
				if (sym >= 29) return false;
				int len = lens[sym] + bits(lext[sym]);
				int ds = decode(dc);
				if (ds < 0 || ds >= 30) return false;
				size_t dist = dists[ds] + bits(dext[ds]);
				if (err || dist > outcnt || outcnt + len > outlen) return false;
				while (len--) { out[outcnt] = out[outcnt - dist]; outcnt++; }
			}
		} while (sym != 256);
		return true;
	}

	bool run()
	{
		int last;
		do {
			last = bits(1);
			int type = bits(2);
			if (err) return false;
			if (type == 0) {
				bitbuf = 0; bitcnt = 0;
				if (incnt + 4 > inlen) return false;
				unsigned len = in[incnt] | (in[incnt + 1] << 8);
				incnt += 4;
				if (incnt + len > inlen || outcnt + len > outlen) return false;
				memcpy(out + outcnt, in + incnt, len);
				incnt += len; outcnt += len;
			} else if (type == 1) {
				static Huff lc, dc; static bool init;
				if (!init) {
					uint8_t l[288];
					int s = 0;
					for (; s < 144; s++) l[s] = 8;
					for (; s < 256; s++) l[s] = 9;
					for (; s < 280; s++) l[s] = 7;
					for (; s < 288; s++) l[s] = 8;
					construct(lc, l, 288);
					for (s = 0; s < 30; s++) l[s] = 5;
					construct(dc, l, 30);
					init = true;
				}
				if (!codes(lc, dc)) return false;
			} else if (type == 2) {
				static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
				uint8_t lengths[320];
				int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
				if (err || nlen > 286 || ndist > 30) return false;
				int i = 0;
				for (; i < ncode; i++) lengths[order[i]] = (uint8_t)bits(3);
				for (; i < 19; i++) lengths[order[i]] = 0;
				Huff lc, dc;
				if (construct(lc, lengths, 19) != 0) return false;
				i = 0;
				while (i < nlen + ndist) {
					int sym = decode(lc);
					if (sym < 0 || err) return false;
					if (sym < 16) lengths[i++] = (uint8_t)sym;
					else {
						int len = 0, rep;
						if (sym == 16) { if (i == 0) return false; len = lengths[i - 1]; rep = 3 + bits(2); }
						else if (sym == 17) rep = 3 + bits(3);
						else rep = 11 + bits(7);
						if (i + rep > nlen + ndist) return false;
						while (rep--) lengths[i++] = (uint8_t)len;
					}
				}
				if (lengths[256] == 0) return false;
				if (construct(lc, lengths, nlen) < 0) return false;
				if (construct(dc, lengths + nlen, ndist) < 0) return false;
				if (!codes(lc, dc)) return false;
			} else return false;
		} while (!last);
		return true;
	}
};

struct RomFile { std::string name; std::vector<uint8_t> data; uint32_t crc; };

static bool read_all(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return false; }
	out.resize((size_t)n);
	bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	return ok;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16le(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void add_zip(const std::string &path, std::vector<RomFile> &out)
{
	std::vector<uint8_t> z;
	if (!read_all(path, z) || z.size() < 22) { fprintf(stderr, "cannot read %s\n", path.c_str()); return; }
	size_t eocd = std::string::npos;
	for (size_t i = z.size() - 22 + 1; i-- > 0;) {
		if (rd32(&z[i]) == 0x06054b50u) { eocd = i; break; }
		if (z.size() - i > 70000) break;
	}
	if (eocd == std::string::npos) { fprintf(stderr, "%s: not a zip file\n", path.c_str()); return; }
	unsigned n = rd16le(&z[eocd + 10]);
	size_t cd = rd32(&z[eocd + 16]);
	for (unsigned e = 0; e < n; e++) {
		if (cd + 46 > z.size() || rd32(&z[cd]) != 0x02014b50u) break;
		unsigned method = rd16le(&z[cd + 10]);
		uint32_t crc = rd32(&z[cd + 16]), csize = rd32(&z[cd + 20]), usize = rd32(&z[cd + 24]);
		unsigned nl = rd16le(&z[cd + 28]), xl = rd16le(&z[cd + 30]), cl = rd16le(&z[cd + 32]);
		size_t lho = rd32(&z[cd + 42]);
		std::string name((const char *)&z[cd + 46], nl);
		cd += 46 + nl + xl + cl;
		if (usize == 0 || usize > (4u << 20) || lho + 30 > z.size() || rd32(&z[lho]) != 0x04034b50u) continue;
		size_t ds = lho + 30 + rd16le(&z[lho + 26]) + rd16le(&z[lho + 28]);
		if (ds + csize > z.size()) continue;
		RomFile rf; rf.name = name; rf.data.resize(usize);
		if (method == 0 && csize == usize) memcpy(rf.data.data(), &z[ds], usize);
		else if (method == 8) {
			Inflater inf; inf.in = &z[ds]; inf.inlen = csize; inf.out = rf.data.data(); inf.outlen = usize;
			if (!inf.run() || inf.outcnt != usize) { fprintf(stderr, "%s: cannot unpack %s\n", path.c_str(), name.c_str()); continue; }
		} else continue;
		rf.crc = crc32(rf.data.data(), rf.data.size());
		size_t sl = rf.name.find_last_of("/\\");
		if (sl != std::string::npos) rf.name = rf.name.substr(sl + 1);
		out.push_back(std::move(rf));
	}
}

static bool ends_with_nocase(const std::string &s, const char *suf)
{
	size_t n = strlen(suf);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
	return true;
}

static void add_path(const std::string &path, std::vector<RomFile> &out)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (fs::is_directory(path, ec)) {
		for (auto &de : fs::directory_iterator(path, ec)) {
			if (!de.is_regular_file(ec)) continue;
			std::string p = de.path().string();
			if (ends_with_nocase(p, ".zip")) add_zip(p, out);
			else if (de.file_size(ec) <= (1u << 20)) {
				RomFile rf; rf.name = de.path().filename().string();
				if (read_all(p, rf.data) && !rf.data.empty()) { rf.crc = crc32(rf.data.data(), rf.data.size()); out.push_back(std::move(rf)); }
			}
		}
	} else if (ends_with_nocase(path, ".zip")) {
		add_zip(path, out);
	} else {
		RomFile rf; rf.name = fs::path(path).filename().string();
		if (read_all(path, rf.data) && !rf.data.empty()) { rf.crc = crc32(rf.data.data(), rf.data.size()); out.push_back(std::move(rf)); }
	}
}

/* ===================================================================== */
/*  ROM set description (MAME "congo" rev C and "tiptop")                */
/* ===================================================================== */
struct RomSpec {
	const char *what;
	uint32_t crc[2];
	const char *name[3];
	size_t size;
};

enum { R_M1, R_M2, R_M3, R_M4, R_SND, R_TX, R_BG0, R_BG1, R_BG2,
       R_SP0, R_SP1, R_SP2, R_SP3, R_SP4, R_SP5, R_TM0, R_TM1, R_PROM, R_COUNT };

static const RomSpec SPECS[R_COUNT] = {
	{ "main CPU 1",   { 0x09355b5b, 0xe19dc77b }, { "congo_rev_c_rom1.u21",  "tiptop1.u35", nullptr }, 0x2000 },
	{ "main CPU 2",   { 0x1c5e30ae, 0x3fcd3b6e }, { "congo_rev_c_rom2a.u22", "tiptop2.u34", nullptr }, 0x2000 },
	{ "main CPU 3",   { 0x5ee1132c, 0x1c94250b }, { "congo_rev_c_rom3.u23",  "tiptop3.u33", nullptr }, 0x2000 },
	{ "main CPU 4",   { 0x5332b9bf, 0x577b501b }, { "congo_rev_c_rom4.u24",  "tiptop4.u32", nullptr }, 0x2000 },
	{ "sound CPU",    { 0x5024e673, 0 },          { "tip_top_rom_17.u19", "tip_top_rom_17.u11", nullptr }, 0x2000 },
	{ "characters",   { 0x7bf6ba2b, 0 },          { "tip_top_rom_5.u76",  nullptr, nullptr }, 0x1000 },
	{ "background 1", { 0xdb99a619, 0 },          { "tip_top_rom_8.u93",  nullptr, nullptr }, 0x2000 },
	{ "background 2", { 0x93e2309e, 0 },          { "tip_top_rom_9.u94",  nullptr, nullptr }, 0x2000 },
	{ "background 3", { 0xf27a9407, 0 },          { "tip_top_rom_10.u95", nullptr, nullptr }, 0x2000 },
	{ "sprites 1",    { 0x15e3377a, 0 },          { "tip_top_rom_12.u78", nullptr, nullptr }, 0x2000 },
	{ "sprites 2",    { 0x1d1321c8, 0 },          { "tip_top_rom_13.u79", nullptr, nullptr }, 0x2000 },
	{ "sprites 3",    { 0x73e2709f, 0 },          { "tip_top_rom_11.u77", nullptr, nullptr }, 0x2000 },
	{ "sprites 4",    { 0xbf9169fe, 0 },          { "tip_top_rom_14.u104", nullptr, nullptr }, 0x2000 },
	{ "sprites 5",    { 0xcb6d5775, 0 },          { "tip_top_rom_16.u106", nullptr, nullptr }, 0x2000 },
	{ "sprites 6",    { 0x7b15a7a4, 0 },          { "tip_top_rom_15.u105", nullptr, nullptr }, 0x2000 },
	{ "tilemap 1",    { 0xd637f02b, 0 },          { "tip_top_rom_6.u57",  nullptr, nullptr }, 0x2000 },
	{ "tilemap 2",    { 0x80927943, 0 },          { "tip_top_rom_7.u58",  nullptr, nullptr }, 0x2000 },
	{ "color PROM",   { 0xb788d8ae, 0x56b9f1ba }, { "mr019.u87", "mr018.u68", nullptr }, 0x100 },
};

struct RomSet { std::vector<uint8_t> d[R_COUNT]; };

static const RomFile *find_rom(const std::vector<RomFile> &files, const RomSpec &s, bool &crc_ok)
{
	for (auto &f : files)
		for (int k = 0; k < 2; k++)
			if (s.crc[k] && f.crc == s.crc[k]) { crc_ok = true; return &f; }
	for (auto &f : files)
		for (int k = 0; k < 3; k++)
			if (s.name[k] && ends_with_nocase(f.name, s.name[k]) && f.name.size() == strlen(s.name[k]) && f.data.size() >= s.size) { crc_ok = false; return &f; }
	return nullptr;
}

static bool load_roms(const std::vector<RomFile> &files, RomSet &rs, bool sound_only)
{
	bool ok = true;
	for (int i = 0; i < R_COUNT; i++) {
		if (sound_only && i != R_SND) continue;
		bool crc_ok = false;
		const RomFile *f = find_rom(files, SPECS[i], crc_ok);
		if (!f) {
			fprintf(stderr, "missing ROM: %-13s (%s, CRC %08x)\n", SPECS[i].what, SPECS[i].name[0], SPECS[i].crc[0]);
			ok = false;
			continue;
		}
		if (!crc_ok) fprintf(stderr, "warning: %s found by name only (CRC %08x differs); using it anyway\n", f->name.c_str(), f->crc);
		rs.d[i] = f->data;
		if (rs.d[i].size() < SPECS[i].size) { fprintf(stderr, "ROM %s is too short\n", f->name.c_str()); ok = false; }
	}
	return ok;
}

/* ===================================================================== */
/*  SN76489A (cycle-based, modelled after MAME's sn76496 as used by the  */
/*  earlier sound-board proof of concept)                                */
/* ===================================================================== */
struct Psg {
	/* The chip output is rendered at OS x the output rate (exact area integration of the square waves)
	 * and then low-pass filtered / decimated with a long windowed-sinc FIR.  Without this the upper
	 * harmonics of the square waves and the noise fold back below 24 kHz as a gritty, "static"-like hash. */
	static constexpr int OS = 8;                 /* oversampling factor */
	static constexpr int HALF = 160;             /* FIR half length in fine bins (320 taps) */
	static constexpr int LAT = HALF / OS + 1;    /* output latency, in output samples */
	static constexpr int RING = 16384, MASK = RING - 1;
	int period[4], count[4], output[4];
	int reg[8], last_reg;
	unsigned rng;
	double vol[4], voltab[16];
	int cur_clock;
	uint64_t next_tick;
	int tick_cycles;
	float ring[RING];
	float fir[2 * HALF];

	static constexpr unsigned FB_MASK = 0x10000;

	static double bessel_i0(double x)
	{
		double sum = 1, term = 1;
		for (int k = 1; k < 40; k++) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
		return sum;
	}

	void init(double clock)
	{
		memset(this, 0, sizeof *this);
		double o = 0.25;
		for (int i = 0; i < 15; i++) { voltab[i] = o; o /= 1.258925412; }
		voltab[15] = 0;
		/* power-on: all channels silent (a real chip is undefined here; the ROM programs it a moment later) */
		for (int i = 0; i < 4; i++) { period[i] = (i == 3) ? 0 : 0x400; vol[i] = voltab[15]; }
		rng = FB_MASK; output[3] = rng & 1;
		cur_clock = 7;
		tick_cycles = (int)(16.0 * SND_HZ / clock + 0.5);
		next_tick = tick_cycles;

		/* Kaiser-windowed sinc, cutoff 21.5 kHz at the oversampled rate, unity gain at DC */
		const double fc = 21500.0 / (RATE * OS), beta = 8.0;
		double sum = 0;
		for (int j = 0; j < 2 * HALF; j++) {
			double t = j - (HALF - 0.5);
			double sinc = (std::fabs(t) < 1e-9) ? 2 * fc : std::sin(2 * M_PI_ * fc * t) / (M_PI_ * t);
			double r = t / HALF;
			double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel_i0(beta);
			fir[j] = (float)(sinc * w); sum += sinc * w;
		}
		for (int j = 0; j < 2 * HALF; j++) fir[j] = (float)(fir[j] / sum);
	}

	static constexpr double M_PI_ = 3.14159265358979323846;

	void add_segment(double level, uint64_t t0, uint64_t t1)
	{
		if (level == 0 || t1 <= t0) return;
		const double BINF = BIN / OS;
		double a = t0 / BINF, b = t1 / BINF;
		long ia = (long)a, ib = (long)b;
		if (ia == ib) { ring[ia & MASK] += (float)(level * (b - a)); return; }
		ring[ia & MASK] += (float)(level * (ia + 1 - a));
		for (long i = ia + 1; i < ib; i++) ring[i & MASK] += (float)level;
		if (b > ib) ring[ib & MASK] += (float)(level * (b - ib));
	}

	double level() const
	{
		double o = 0;
		for (int i = 0; i < 4; i++) if (output[i]) o += vol[i];
		return o;
	}

	void chip_step()
	{
		for (int i = 0; i < 3; i++)
			if (--count[i] <= 0) { output[i] ^= 1; count[i] = period[i]; }
		if (--count[3] <= 0) {
			int t1 = (rng & 0x04) != 0;
			int t2 = ((rng & 0x08) != 0) && ((reg[6] >> 2) & 1);
			if (t1 != t2) { rng >>= 1; rng |= FB_MASK; } else rng >>= 1;
			output[3] = rng & 1;
			count[3] = period[3];
		}
	}

	void run(uint64_t now)
	{
		while (next_tick <= now) {
			uint64_t t0 = next_tick - tick_cycles;
			add_segment(level(), t0, next_tick);
			chip_step();
			next_tick += tick_cycles;
		}
	}

	void write(uint64_t now, uint8_t data)
	{
		run(now);
		int r;
		if (data & 0x80) { r = (data & 0x70) >> 4; last_reg = r; reg[r] = (reg[r] & 0x3f0) | (data & 0x0f); }
		else r = last_reg;
		int c = r >> 1;
		switch (r) {
		case 0: case 2: case 4:
			if (!(data & 0x80)) reg[r] = (reg[r] & 0x0f) | ((data & 0x3f) << 4);
			period[c] = reg[r] ? reg[r] : 0x400;
			if (r == 4 && (reg[6] & 3) == 3) period[3] = period[2] << 1;
			break;
		case 1: case 3: case 5: case 7:
			vol[c] = voltab[data & 0x0f];
			if (!(data & 0x80)) reg[r] = (reg[r] & 0x3f0) | (data & 0x0f);
			break;
		case 6: {
			if (!(data & 0x80)) reg[6] = (reg[6] & 0x3f0) | (data & 0x0f);
			int n = reg[6];
			period[3] = ((n & 3) == 3) ? (period[2] << 1) : (1 << (5 + (n & 3)));
			rng = FB_MASK;
			break; }
		}
	}

	/* output sample n (needs the chip to have run up to (n + LAT) output samples) */
	float take(long n)
	{
		const long start = n * OS + OS / 2 - HALF;
		double sum = 0;
		for (int j = 0; j < 2 * HALF; j++) sum += fir[j] * ring[(start + j) & MASK];
		for (int i = 0; i < OS; i++) ring[(start + i) & MASK] = 0;
		return (float)sum;
	}
};

/* 8255 on the sound board: PA = command latch in, PB1 = gorilla trigger,
 * PB7 = output mute, PC0..3 = bass / conga L / conga H / rim triggers */
struct Ppi {
	uint8_t out_a, out_b, out_c;
	int a_in, b_in, cu_in, cl_in;
	uint8_t latch;
	uint8_t pin_b() const { return b_in ? 0xff : out_b; }
	uint8_t pin_c() const
	{
		uint8_t hi = cu_in ? 0xf0 : (out_c & 0xf0);
		uint8_t lo = cl_in ? 0x0f : (out_c & 0x0f);
		return hi | lo;
	}
};

/* ===================================================================== */
/*  The machine                                                          */
/* ===================================================================== */
struct Machine {
	Z80 main, snd;
	bool sound_only = false;

	/* memory */
	uint8_t mrom[0x8000], mram[0x1000], vram[0x400], cram[0x400], spr[0x100];
	uint8_t srom[0x2000], sram[0x800];

	/* decoded graphics */
	uint8_t txpix[256 * 64];
	uint8_t sppix[128 * 1024];
	std::vector<uint16_t> bgpix;          /* 256 x 4096 pens (color * 8 + pixel) */
	uint32_t pal[512];

	/* main-board latches and state */
	uint8_t l0 = 0, l1 = 0;
	bool int_en = false, irq_line = false;
	int coin_status[3] = {0, 0, 0};
	bool bg_enable = false, flip = false;
	int bg_color = 0, bg_pos = 0, fg_color = 0, fg_bank = 0, color_bank = 0;
	uint8_t custom[4] = {0, 0, 0, 0};
	uint8_t soundlatch = 0;

	/* inputs (active high) */
	uint8_t sw00 = 0, sw01 = 0, sw100 = 0;
	uint8_t dsw02 = 0x77;   /* bonus 10000, medium, 3 lives, sound on, upright */
	uint8_t dsw03 = 0x33;   /* 1 coin / 1 credit */

	/* sound board */
	Psg psg1, psg2;
	Ppi ppi;
	congo_perc::synth perc;
	congo_perc::gorilla gorilla;
	bool mute = true, armed = false;
	uint8_t last_pb = 0xff, last_pc = 0xff;
	uint64_t next_snd_irq = 16384;
	double dc_x = 0, dc_y = 0;
	float volume = 1.0f;
	double vol_psg = 1.0, vol_perc = 1.0, vol_gor = 1.0, vol_bass = 1.0;   /* per-kind mixer levels (options menu) */
	/* "soft bass edge": the bass drum model starts with a squared-off 3 ms plateau and 0.45 V steps that
	 * click on speakers; a 2-pole 200 Hz low-pass rounds the corners and keeps the 73 Hz thump */
	bool bass_soft = true;
	double bs_b0 = 0, bs_b1 = 0, bs_b2 = 0, bs_a1 = 0, bs_a2 = 0, bs_z1 = 0, bs_z2 = 0;
	void set_bass_filter()
	{
		const double w0 = 2 * 3.14159265358979323846 * 200.0 / RATE, c = std::cos(w0), al = std::sin(w0) / (2 * 0.70710678), a0 = 1 + al;
		bs_b0 = (1 - c) / 2 / a0; bs_b1 = (1 - c) / a0; bs_b2 = bs_b0; bs_a1 = -2 * c / a0; bs_a2 = (1 - al) / a0;
		bs_z1 = bs_z2 = 0;
	}
	/* optional output low-pass ("speaker roll-off"): 2-pole Butterworth */
	double lp_b0 = 1, lp_b1 = 0, lp_b2 = 0, lp_a1 = 0, lp_a2 = 0, lp_z1 = 0, lp_z2 = 0;
	bool lp_on = false;
	void set_treble(int mode)
	{
		static const double fc[] = { 0, 12000, 8000, 5000, 3000 };
		lp_on = mode > 0 && mode < 5;
		lp_z1 = lp_z2 = 0;
		if (!lp_on) return;
		const double w0 = 2 * 3.14159265358979323846 * fc[mode] / RATE, c = std::cos(w0), al = std::sin(w0) / (2 * 0.70710678);
		const double a0 = 1 + al;
		lp_b0 = (1 - c) / 2 / a0; lp_b1 = (1 - c) / a0; lp_b2 = (1 - c) / 2 / a0;
		lp_a1 = -2 * c / a0; lp_a2 = (1 - al) / a0;
	}

	/* timing */
	double main_target = 0, snd_target = 0;
	long sample_no = 0;
	int frame_cyc = 0;
	bool vb_done = false;
	bool frame_done = false;
	int sound_only_samples = 0;

	/* output */
	uint32_t fb[SCR_W * SCR_H];
	std::vector<int16_t> audio;
	long frames = 0;
	std::vector<uint16_t> watch;      /* dev: log main-RAM writes to these addresses */

	/* ------------------------------------------------------------ */
	void decode_gfx(const RomSet &rs)
	{
		const uint8_t *tx = rs.d[R_TX].data();
		for (int c = 0; c < 256; c++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) {
					int b0 = (tx[c * 8 + y] >> (7 - x)) & 1;
					int b1 = (tx[0x800 + c * 8 + y] >> (7 - x)) & 1;
					txpix[c * 64 + y * 8 + x] = (uint8_t)(b1 << 1 | b0);   /* MAME gfx_8x8x2_planar: second half is the MSB */
				}

		uint8_t bgrom[0x6000];
		for (int i = 0; i < 3; i++) memcpy(bgrom + i * 0x2000, rs.d[R_BG0 + i].data(), 0x2000);
		static uint8_t bgtile[1024 * 64];
		for (int c = 0; c < 1024; c++)
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) {
					int b0 = (bgrom[c * 8 + y] >> (7 - x)) & 1;
					int b1 = (bgrom[0x2000 + c * 8 + y] >> (7 - x)) & 1;
					int b2 = (bgrom[0x4000 + c * 8 + y] >> (7 - x)) & 1;
					bgtile[c * 64 + y * 8 + x] = (uint8_t)(b2 << 2 | b1 << 1 | b0);   /* MAME gfx_8x8x3_planar: last third is the MSB */
				}

		uint8_t sprom[0xc000];
		for (int i = 0; i < 6; i++) memcpy(sprom + i * 0x2000, rs.d[R_SP0 + i].data(), 0x2000);
		for (int c = 0; c < 128; c++)
			for (int y = 0; y < 32; y++)
				for (int x = 0; x < 32; x++) {
					int byte = c * 128 + (x >> 3) * 8 + (y & 7) + (y >> 3) * 32;
					int bit = 7 - (x & 7);
					int p2 = (sprom[0x8000 + byte] >> bit) & 1;
					int p1 = (sprom[0x4000 + byte] >> bit) & 1;
					int p0 = (sprom[byte] >> bit) & 1;
					sppix[c * 1024 + y * 32 + x] = (uint8_t)(p2 << 2 | p1 << 1 | p0);
				}

		/* background pixmap: 32 x 512 tiles of 8x8 (only the first 256 rows are distinct) */
		uint8_t tmap[0x4000];
		memcpy(tmap, rs.d[R_TM0].data(), 0x2000);
		memcpy(tmap + 0x2000, rs.d[R_TM1].data(), 0x2000);
		bgpix.assign(256 * 4096, 0);
		for (int ti = 0; ti < 32 * 512; ti++) {
			int row = ti / 32, col = ti % 32;
			int eff = ti & 0x1fff;
			int code = tmap[eff] + 256 * (tmap[eff + 0x2000] & 3);
			int color = tmap[eff + 0x2000] >> 4;
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					bgpix[(row * 8 + y) * 256 + col * 8 + x] = (uint16_t)(color * 8 + bgtile[code * 64 + y * 8 + x]);
		}

		/* palette: resistor weights 1000/470/220 ohms (R,G: 3 bits, B: 2 bits) with 470 ohm pulldown */
		const std::vector<uint8_t> &pr = rs.d[R_PROM];
		auto weights = [](const double *r, int n, double *w, double scale) {
			double sum = 1.0 / 470.0;                 /* 470 ohm pulldown */
			for (int i = 0; i < n; i++) sum += 1.0 / r[i];
			for (int i = 0; i < n; i++) w[i] = scale * (1.0 / r[i]) / sum;
		};
		const double rr[3] = { 1000, 470, 220 }, rb[2] = { 470, 220 };
		/* full-scale (all bits on) of the 3-bit channels maps to 255; blue ends up slightly lower */
		double full = 0; { double sum = 1.0 / 470.0, on = 0; for (double r : rr) { sum += 1.0 / r; on += 1.0 / r; } full = on / sum; }
		double rw[3], gw[3], bw[2];
		weights(rr, 3, rw, 255.0 / full); weights(rr, 3, gw, 255.0 / full); weights(rb, 2, bw, 255.0 / full);
		for (int i = 0; i < 512; i++) {
			int v = pr[(pr.size() >= 0x200) ? i : (i & 0xff)];
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
		memset(mram, 0, sizeof mram); memset(vram, 0, sizeof vram); memset(cram, 0, sizeof cram);
		memset(spr, 0, sizeof spr); memset(sram, 0, sizeof sram);
		l0 = l1 = 0; int_en = irq_line = false;
		coin_status[0] = coin_status[1] = coin_status[2] = 0;
		bg_enable = flip = false; bg_color = bg_pos = fg_color = fg_bank = color_bank = 0;
		memset(custom, 0, sizeof custom); soundlatch = 0;
		main.ud = this; main.rd = main_rd; main.wr = main_wr; main.in = io_rd; main.out = io_wr;
		z80_reset(&main);
		snd.ud = this; snd.rd = snd_rd; snd.wr = snd_wr; snd.in = io_rd; snd.out = io_wr;
		z80_reset(&snd);
		psg1.init(4000000.0); psg2.init(1000000.0);
		memset(&ppi, 0, sizeof ppi);
		ppi.a_in = ppi.b_in = ppi.cu_in = ppi.cl_in = 1;
		perc.set_rate(RATE, 1.0 / 2.216); perc.reset(); set_bass_filter();
		gorilla.reset();
		mute = true; armed = false; last_pb = last_pc = 0xff;
		next_snd_irq = 16384; dc_x = dc_y = 0;
		main_target = snd_target = 0; sample_no = 0; frame_cyc = 0; vb_done = false; frame_done = false;
		sound_only_samples = 0;
	}

	/* ------------------------------------------------------------ */
	/*  main CPU bus                                                  */
	/* ------------------------------------------------------------ */
	static uint8_t io_rd(void *, uint16_t) { return 0xff; }
	static void io_wr(void *, uint16_t, uint8_t) {}

	static uint8_t main_rd(void *ud, uint16_t a)
	{
		Machine *m = (Machine *)ud;
		if (a < 0x8000) return m->mrom[a];
		if (a < 0x9000) return m->lockon[a & 0xfff] ? m->lockv[a & 0xfff] : m->mram[a & 0xfff];
		if ((a & 0xe000) == 0xa000) {
			int o = a & ~0x1800;
			if (o < 0xa400) return m->vram[a & 0x3ff];
			if (o < 0xa800) return m->cram[a & 0x3ff];
			return 0;
		}
		if ((a & 0xe000) == 0xc000) {
			switch (a & 0x38) {
			case 0x00:
				switch (a & 3) {
				case 0: return m->sw00;
				case 1: return m->sw01;
				case 2: return m->dsw02;
				default: return m->dsw03;
				}
			case 0x08:
				return (uint8_t)(m->sw100 | (m->coin_status[0] << 5) | (m->coin_status[1] << 6) | (m->coin_status[2] << 7));
			}
		}
		return 0;
	}

	void latch_write(int which, int bit, int val)
	{
		uint8_t &L = which ? l1 : l0;
		if (val) L |= (uint8_t)(1 << bit); else L &= (uint8_t)~(1 << bit);
		if (which == 0) {
			switch (bit) {
			case 0: case 1: case 2:
				for (int n = 0; n < 3; n++) if (!((l0 >> n) & 1)) coin_status[n] = 0;
				break;
			case 5: bg_enable = val; break;
			case 6: flip = !val; break;
			case 7: int_en = val; if (!val) irq_line = false; break;
			}
		} else {
			switch (bit) {
			case 1: fg_color = val * 0x80; break;
			case 3: bg_color = val * 0x80; break;
			case 6: fg_bank = val; break;
			case 7: color_bank = val; break;
			}
		}
	}

	static void main_wr(void *ud, uint16_t a, uint8_t v)
	{
		Machine *m = (Machine *)ud;
		if (a >= 0x8000 && a < 0x9000) {
			if (!m->watch.empty()) for (uint16_t w : m->watch) if (w == a && m->mram[a & 0xfff] != v)
				fprintf(stderr, "W f=%ld pc=%04X %04X: %02X -> %02X\n", m->frames, (unsigned)m->main.pc.w, a, m->mram[a & 0xfff], v);
			m->mram[a & 0xfff] = m->lockon[a & 0xfff] ? m->lockv[a & 0xfff] : v; return; }
		if ((a & 0xe000) == 0xa000) {
			int o = a & ~0x1800;
			if (!m->watch.empty() && o < 0xa400) for (uint16_t w : m->watch) if (w == (a & 0xa3ff)) fprintf(stderr, "V f=%ld pc=%04X %04X: %02X -> %02X\n", m->frames, (unsigned)m->main.pc.w, a, m->vram[a & 0x3ff], v);
			if (o < 0xa400) m->vram[a & 0x3ff] = v;
			else if (o < 0xa800) m->cram[a & 0x3ff] = v;
			return;
		}
		if ((a & 0xe000) != 0xc000) return;
		switch (a & 0x38) {
		case 0x18: m->latch_write(0, a & 7, v & 1); break;
		case 0x20: m->latch_write(1, a & 7, v & 1); break;
		case 0x28:
			if ((a & 3) == 0) m->bg_pos = (m->bg_pos & 0x700) | v;
			else if ((a & 3) == 1) m->bg_pos = (m->bg_pos & 0x0ff) | ((v << 8) & 0x700);
			break;
		case 0x30: {
			m->custom[a & 3] = v;
			if ((a & 3) == 3 && v == 0x01) {
				/* custom sprite DMA: copies 4 bytes per sprite from main RAM into sprite RAM */
				uint16_t saddr = (uint16_t)(m->custom[0] | (m->custom[1] << 8));
				int count = m->custom[2];
				m->main.cycles += (uint64_t)count * 5;
				while (count-- >= 0) {
					uint8_t daddr = (uint8_t)(main_rd(m, saddr) * 4);
					m->spr[(daddr + 0) & 0xff] = main_rd(m, (uint16_t)(saddr + 1));
					m->spr[(daddr + 1) & 0xff] = main_rd(m, (uint16_t)(saddr + 2));
					m->spr[(daddr + 2) & 0xff] = main_rd(m, (uint16_t)(saddr + 3));
					m->spr[(daddr + 3) & 0xff] = main_rd(m, (uint16_t)(saddr + 4));
					saddr = (uint16_t)(saddr + 0x20);
				}
			}
			break; }
		case 0x38: m->soundlatch = v; m->ppi.latch = v; break;
		}
	}

	/* ------------------------------------------------------------ */
	/*  sound CPU bus                                                 */
	/* ------------------------------------------------------------ */
	static uint8_t snd_rd(void *ud, uint16_t a)
	{
		Machine *m = (Machine *)ud;
		if (a < 0x2000) return m->srom[a];
		if (a >= 0x4000 && a < 0x6000) return m->sram[a & 0x7ff];
		if (a >= 0x8000 && a < 0xa000) {
			Ppi *p = &m->ppi;
			switch (a & 3) {
			case 0: return p->a_in ? p->latch : p->out_a;
			case 1: return p->pin_b();
			case 2: return p->pin_c();
			default: return 0xff;
			}
		}
		return 0xff;
	}

	void ppi_pins_changed()
	{
		uint8_t b = ppi.pin_b(), c = ppi.pin_c();
		uint8_t db = b ^ last_pb, dc = c ^ last_pc;
		last_pb = b; last_pc = c;
		if (db & 0x80) { mute = (b & 0x80) != 0; if (mute) armed = true; }
		/* the 8255 clears its outputs when the ROM programs it; ignore the resulting power-on edges */
		if (!armed) return;
		if ((db & 0x02) && !(b & 0x02)) gorilla.trigger();
		/* the drum circuits are pinged by the rising edge at the end of the low pulse */
		uint8_t rising = dc & c & 0x0f;
		for (int v = 0; v < 4; v++) if ((rising >> v) & 1) perc.trigger(v);
	}

	static void snd_wr(void *ud, uint16_t a, uint8_t v)
	{
		Machine *m = (Machine *)ud;
		uint64_t now = m->snd.cycles;
		if (a >= 0x4000 && a < 0x6000) { m->sram[a & 0x7ff] = v; return; }
		if (a >= 0x6000 && a < 0x8000) { m->psg1.write(now, v); return; }
		if (a >= 0xa000 && a < 0xc000) { m->psg2.write(now, v); return; }
		if (a >= 0x8000 && a < 0xa000) {
			Ppi *p = &m->ppi;
			switch (a & 3) {
			case 0: if (!p->a_in) p->out_a = v; break;
			case 1: p->out_b = v; break;
			case 2: p->out_c = v; break;
			default:
				if (v & 0x80) {
					p->a_in = (v >> 4) & 1; p->cu_in = (v >> 3) & 1;
					p->b_in = (v >> 1) & 1; p->cl_in = v & 1;
					p->out_a = p->out_b = p->out_c = 0;
				} else {
					int bit = (v >> 1) & 7;
					if (v & 1) p->out_c |= (uint8_t)(1 << bit); else p->out_c &= (uint8_t)~(1 << bit);
				}
			}
			m->ppi_pins_changed();
		}
	}

	/* ------------------------------------------------------------ */
	/*  video                                                         */
	/* ------------------------------------------------------------ */
	static int find_minimum_y(uint8_t value, int flip_)
	{
		int flipmask = flip_ ? 0xff : 0x00;
		int flipconst = flip_ ? 0xef : 0xf1;
		int y;
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
			const int colorbase = bg_color + (color_bank << 8);
			const int flipoffs = 0x3f;
			for (int y = 16; y < 16 + SCR_H; y++) {
				uint16_t *dst = &pen[(y - 16) * SCR_W];
				int vf = y ^ flipmask;
				int srcy = vf + ((bg_pos << 1) ^ 0xfff) + 1;
				const uint16_t *src = &bgpix[(srcy & 4095) * 256];
				int add = ((vf >> 1) ^ 0xff) + 1 + flipoffs;
				for (int x = 0; x < SCR_W; x++) {
					int srcx = (x ^ flipmask) + add;
					dst[x] = (uint16_t)(src[srcx & 255] + colorbase);
				}
			}
		} else {
			/* black */
			for (int i = 0; i < SCR_W * SCR_H; i++) pen[i] = 0xffff;
		}

		/* 2. sprites (only the lower 32 entries of sprite RAM are used) */
		for (int offs = 0x7c; offs >= 0; offs -= 4) {
			int sy = find_minimum_y(spr[offs], flip);
			int flipy = ((spr[offs + 1] ^ flipmask) & 0x80) != 0;
			int flipx = ((spr[offs + 2] ^ flipmask) & 0x80) != 0;
			int code = spr[offs + 1] & 0x7f;
			int color = (spr[offs + 2] & 0x1f) + (color_bank << 5);
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

		/* 3. 32x32 character layer (pen 0 transparent) */
		const int pbase = fg_color + (color_bank << 8);
		for (int y = 16; y < 16 + SCR_H; y++) {
			uint16_t *dst = &pen[(y - 16) * SCR_W];
			int ty = flip ? 255 - y : y;
			for (int x = 0; x < SCR_W; x++) {
				int tx = flip ? 255 - x : x;
				int idx = (ty >> 3) * 32 + (tx >> 3);
				int code = (vram[idx] + (fg_bank << 8)) & 0xff;
				int p = txpix[code * 64 + (ty & 7) * 8 + (tx & 7)];
				if (p) dst[x] = (uint16_t)((cram[idx] & 0x1f) * 8 + p + pbase);
			}
		}

		for (int i = 0; i < SCR_W * SCR_H; i++)
			fb[i] = pen[i] == 0xffff ? 0xff000000u : pal[pen[i] & 511];
	}

	/* ------------------------------------------------------------ */
	/*  run one output sample (1/48000 s) of both boards              */
	/* ------------------------------------------------------------ */
	void step_sample()
	{
		if (!sound_only) {
			main_target += MAIN_HZ / RATE;
			while ((double)main.cycles < main_target) {
				main.irq = irq_line;
				uint64_t c0 = main.cycles;
				z80_step(&main);
				frame_cyc += (int)(main.cycles - c0);
				/* a requested scene change is made at the instant the vblank interrupt starts: the game is then between
				 * two frames (nothing half-updated, nothing on the stack), whatever it was busy with */
				if (skip_pending && main.pc.w == 0x0038) { skip_scene(skip_pending); skip_pending = 0; }
				if (!vb_done && frame_cyc >= VBLANK_AT) {
					vb_done = true;
					if (int_en) irq_line = true;
					render();
				}
				if (frame_cyc >= FRAME_CYC) {
					frame_cyc -= FRAME_CYC; vb_done = false; frame_done = true; frames++;
				}
			}
		} else if (++sound_only_samples >= 800) {
			sound_only_samples = 0; frame_done = true; frames++;
		}

		snd_target += BIN;
		while ((double)snd.cycles < snd_target) {
			if (snd.cycles >= next_snd_irq) { snd.irq = 1; next_snd_irq += 16384; }
			z80_step(&snd);
		}
		uint64_t t_end = (uint64_t)snd_target;
		psg1.run(t_end); psg2.run(t_end);

		/* one sample of latency so the PSG integrators are complete */
		double x = 0;
		if (sample_no >= Psg::LAT) x = psg1.take(sample_no - Psg::LAT) + psg2.take(sample_no - Psg::LAT);
		x *= vol_psg;
		double pv[congo_perc::synth::NUM_VOICES];
		perc.tick_voices(pv);
		double bass = pv[congo_perc::synth::BASS];
		if (bass_soft) {
			double z = bs_b0 * bass + bs_z1;
			bs_z1 = bs_b1 * bass - bs_a1 * z + bs_z2;
			bs_z2 = bs_b2 * bass - bs_a2 * z;
			bass = z;
		}
		double p = (bass * vol_bass + pv[1] + pv[2] + pv[3]) * vol_perc + gorilla.tick() * (1.0 / 2.216) * vol_gor;
		x += p;
		if (mute) x = 0;       /* SOU MUTE silences the whole output until the sound ROM gets command 0x30 */
		sample_no++;

		/* AC coupling, gain, soft clip */
		double y = x - dc_x + 0.9995 * dc_y;
		dc_x = x; dc_y = y;
		double o = y * volume * 1.4;
		if (lp_on) {
			double z = lp_b0 * o + lp_z1;
			lp_z1 = lp_b1 * o - lp_a1 * z + lp_z2;
			lp_z2 = lp_b2 * o - lp_a2 * z;
			o = z;
		}
		o = std::tanh(o);
		audio.push_back((int16_t)std::lrint(o * 32000.0));
	}

	/* cheats: (main RAM address 0x8000-0x8FFF, value) pairs written back at the end of every frame */
	std::vector<std::pair<uint16_t, uint8_t>> pokes;
	/* cheats freeze a RAM byte at the bus: reads return the locked value and writes cannot change it, so the
	 * game never sees another value in the middle of a frame (a poke once per frame would miss that) */
	uint8_t lockon[0x1000] = {}, lockv[0x1000] = {};
	/* cheats may also patch the program ROM (addresses 0000-7FFF, like a Game Genie): the original bytes are kept
	 * and restored whenever the cheat is switched off */
	uint8_t mrom_orig[0x8000]; bool mrom_saved = false;
	void sync_locks()
	{
		if (!mrom_saved) { memcpy(mrom_orig, mrom, sizeof mrom); mrom_saved = true; }
		memcpy(mrom, mrom_orig, sizeof mrom);
		memset(lockon, 0, sizeof lockon);
		for (auto &p : pokes) {
			if (p.first >= 0x8000 && p.first < 0x9000) { lockon[p.first & 0xfff] = 1; lockv[p.first & 0xfff] = p.second; }
			else if (p.first < 0x8000) mrom[p.first] = p.second;
		}
	}
	/* debug: jump to the next / previous scene.  The game's own set-up state (800A = 3) builds the scene from the
	 * scene (8223, 1-4) and level (8222) counters, exactly as it does after finishing a scene. */
	int skip_pending = 0;               /* +1 / -1 = a scene change waits for the next interrupt */
	bool can_skip() const { return mram[0x004] == 0 && mram[0x00A] == 4; }
	bool request_skip(int dir) { if (!can_skip()) return false; skip_pending = dir; return true; }
	bool skip_scene(int dir)
	{
		if (!can_skip()) return false;      /* only while a game is being played */
		int sc = mram[0x223], lv = mram[0x222];
		if (sc < 1 || sc > 4 || lv < 1) return false;
		if (dir > 0) { if (sc >= 4) { sc = 1; if (lv < 99) lv++; } else sc++; }
		else { if (sc <= 1) { if (lv <= 1) return false; lv--; sc = 4; } else sc--; }
		mram[0x223] = (uint8_t)sc; mram[0x222] = (uint8_t)lv; mram[0x00A] = 3;
		return true;
	}

	void run_frame()
	{
		frame_done = false;
		audio.clear();
		while (!frame_done) step_sample();
		for (auto &p : pokes) if (p.first >= 0x8000 && p.first < 0x9000) mram[p.first & 0xfff] = p.second;
	}
};

static Machine M;

#ifndef CB_LIBRETRO

/* ===================================================================== */
/*  Cheats: cheats.txt next to the program, one per line:                 */
/*      Name = 8123:09, 8124:09        (hex address : hex value)          */
/*  A leading * switches the cheat on at start-up.  ; starts a comment.   */
/* ===================================================================== */
struct Cheat { std::string name; std::vector<std::pair<uint16_t, uint8_t>> pk; bool on = false; std::string desc; };

static std::string cheats_path()
{
	const char *bp = SDL_GetBasePath();
	return std::string(bp ? bp : "") + "cheats.txt";
}

static bool parse_poke(const std::string &t, std::pair<uint16_t, uint8_t> &out)
{
	unsigned a, v;
	if (sscanf(t.c_str(), " %x : %x", &a, &v) != 2 && sscanf(t.c_str(), " %x = %x", &a, &v) != 2) return false;
	if (a > 0x8fff || v > 255) return false;       /* 0000-7FFF patch the program ROM, 8000-8FFF lock a RAM byte */
	out = { (uint16_t)a, (uint8_t)v };
	return true;
}

static void cheats_load(std::vector<Cheat> &list)
{
	FILE *f = fopen(cheats_path().c_str(), "r");
	if (!f) return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		std::string l = line;
		size_t c = l.find(';'); if (c != std::string::npos) l.resize(c);
		while (!l.empty() && isspace((unsigned char)l.back())) l.pop_back();
		size_t i = 0; while (i < l.size() && isspace((unsigned char)l[i])) i++;
		l = l.substr(i);
		if (l.empty()) continue;
		Cheat ch;
		if (l[0] == '*') { ch.on = true; l = l.substr(1); while (!l.empty() && isspace((unsigned char)l[0])) l = l.substr(1); }
		size_t eq = l.find('=');
		if (eq == std::string::npos) continue;
		ch.name = l.substr(0, eq);
		while (!ch.name.empty() && isspace((unsigned char)ch.name.back())) ch.name.pop_back();
		for (auto &c2 : ch.name) c2 = (char)toupper((unsigned char)c2);
		std::string rest = l.substr(eq + 1);
		size_t pos = 0;
		while (pos <= rest.size()) {
			size_t e = rest.find(',', pos); if (e == std::string::npos) e = rest.size();
			std::pair<uint16_t, uint8_t> pk;
			if (parse_poke(rest.substr(pos, e - pos), pk)) ch.pk.push_back(pk);
			pos = e + 1;
		}
		if (!ch.pk.empty() && !ch.name.empty()) list.push_back(ch);
	}
	fclose(f);
}

static void cheat_append(const Cheat &c)
{
	FILE *f = fopen(cheats_path().c_str(), "a");
	if (!f) return;
	fprintf(f, "%s = ", c.name.c_str());
	for (size_t i = 0; i < c.pk.size(); i++) fprintf(f, "%s%04X:%02X", i ? ", " : "", c.pk[i].first, c.pk[i].second);
	fprintf(f, "\n");
	fclose(f);
}

/* ===================================================================== */
/*  File output helpers                                                  */
/* ===================================================================== */
static void write_wav(const char *path, const std::vector<int16_t> &s)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	uint32_t n = (uint32_t)(s.size() * 2), rate = RATE, br = RATE * 2;
	auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + n); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32(rate); w32(br); w16(2); w16(16);
	fwrite("data", 1, 4, f); w32(n); fwrite(s.data(), 2, s.size(), f);
	fclose(f);
}

/* rotate the 256x224 frame for display: 0 = as scanned, 1 = 90 degrees clockwise (the arcade monitor) */
static void make_view(const uint32_t *src, int rotation, std::vector<uint32_t> &dst, int &w, int &h)
{
	if (rotation == 0) {
		w = SCR_W; h = SCR_H; dst.assign(src, src + SCR_W * SCR_H);
		return;
	}
	w = SCR_H; h = SCR_W; dst.resize((size_t)w * h);
	for (int y = 0; y < SCR_H; y++)
		for (int x = 0; x < SCR_W; x++)
			dst[(size_t)x * w + (SCR_H - 1 - y)] = src[y * SCR_W + x];
}

static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	uint32_t rowb = (uint32_t)((w * 3 + 3) & ~3), size = 54 + rowb * h;
	uint8_t hdr[54] = {'B', 'M'};
	auto p32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
	p32(2, size); p32(10, 54); p32(14, 40); p32(18, (uint32_t)w); p32(22, (uint32_t)h); hdr[26] = 1; hdr[28] = 24; p32(34, rowb * h);
	fwrite(hdr, 1, 54, f);
	std::vector<uint8_t> row(rowb, 0);
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			uint32_t c = px[y * w + x];
			row[x * 3 + 0] = c & 0xff; row[x * 3 + 1] = (c >> 8) & 0xff; row[x * 3 + 2] = (c >> 16) & 0xff;
		}
		fwrite(row.data(), 1, rowb, f);
	}
	fclose(f);
}


/* ===================================================================== */
/*  settings (saved in congobongo.ini)                                   */
/* ===================================================================== */
struct CrtParams { int curve, corner, scan, mask, glow, vig, bright; };   /* all 0..100 (bright 50..150) */
static const CrtParams CRT_PRESETS[4] = {
	{ 22, 25, 55, 25, 30, 30, 100 },   /* ARCADE: a cabinet monitor, gentle curve */
	{ 14, 20, 30, 15, 45, 25, 105 },   /* SOFT: soft glow, faint lines */
	{ 18, 20, 70,  0,  5, 15, 100 },   /* SHARP: crisp lines, no mask */
	{ 45, 40, 65, 35, 35, 40, 100 },   /* HEAVY: strongly bulged glass */
};
static const char *const CRT_LOOK_NAMES[5] = { "ARCADE", "SOFT", "SHARP", "HEAVY", "CUSTOM" };

struct Settings {
	int fullscreen = 0;      /* 0 windowed, 1 fullscreen */
	int integer_scale = 1;   /* 1 integer scaling, 0 fit window */
	int crt = 0;             /* 0 off, 1 scanlines, 2 CRT, 3 curved CRT */
	int crt_look = 0;        /* 0..3 = CRT_PRESETS, 4 = crt_custom */
	CrtParams crt_custom = { 22, 25, 55, 25, 30, 30, 100 };
	int rotate = 1;          /* 1 = rotated like the arcade monitor */
	int show_fps = 0;
	int window_scale = 0;    /* 0 = AUTO (largest whole-number size that fits the screen), else 1x, 2x, ... */
	int vol_master = 100, vol_psg = 100, vol_perc = 100, vol_gor = 100, vol_bass = 100;   /* percent, 0..200 */
	int bass_soft = 1;       /* rounded bass-drum edges (removes the click) */
	int treble = 2;          /* output low-pass: 0 off, 1 12 kHz, 2 8 kHz, 3 5 kHz, 4 3 kHz */
	int font_mode = 0;       /* menu font: 0 auto (font.bmp, then character ROM, then built-in), 1 ROM only, 2 built-in */
	int font_alpha = -1, font_digit = -1;   /* force the tile number of 'A' / '0' in the character ROM (-1 = detect) */
};

static CrtParams crt_params(const Settings &s) { return s.crt_look < 4 ? CRT_PRESETS[s.crt_look] : s.crt_custom; }

static std::string settings_path()
{
	const char *bp = SDL_GetBasePath();
	return std::string(bp ? bp : "") + "congobongo.ini";
}

static void settings_load(Settings &s)
{
	FILE *f = fopen(settings_path().c_str(), "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char key[64]; int v;
		if (sscanf(line, " %63[a-z_] = %i", key, &v) != 2) continue;
		std::string k = key;
		auto clampi = [](int x, int lo, int hi) { return std::max(lo, std::min(hi, x)); };
		if (k == "fullscreen") s.fullscreen = clampi(v, 0, 1);
		else if (k == "integer_scale") s.integer_scale = clampi(v, 0, 1);
		else if (k == "crt") s.crt = clampi(v, 0, 3);
		else if (k == "crt_look") s.crt_look = clampi(v, 0, 4);
		else if (k == "crt_curve") s.crt_custom.curve = clampi(v, 0, 100);
		else if (k == "crt_corner") s.crt_custom.corner = clampi(v, 0, 100);
		else if (k == "crt_scan") s.crt_custom.scan = clampi(v, 0, 100);
		else if (k == "crt_mask") s.crt_custom.mask = clampi(v, 0, 100);
		else if (k == "crt_glow") s.crt_custom.glow = clampi(v, 0, 100);
		else if (k == "crt_vignette") s.crt_custom.vig = clampi(v, 0, 100);
		else if (k == "crt_bright") s.crt_custom.bright = clampi(v, 50, 150);
		else if (k == "rotate") s.rotate = clampi(v, 0, 1);
		else if (k == "show_fps") s.show_fps = clampi(v, 0, 1);
		else if (k == "window_scale") s.window_scale = clampi(v, 0, 9);
		else if (k == "vol_master") s.vol_master = clampi(v, 0, 200);
		else if (k == "vol_psg") s.vol_psg = clampi(v, 0, 200);
		else if (k == "vol_drums") s.vol_perc = clampi(v, 0, 200);
		else if (k == "vol_bass") s.vol_bass = clampi(v, 0, 200);
		else if (k == "bass_soft") s.bass_soft = clampi(v, 0, 1);
		else if (k == "vol_gorilla") s.vol_gor = clampi(v, 0, 200);
		else if (k == "treble") s.treble = clampi(v, 0, 4);
		else if (k == "font_mode") s.font_mode = clampi(v, 0, 2);
		else if (k == "font_alpha") s.font_alpha = clampi(v, -1, 230);
		else if (k == "font_digit") s.font_digit = clampi(v, -1, 246);
	}
	fclose(f);
}

static void settings_save(const Settings &s)
{
	FILE *f = fopen(settings_path().c_str(), "w");
	if (!f) return;
	fprintf(f, "window_scale=%d\ncrt_look=%d\ncrt_curve=%d\ncrt_corner=%d\ncrt_scan=%d\ncrt_mask=%d\ncrt_glow=%d\ncrt_vignette=%d\ncrt_bright=%d\n",
	        s.window_scale, s.crt_look, s.crt_custom.curve, s.crt_custom.corner, s.crt_custom.scan, s.crt_custom.mask, s.crt_custom.glow, s.crt_custom.vig, s.crt_custom.bright);
	fprintf(f, "fullscreen=%d\ninteger_scale=%d\ncrt=%d\nrotate=%d\nshow_fps=%d\n"
	           "vol_master=%d\nvol_psg=%d\nvol_drums=%d\nvol_gorilla=%d\nvol_bass=%d\nbass_soft=%d\ntreble=%d\nfont_mode=%d\nfont_alpha=%d\nfont_digit=%d\n",
	        s.fullscreen, s.integer_scale, s.crt, s.rotate, s.show_fps,
	        s.vol_master, s.vol_psg, s.vol_perc, s.vol_gor, s.vol_bass, s.bass_soft, s.treble, s.font_mode, s.font_alpha, s.font_digit);
	fclose(f);
}

/* ===================================================================== */
/*  software CRT: 3x upscale with scanlines (mode 1) or                   */
/*  scanlines + phosphor triads + soft horizontal blur + vignette (mode 2) */
/* ===================================================================== */
/* The tunable CRT (modes 2 and 3).  Mode 2 is the flat version, mode 3 adds the curved glass.
 *  - scanlines: every source line is a vertical Gaussian beam whose width grows with the pixel's
 *    brightness (bright lines bloom into the gap, dark ones leave it open, like a real tube)
 *  - mask: RGB phosphor stripes, glow: halation (a soft blur mixed in), vignette, brightness
 *  - curvature: barrel distortion with rounded corners; the geometry is cached in a lookup table */
static void crt_engine(const uint32_t *src, int w, int h, const CrtParams &P, bool curved, std::vector<uint32_t> &dst)
{
	struct Tap { uint16_t x0; int16_t r0; uint8_t fr, fa; uint16_t k; };   /* k: brightness of the glass, 1.0 = 4096 */
	static std::vector<Tap> lut; static int lw = 0, lh = 0, lc = -1, l_curve = -1, l_corner = -1, l_vig = -1;
	const int W = w * 3, H = h * 3;
	dst.resize((size_t)W * H);
	if (lw != w || lh != h || lc != (int)curved || l_curve != P.curve || l_corner != P.corner || l_vig != P.vig) {
		lw = w; lh = h; lc = curved; l_curve = P.curve; l_corner = P.corner; l_vig = P.vig;
		lut.assign((size_t)W * H, Tap{ 0, 0, 0, 0, 0 });
		const float kd = curved ? P.curve * 0.002f : 0.0f;                 /* barrel distortion */
		const float rad = curved ? std::max(0.004f, P.corner * 0.002f) : 0.0f;
		const float fill = curved ? 0.99f - 0.3f * kd : 1.0f;              /* image edge midpoints land at +-fill */
		const float s = 1.0f / (fill * (1.0f + kd * fill * fill));
		const float vg = P.vig * 0.005f;
		const float pix = 2.2f / std::min(W, H);                           /* one output pixel in picture units */
		for (int sy = 0; sy < H; sy++) for (int sx = 0; sx < W; sx++) {
			const float cx = (sx + 0.5f) / W * 2.0f - 1.0f, cy = (sy + 0.5f) / H * 2.0f - 1.0f;
			const float f = s * (1.0f + kd * (cx * cx + cy * cy));
			const float px = cx * f, py = cy * f;
			float cov = 1.0f, edge = 1.0f;
			if (curved) {
				const float qx = std::fabs(px) - (1.0f - rad), qy = std::fabs(py) - (1.0f - rad);
				const float d = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - rad;
				cov = std::min(1.0f, std::max(0.0f, 0.5f - d / pix));         /* antialiased rounded edge */
				if (cov <= 0) continue;
				edge = std::min(1.0f, std::max(0.0f, -d / 0.10f));            /* darkening towards the glass edge */
			}
			const float tx = (px + 1.0f) * 0.5f * w - 0.5f, ty = (py + 1.0f) * 0.5f * h;
			int x0 = (int)std::floor(tx); float fr = tx - x0;
			if (x0 < 0) { x0 = 0; fr = 0; }
			if (x0 >= w - 1) { x0 = w - 1; fr = 0; }
			Tap &t = lut[(size_t)sy * W + sx];
			const float u = ty - 0.5f; const int r0 = (int)std::floor(u); const float fv = u - r0;
			t.x0 = (uint16_t)x0; t.fr = (uint8_t)std::min(255, (int)(fr * 256.0f)); t.r0 = (int16_t)r0;
			t.fa = (uint8_t)std::min(64, (int)(fv * 64.0f + 0.5f));
			const float k = cov * (1.0f - vg * (cx * cx + cy * cy)) * (curved ? 0.85f + 0.15f * edge : 1.0f);
			t.k = (uint16_t)std::max(1, std::min(4096, (int)(k * 4096.0f + 0.5f)));
		}
	}
	/* beam profile table: [brightness level][distance to the line centre, 0..1 in 1/64 steps], normalised so that
	 * a uniform field keeps its average brightness */
	static int gtab[16][65]; static int gscan = -1;     /* 1.0 = 1024 */
	if (gscan != P.scan) {
		gscan = P.scan;
		const float sc = P.scan * 0.01f;
		for (int l = 0; l < 16; l++) {
			const float sig = std::max(0.08f, 0.55f - 0.40f * sc + 0.12f * sc * (l / 15.0f));
			double avg = 0;
			for (int i = 0; i < 1024; i++) {
				const double d = (i + 0.5) / 1024.0;
				avg += std::exp(-d * d / (2.0 * sig * sig)) + std::exp(-(1 - d) * (1 - d) / (2.0 * sig * sig));
			}
			avg /= 1024.0;
			for (int i = 0; i <= 64; i++) { const double d = i / 64.0; gtab[l][i] = (int)std::lround(1024.0 * std::exp(-d * d / (2.0 * sig * sig)) / avg); }
		}
	}
	/* source as floats, with halation */
	static std::vector<float> fb, bl, tmp;
	const size_t n3 = (size_t)w * h * 3;
	fb.resize(n3);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		const uint32_t p = src[i];
		fb[i * 3] = (float)((p >> 16) & 255); fb[i * 3 + 1] = (float)((p >> 8) & 255); fb[i * 3 + 2] = (float)(p & 255);
	}
	const float g = P.glow * 0.006f;
	if (g > 0.001f) {
		/* halation: separable 3-tap blur applied twice (about radius 2 source pixels), vertical pass in rows */
		tmp.resize(n3); bl.resize(n3);
		const size_t rs = (size_t)w * 3;
		for (int pass = 0; pass < 2; pass++) {
			const float *in = pass == 0 ? fb.data() : bl.data();
			for (int y = 0; y < h; y++) {
				const float *r = in + (size_t)y * rs; float *o = tmp.data() + (size_t)y * rs;
				for (int c = 0; c < 3; c++) o[c] = 0.75f * r[c] + 0.25f * r[3 + c];
				for (size_t i = 3; i + 3 < rs; i++) o[i] = 0.5f * r[i] + 0.25f * (r[i - 3] + r[i + 3]);
				for (int c = 0; c < 3; c++) o[rs - 3 + c] = 0.75f * r[rs - 3 + c] + 0.25f * r[rs - 6 + c];
			}
			for (int y = 0; y < h; y++) {
				const float *u = tmp.data() + (size_t)std::max(0, y - 1) * rs, *m0 = tmp.data() + (size_t)y * rs, *d = tmp.data() + (size_t)std::min(h - 1, y + 1) * rs;
				float *o = bl.data() + (size_t)y * rs;
				for (size_t i = 0; i < rs; i++) o[i] = 0.5f * m0[i] + 0.25f * (u[i] + d[i]);
			}
		}
		for (size_t i = 0; i < n3; i++) fb[i] = fb[i] * (1.0f - g) + bl[i] * g;
	}
	static std::vector<uint32_t> pk;
	pk.resize((size_t)w * h);
	for (size_t i = 0; i < (size_t)w * h; i++)
		pk[i] = (uint32_t)(fb[i * 3] + 0.5f) << 16 | (uint32_t)(fb[i * 3 + 1] + 0.5f) << 8 | (uint32_t)(fb[i * 3 + 2] + 0.5f);
	const float M = P.mask * 0.006f, mavg = 1.0f - M * (2.0f / 3.0f), gain = P.bright * 0.01f;
	int mk[3][3];                                        /* 1.0 = 1024 */
	for (int p = 0; p < 3; p++) for (int c = 0; c < 3; c++) mk[p][c] = (int)std::lround((p == c ? 1.0f : 1.0f - M) / mavg * gain * 1024.0f);
	for (int sy = 0; sy < H; sy++) {
		uint32_t *drow = &dst[(size_t)sy * W];
		const Tap *trow = &lut[(size_t)sy * W];
		for (int sx = 0; sx < W; sx++) {
			const Tap &t = trow[sx];
			if (t.k == 0) { drow[sx] = 0xff000000u; continue; }
			const int ra = std::max(0, std::min(h - 1, (int)t.r0)), rb = std::max(0, std::min(h - 1, (int)t.r0 + 1));
			const int xa = t.x0, xb = std::min(w - 1, xa + 1), fr = t.fr;
			const uint32_t a0 = pk[(size_t)ra * w + xa], a1 = pk[(size_t)ra * w + xb];
			const uint32_t b0 = pk[(size_t)rb * w + xa], b1 = pk[(size_t)rb * w + xb];
			int A[3], B[3];
			for (int c = 0, sh = 16; c < 3; c++, sh -= 8) {
				const int pa = (a0 >> sh) & 255, qa = (a1 >> sh) & 255, pb = (b0 >> sh) & 255, qb = (b1 >> sh) & 255;
				A[c] = (pa * (256 - fr) + qa * fr) >> 8; B[c] = (pb * (256 - fr) + qb * fr) >> 8;
			}
			const int la = (A[0] * 77 + A[1] * 151 + A[2] * 28) >> 12, lb = (B[0] * 77 + B[1] * 151 + B[2] * 28) >> 12;
			const int wa = gtab[la][t.fa], wb = gtab[lb][64 - t.fa];
			const int *m3 = mk[sx % 3];
			uint32_t o = 0xff000000u;
			for (int c = 0, sh = 16; c < 3; c++, sh -= 8) {
				const int v = (A[c] * wa + B[c] * wb) >> 10;
				const int mul = (m3[c] * t.k) >> 12;
				o |= (uint32_t)std::min(255, (v * mul) >> 10) << sh;
			}
			drow[sx] = o;
		}
	}
}

/* mode 1: plain scanlines.  modes 2, 3: crt_engine (flat / curved) */
static void crt_filter(const uint32_t *src, int w, int h, int mode, const CrtParams &P, std::vector<uint32_t> &dst)
{
	if (mode >= 2) { crt_engine(src, w, h, P, mode == 3, dst); return; }
	const int W = w * 3, H = h * 3;
	dst.resize((size_t)W * H);
	static const float vprof1[3] = { 0.62f, 1.0f, 0.62f };
	for (int sy = 0; sy < H; sy++) {
		const float vf = vprof1[sy % 3];
		const uint32_t *srow = src + (size_t)(sy / 3) * w;
		uint32_t *drow = &dst[(size_t)sy * W];
		for (int sx = 0; sx < W; sx++) {
			const uint32_t p = srow[sx / 3];
			drow[sx] = 0xff000000u
				| ((uint32_t)std::min(255.0f, ((p >> 16) & 255) * vf) << 16)
				| ((uint32_t)std::min(255.0f, ((p >> 8) & 255) * vf) << 8)
				| (uint32_t)std::min(255.0f, (p & 255) * vf);
		}
	}
}


/* Press Start 2P 8x8 glyphs (ASCII 32..126), rasterised from the font at 8 px (SIL OFL 1.1) */
static const uint8_t PS2P_GLYPHS[95][8] = {
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x38, 0x38, 0x38, 0x30, 0x30, 0x00, 0x30, 0x00 },
	{ 0x6C, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x6C, 0xFE, 0x6C, 0x6C, 0x6C, 0xFE, 0x6C, 0x00 },
	{ 0x10, 0x7C, 0xD0, 0x7C, 0x16, 0xFC, 0x10, 0x00 },
	{ 0x62, 0xA4, 0xC8, 0x10, 0x26, 0x4A, 0x8C, 0x00 },
	{ 0x70, 0xD8, 0xD8, 0x70, 0xDA, 0xCC, 0x7E, 0x00 },
	{ 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 },
	{ 0x60, 0x30, 0x18, 0x18, 0x18, 0x30, 0x60, 0x00 },
	{ 0x00, 0x6C, 0x38, 0xFE, 0x38, 0x6C, 0x00, 0x00 },
	{ 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 },
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x60 },
	{ 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 },
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00 },
	{ 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00 },
	{ 0x38, 0x4C, 0xC6, 0xC6, 0xC6, 0x64, 0x38, 0x00 },
	{ 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },
	{ 0x7C, 0xC6, 0x0E, 0x3C, 0x78, 0xE0, 0xFE, 0x00 },
	{ 0x7E, 0x0C, 0x18, 0x3C, 0x06, 0xC6, 0x7C, 0x00 },
	{ 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00 },
	{ 0xFC, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00 },
	{ 0x3C, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00 },
	{ 0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 },
	{ 0x78, 0xC4, 0xE4, 0x78, 0x9E, 0x86, 0x7C, 0x00 },
	{ 0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00 },
	{ 0x00, 0x30, 0x30, 0x00, 0x30, 0x30, 0x00, 0x00 },
	{ 0x00, 0x30, 0x30, 0x00, 0x30, 0x30, 0x60, 0x00 },
	{ 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00 },
	{ 0x00, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0x00, 0x00 },
	{ 0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00 },
	{ 0x7C, 0xFE, 0xC6, 0x0C, 0x38, 0x00, 0x38, 0x00 },
	{ 0x7C, 0x82, 0xBA, 0xAA, 0xBE, 0x80, 0x7C, 0x00 },
	{ 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0x00 },
	{ 0xFC, 0xC6, 0xC6, 0xFC, 0xC6, 0xC6, 0xFC, 0x00 },
	{ 0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00 },
	{ 0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8, 0x00 },
	{ 0xFE, 0xC0, 0xC0, 0xFC, 0xC0, 0xC0, 0xFE, 0x00 },
	{ 0xFE, 0xC0, 0xC0, 0xFC, 0xC0, 0xC0, 0xC0, 0x00 },
	{ 0x3E, 0x60, 0xC0, 0xCE, 0xC6, 0x66, 0x3E, 0x00 },
	{ 0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00 },
	{ 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },
	{ 0x06, 0x06, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0x00 },
	{ 0xC6, 0xCC, 0xD8, 0xF0, 0xF8, 0xDC, 0xCE, 0x00 },
	{ 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 },
	{ 0xC6, 0xEE, 0xFE, 0xD6, 0xD6, 0xC6, 0xC6, 0x00 },
	{ 0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00 },
	{ 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 },
	{ 0xFC, 0xC6, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0, 0x00 },
	{ 0x7C, 0xC6, 0xC6, 0xC6, 0xDE, 0xCC, 0x7A, 0x00 },
	{ 0xFC, 0xC6, 0xC6, 0xCE, 0xF8, 0xDC, 0xCE, 0x00 },
	{ 0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6, 0x7C, 0x00 },
	{ 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
	{ 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 },
	{ 0xC6, 0xC6, 0xC6, 0xEE, 0x7C, 0x38, 0x10, 0x00 },
	{ 0xD6, 0xD6, 0xD6, 0xD6, 0xFE, 0xEE, 0x44, 0x00 },
	{ 0xC6, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0xC6, 0x00 },
	{ 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00 },
	{ 0xFE, 0x0E, 0x1C, 0x38, 0x70, 0xE0, 0xFE, 0x00 },
	{ 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00 },
	{ 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x00 },
	{ 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78, 0x00 },
	{ 0x38, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE },
	{ 0x10, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x00, 0x00, 0x7C, 0x06, 0x7E, 0xC6, 0x7E, 0x00 },
	{ 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 },
	{ 0x00, 0x00, 0x7E, 0xC0, 0xC0, 0xC0, 0x7E, 0x00 },
	{ 0x06, 0x06, 0x7E, 0xC6, 0xC6, 0xC6, 0x7E, 0x00 },
	{ 0x00, 0x00, 0x7C, 0xC6, 0xFE, 0xC0, 0x7C, 0x00 },
	{ 0x0E, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x00 },
	{ 0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x7C },
	{ 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00 },
	{ 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00 },
	{ 0x0C, 0x00, 0x1C, 0x0C, 0x0C, 0x0C, 0x0C, 0x78 },
	{ 0xC0, 0xC0, 0xC6, 0xCC, 0xF8, 0xCC, 0xC6, 0x00 },
	{ 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },
	{ 0x00, 0x00, 0xFC, 0xB6, 0xB6, 0xB6, 0xB6, 0x00 },
	{ 0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00 },
	{ 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 },
	{ 0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0 },
	{ 0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x06 },
	{ 0x00, 0x00, 0x6E, 0x70, 0x60, 0x60, 0x60, 0x00 },
	{ 0x00, 0x00, 0x7C, 0xC0, 0x7C, 0x06, 0xFC, 0x00 },
	{ 0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x00 },
	{ 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0x7E, 0x00 },
	{ 0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },
	{ 0x00, 0x00, 0xD6, 0xD6, 0xD6, 0xD6, 0x6C, 0x00 },
	{ 0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00 },
	{ 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0x7C },
	{ 0x00, 0x00, 0xFE, 0x1C, 0x38, 0x70, 0xFE, 0x00 },
	{ 0x0C, 0x18, 0x18, 0x30, 0x18, 0x18, 0x0C, 0x00 },
	{ 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
	{ 0x60, 0x30, 0x30, 0x18, 0x30, 0x30, 0x60, 0x00 },
	{ 0x00, 0x00, 0x70, 0xBA, 0x1C, 0x00, 0x00, 0x00 },
};

#include "congo_logo_data.h"

/* ---- vector logo: a small scanline rasteriser for the embedded Bezier paths (fill: non-zero winding with
 * 5x vertical and exact horizontal coverage; stroke: distance to the flattened outline, round joins, butt or
 * round caps).  The logo is redrawn at the real output size, so it stays sharp at any window size. ---- */
struct LogoPt { float x, y; };

static void logo_flatten(const LogoSub &sb, float sc, float ox, float oy, std::vector<LogoPt> &out)
{
	const float *p = &LOGO_PTS[sb.pt * 2];
	out.push_back({ p[0] * sc + ox, p[1] * sc + oy });
	const float *q = p + 2;
	for (int i = 0; i < sb.nseg; i++, q += 6) {
		const LogoPt a = out.back(), b = { q[0] * sc + ox, q[1] * sc + oy }, c = { q[2] * sc + ox, q[3] * sc + oy }, d = { q[4] * sc + ox, q[5] * sc + oy };
		const float len = std::hypot(b.x - a.x, b.y - a.y) + std::hypot(c.x - b.x, c.y - b.y) + std::hypot(d.x - c.x, d.y - c.y);
		const int n = std::max(2, std::min(48, (int)std::ceil(len / 1.5f)));
		for (int k = 1; k <= n; k++) {
			const float t = (float)k / n, u = 1 - t;
			out.push_back({ u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
			                u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y });
		}
	}
}

static void logo_render(int wpx, std::vector<uint32_t> &out, int &W, int &H)
{
	float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, hwmax = 0;
	for (auto &pp : LOGO_PATHS) hwmax = std::max(hwmax, pp.stroke ? pp.stroke_w * 0.5f : 0.0f);
	const size_t npts = sizeof LOGO_PTS / sizeof LOGO_PTS[0] / 2;
	for (size_t i = 0; i < npts; i++) {
		minx = std::min(minx, LOGO_PTS[i * 2]); maxx = std::max(maxx, LOGO_PTS[i * 2]);
		miny = std::min(miny, LOGO_PTS[i * 2 + 1]); maxy = std::max(maxy, LOGO_PTS[i * 2 + 1]);
	}
	minx -= hwmax; miny -= hwmax; maxx += hwmax; maxy += hwmax;
	const float sc = (float)wpx / (maxx - minx);
	W = wpx; H = (int)std::ceil((maxy - miny) * sc);
	const float ox = -minx * sc, oy = -miny * sc;
	std::vector<float> R((size_t)W * H, 0.0f), G(R), B(R), A(R), cov((size_t)W * H);
	auto composite = [&](uint32_t col, const std::vector<float> &cv) {
		const float cr = ((col >> 16) & 255) / 255.0f, cg = ((col >> 8) & 255) / 255.0f, cb = (col & 255) / 255.0f;
		for (size_t i = 0; i < cv.size(); i++) {
			const float a = std::min(1.0f, cv[i]); if (a <= 0) continue;
			R[i] = cr * a + R[i] * (1 - a); G[i] = cg * a + G[i] * (1 - a); B[i] = cb * a + B[i] * (1 - a); A[i] = a + A[i] * (1 - a);
		}
	};
	std::vector<std::vector<LogoPt>> polys;
	for (auto &pp : LOGO_PATHS) {
		polys.clear();
		for (int s = 0; s < pp.n_sub; s++) { polys.emplace_back(); logo_flatten(LOGO_SUBS[pp.first_sub + s], sc, ox, oy, polys.back()); }
		if (pp.fill) {
			std::fill(cov.begin(), cov.end(), 0.0f);
			float y0 = 1e9f, y1 = -1e9f;
			for (auto &pl : polys) for (auto &pt : pl) { y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y); }
			const int ya = std::max(0, (int)std::floor(y0)), yb = std::min(H - 1, (int)std::floor(y1));
			const int SS = 5;
			std::vector<std::pair<float, int>> xs;
			for (int y = ya; y <= yb; y++) for (int ss = 0; ss < SS; ss++) {
				const float sy = y + (ss + 0.5f) / SS;
				xs.clear();
				for (auto &pl : polys) {
					const size_t n = pl.size();
					for (size_t i = 0; i < n; i++) {
						const LogoPt a = pl[i], b = pl[(i + 1) % n];     /* fills close each subpath implicitly */
						if ((a.y <= sy) == (b.y <= sy)) continue;
						xs.push_back({ a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x), b.y > a.y ? 1 : -1 });
					}
				}
				std::sort(xs.begin(), xs.end());
				int wind = 0;
				for (size_t i = 0; i + 1 < xs.size(); i++) {
					wind += xs[i].second;
					if (!wind) continue;
					const float xa = std::max(0.0f, xs[i].first), xb = std::min((float)W, xs[i + 1].first);
					if (xb <= xa) continue;
					const int ia = (int)xa, ib = std::min(W - 1, (int)xb);
					float *row = &cov[(size_t)y * W];
					if (ia == ib) row[ia] += (xb - xa) / SS;
					else {
						row[ia] += (ia + 1 - xa) / SS;
						for (int x = ia + 1; x < ib; x++) row[x] += 1.0f / SS;
						row[ib] += (xb - ib) / SS;
					}
				}
			}
			composite(pp.fill, cov);
		}
		if (pp.stroke && pp.stroke_w > 0) {
			std::fill(cov.begin(), cov.end(), 0.0f);
			const float hw = pp.stroke_w * sc * 0.5f;
			for (int s = 0; s < pp.n_sub; s++) {
				const auto &pl = polys[s]; const bool closed = LOGO_SUBS[pp.first_sub + s].closed != 0;
				const size_t n = pl.size(), nseg = closed ? n : n - 1;
				for (size_t i = 0; i < nseg; i++) {
					const LogoPt a = pl[i], b = pl[(i + 1) % n];
					const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
					if (l2 < 1e-12f) continue;
					const bool skip_start = !closed && i == 0 && !pp.round_cap, skip_end = !closed && i == nseg - 1 && !pp.round_cap;
					const int x0 = std::max(0, (int)std::floor(std::min(a.x, b.x) - hw - 1)), x1 = std::min(W - 1, (int)std::ceil(std::max(a.x, b.x) + hw + 1));
					const int y0 = std::max(0, (int)std::floor(std::min(a.y, b.y) - hw - 1)), y1 = std::min(H - 1, (int)std::ceil(std::max(a.y, b.y) + hw + 1));
					for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
						const float px = x + 0.5f - a.x, py = y + 0.5f - a.y;
						float t = (px * dx + py * dy) / l2;
						if ((t < 0 && skip_start) || (t > 1 && skip_end)) continue;
						t = std::max(0.0f, std::min(1.0f, t));
						const float ex = px - t * dx, ey = py - t * dy;
						const float c = std::min(1.0f, std::max(0.0f, hw + 0.5f - std::sqrt(ex * ex + ey * ey)));
						float &dstc = cov[(size_t)y * W + x]; if (c > dstc) dstc = c;
					}
				}
			}
			composite(pp.stroke, cov);
		}
	}
	out.resize((size_t)W * H);
	for (size_t i = 0; i < out.size(); i++) {
		const float a = A[i];
		if (a <= 0.0f) { out[i] = 0; continue; }
		auto q = [&](float v) { return (uint32_t)std::lround(std::max(0.0f, std::min(1.0f, v / a)) * 255.0f); };
		out[i] = (uint32_t)std::lround(std::min(1.0f, a) * 255.0f) << 24 | q(R[i]) << 16 | q(G[i]) << 8 | q(B[i]);
	}
}

/* ===================================================================== */
/*  menu font: the game's own character ROM, a user font.bmp, or the      */
/*  built-in SDL debug font                                               */
/* ===================================================================== */
struct MenuFont {
	uint8_t g[128][8];                 /* ASCII 0..127, 8 rows, bit 7 = leftmost pixel */
	char info[200];
	SDL_Texture *atlas = nullptr;
};

static uint64_t rows_to_mask(const uint8_t *r)
{
	uint64_t m = 0;
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if ((r[y] >> (7 - x)) & 1) m |= 1ULL << (y * 8 + x);
	return m;
}
static void mask_to_rows(uint64_t m, uint8_t *r)
{
	for (int y = 0; y < 8; y++) {
		uint8_t v = 0;
		for (int x = 0; x < 8; x++) if ((m >> (y * 8 + x)) & 1) v |= (uint8_t)(0x80 >> x);
		r[y] = v;
	}
}
static uint64_t shift_mask(uint64_t m, int dx, int dy)
{
	uint64_t o = 0;
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
		if (!((m >> (y * 8 + x)) & 1)) continue;
		int nx = x + dx, ny = y + dy;
		if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8) o |= 1ULL << (ny * 8 + nx);
	}
	return o;
}
/* similarity between a ROM tile mask and a reference glyph: best IoU over +-1 pixel shifts, also trying a
 * horizontally "bold" version of the reference (arcade fonts are chunkier than the debug font) */
static double glyph_sim(uint64_t rom, uint64_t ref)
{
	if (!rom || !ref) return 0;
	double best = 0;
	for (int bold = 0; bold < 2; bold++) {
		uint64_t v = bold ? (ref | shift_mask(ref, 1, 0)) : ref;
		for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
			uint64_t u = shift_mask(v, dx, dy);
			int i = __builtin_popcountll(rom & u), n = __builtin_popcountll(rom | u);
			if (n) best = std::max(best, (double)i / n);
		}
	}
	return best;
}

/* the built-in menu font (Press Start 2P, 8x8): the fallback when the ROM font is not found, and the reference
 * shapes used to recognise the letters in the character ROM */
static bool builtin_glyphs(uint8_t out[128][8])
{
	memset(out, 0, 128 * 8);
	for (int c = 33; c < 127; c++) memcpy(out[c], PS2P_GLYPHS[c - 32], 8);
	return true;
}

/* user-supplied bitmap: 16 glyphs per row, 8x8 each; either 6 rows (ASCII 32..127) or 8 rows (ASCII 0..127);
 * any bright pixel counts as set */
static bool load_font_bmp(const std::string &path, uint8_t g[128][8])
{
	SDL_Surface *sf = SDL_LoadBMP(path.c_str());
	if (!sf) return false;
	SDL_Surface *cv = SDL_ConvertSurface(sf, SDL_PIXELFORMAT_ARGB8888);
	SDL_DestroySurface(sf);
	if (!cv) return false;
	bool ok = cv->w >= 128 && cv->h >= 48;
	if (ok) {
		const int rows = cv->h >= 64 ? 8 : 6, first = rows == 8 ? 0 : 2;
		const uint32_t *px = (const uint32_t *)cv->pixels; const int pitch = cv->pitch / 4;
		for (int gi = 0; gi < rows * 16; gi++) {
			int c = first * 16 + gi, gx = (gi & 15) * 8, gy = (gi >> 4) * 8;
			for (int y = 0; y < 8; y++) {
				uint8_t v = 0;
				for (int x = 0; x < 8; x++) {
					uint32_t p = px[(gy + y) * pitch + gx + x];
					int lum = (int)(((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3;
					if (lum > 96 && ((p >> 24) & 255) > 0) v |= (uint8_t)(0x80 >> x);
				}
				g[c][y] = v;
			}
		}
	}
	SDL_DestroySurface(cv);
	return ok;
}

/* Work out which character-ROM tiles are the letters and digits by comparing every tile with the reference
 * glyphs: letters and digits are stored as consecutive runs, so look for the 10 / 26 consecutive tiles that
 * match '0'-'9' / 'A'-'Z' best.  Returns false (and leaves g alone) when the match is not convincing. */
static bool detect_rom_font(const uint8_t *txpix, int force_alpha, int force_digit, uint8_t g[128][8], char *info, size_t info_n, double *conf_out)
{
	uint8_t ref[128][8];
	if (!builtin_glyphs(ref)) return false;
	uint64_t refm[128];
	for (int c = 0; c < 128; c++) refm[c] = rows_to_mask(ref[c]);

	/* candidate masks per tile: any pen, or a single pen (some fonts have an outline pen) */
	static uint64_t tm[256][4];
	for (int t = 0; t < 256; t++) {
		for (int k = 0; k < 4; k++) tm[t][k] = 0;
		for (int i = 0; i < 64; i++) {
			int p = txpix[t * 64 + i];
			if (p) tm[t][0] |= 1ULL << i;
			if (p >= 1 && p <= 3) tm[t][p] |= 1ULL << i;
		}
	}
	auto sim = [&](int tile, int ch, int *which = nullptr) {
		double b = 0; int bw = 0;
		for (int k = 0; k < 4; k++) { double v = glyph_sim(tm[tile][k], refm[ch]); if (v > b) { b = v; bw = k; } }
		if (which) *which = bw;
		return b;
	};

	auto best_run = [&](int first_ch, int n, int forced, double *mean) {
		int best_s = -1; double best = -1;
		for (int s = 0; s + n <= 256; s++) {
			if (forced >= 0 && s != forced) continue;
			double sum = 0;
			for (int i = 0; i < n; i++) sum += sim(s + i, first_ch + i);
			if (sum > best) { best = sum; best_s = s; }
		}
		*mean = best / n;
		return best_s;
	};
	double ma = 0, md = 0;
	int sa = best_run('A', 26, force_alpha, &ma);
	int sd = best_run('0', 10, force_digit, &md);
	if (conf_out) *conf_out = std::min(ma, md);
	const bool forced = force_alpha >= 0 || force_digit >= 0;
	if (sa < 0 || sd < 0 || (!forced && (ma < 0.64 || md < 0.62))) {
		snprintf(info, info_n, "ROM font not recognised (letters %.2f, digits %.2f), using built-in", ma, md);
		return false;
	}

	std::vector<bool> used(256, false);
	auto put = [&](int ch, int tile) {
		int w; sim(tile, ch, &w);
		mask_to_rows(tm[tile][w], g[ch]);
		used[tile] = true;
	};
	for (int i = 0; i < 26; i++) { put('A' + i, sa + i); memcpy(g['a' + i], g['A' + i], 8); }
	for (int i = 0; i < 10; i++) put('0' + i, sd + i);
	/* a few punctuation marks, only on a very close match outside the letter/digit runs */
	int np = 0;
	for (const char *p = ".-/%=+:"; *p; p++) {
		double b = 0; int bt = -1;
		for (int t = 0; t < 256; t++) {
			if (used[t]) continue;
			double v = sim(t, (unsigned char)*p);
			if (v > b) { b = v; bt = t; }
		}
		if (bt >= 0 && b >= 0.78) { put((unsigned char)*p, bt); np++; }
	}
	memset(g[' '], 0, 8);
	snprintf(info, info_n, "menu font from the character ROM: letters at tile %02X, digits at tile %02X (match %.2f / %.2f), %d punctuation marks",
	         sa, sd, ma, md, np);
	return true;
}

static SDL_Texture *make_font_atlas(SDL_Renderer *ren, const uint8_t g[128][8])
{
	static uint32_t px[128 * 64];
	for (int c = 0; c < 128; c++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				px[((c >> 4) * 8 + y) * 128 + (c & 15) * 8 + x] = ((g[c][y] >> (7 - x)) & 1) ? 0xffffffffu : 0x00000000u;
	SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 128, 64);
	if (!t) return nullptr;
	SDL_UpdateTexture(t, nullptr, px, 128 * 4);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
	return t;
}

#endif /* !CB_LIBRETRO */

} // namespace cb

#ifndef CB_LIBRETRO

/* ===================================================================== */
/*  main                                                                 */
/* ===================================================================== */
using namespace cb;

static void usage()
{
	printf(
"Congo Bongo / Tip Top emulator (SDL3)\n"
"\n"
"usage: congobongo [options] [rom-zip-or-folder ...]\n"
"\n"
"  With no path, the folder 'roms' and the current folder are searched for\n"
"  congo.zip / tiptop.zip or loose ROM files (matched by CRC32).\n"
"\n"
"options:\n"
"  --scale N        window scale (default 3)\n"
"  --no-rotate      do not rotate the picture 90 degrees\n"
"  --volume X       master volume, 0..2 (default 1)\n"
"  --lives N        3, 4 or 5 (default 3);  --free-play\n"
"  --bonus N        10000, 20000, 30000 or 40000 (default 10000)\n"
"  --difficulty N   0 easy .. 3 hardest (default 1)\n"
"  --font MODE      menu font: auto (default), rom, builtin.  A font.bmp next to the program\n"
"                   (16 glyphs per row, 8x8 each, 6 rows = ASCII 32..127) overrides the ROM font\n"
"  --poke ADDR:VAL  keep main-RAM byte ADDR (hex, 8000-8FFF) at VAL (hex), like a cheat; repeatable\n"
"  --dump-font F    write a picture of the menu font to F (BMP) and exit\n"
"  --sound-test     sound board only (needs just the sound ROM); pick a command\n"
"                   with the arrow keys and press Enter to play it\n"
"  --headless       no window or audio device; run --frames N and exit\n"
"  --frames N       frames to run in headless mode (default 600)\n"
"  --wav FILE       headless: write the audio to a WAV file\n"
"  --shot FILE      headless: save the last frame as a BMP\n"
"  --shot-every N   headless: also save every Nth frame as FILE-<n>.bmp\n"
"  --coin-at N      headless: insert a coin at frame N;  --start-at N presses 1P start\n"
"  --cmd T:CMD      headless sound test: send command CMD at T seconds (e.g. 1.0:0x91)\n"
"\n"
"keys: arrows = move, Z / Left Ctrl / Space = jump, 5 = coin, 1 / 2 = start,\n"
"      Esc = options menu (volumes, CRT, scaling, fullscreen), P = pause,\n"
"      R = rotate view, F3 = reset, F11 = fullscreen, F12 = screenshot, +/- = master volume\n"
"\n"
"Settings from the options menu are saved in congobongo.ini next to the program.\n");
}

int main(int argc, char **argv)
{
	int scale = 0, rotation = 1;      /* scale: --scale N (0 = use the saved WINDOW SIZE setting) */
	bool headless = false, sound_only = false, free_play = false;
	int frames_to_run = 600;
	float volume = 1.0f; bool vol_given = false;
	int bass_edge_cli = -1; float vol_bass_cli = -1;
	float vol_cli[3] = { -1, -1, -1 };   /* --vol-psg / --vol-drums / --vol-gorilla (0..2) */
	bool dev_stats = false;
	std::vector<std::string> keyscript; size_t kidx = 0; int kwait = 0;   /* developer: --keys 30,esc,down,enter (numbers = wait that many frames) */
	std::vector<std::pair<uint16_t, uint8_t>> cli_pokes;
	int font_cli = 0; bool font_cli_set = false; std::string dump_font;
	int dev_page = -1, dev_crt = -1; std::string dev_out;   /* developer: --menu-shot PAGE:FILE.bmp, --crt N */
	int lives = 3, bonus = 10000, difficulty = 1;
	struct InpEv { int f0, f1; unsigned bits; };
	struct PokeAt { int f, a, v; };
	std::vector<PokeAt> pokeat, skipat;
	std::vector<InpEv> inps; std::vector<uint16_t> watch_cli; std::string dump_ram;
	std::string wav_path, shot_path;
	int shot_every = 0, coin_at = -1, start_at = -1;
	struct Cmd { double t; int cmd; bool done; };
	std::vector<Cmd> cmds;
	std::vector<std::string> paths;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](const char *name) -> const char * {
			if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", name); exit(1); }
			return argv[++i];
		};
		if (a == "-h" || a == "--help") { usage(); return 0; }
		else if (a == "--scale") scale = std::max(1, atoi(next("--scale")));
		else if (a == "--no-rotate") rotation = 0;
		else if (a == "--volume") { volume = (float)atof(next("--volume")); vol_given = true; }
		else if (a == "--lives") lives = atoi(next("--lives"));
		else if (a == "--free-play") free_play = true;
		else if (a == "--bonus") bonus = atoi(next("--bonus"));
		else if (a == "--difficulty") difficulty = atoi(next("--difficulty"));
		else if (a == "--sound-test") sound_only = true;
		else if (a == "--headless") headless = true;
		else if (a == "--frames") frames_to_run = atoi(next("--frames"));
		else if (a == "--wav") wav_path = next("--wav");
		else if (a == "--shot") shot_path = next("--shot");
		else if (a == "--shot-every") shot_every = atoi(next("--shot-every"));
		else if (a == "--coin-at") coin_at = atoi(next("--coin-at"));
		else if (a == "--start-at") start_at = atoi(next("--start-at"));
		else if (a == "--menu-shot") { std::string v = next("--menu-shot"); size_t c = v.find(':'); dev_page = atoi(v.c_str()); if (c != std::string::npos) dev_out = v.substr(c + 1); }
		else if (a == "--crt") dev_crt = atoi(next("--crt"));
		else if (a == "--stats") dev_stats = true;
		else if (a == "--keys") { std::string v = next("--keys"); size_t p = 0; while (p <= v.size()) { size_t e = v.find(',', p); if (e == std::string::npos) e = v.size(); if (e > p) keyscript.push_back(v.substr(p, e - p)); p = e + 1; } }
		else if (a == "--poke") { std::pair<uint16_t, uint8_t> pk; if (parse_poke(next("--poke"), pk)) cli_pokes.push_back(pk); else { fprintf(stderr, "--poke wants ADDR:VALUE in hex (0000-7FFF patch the ROM, 8000-8FFF lock RAM)\n"); return 2; } }
		else if (a == "--font") { std::string v = next("--font"); font_cli = v == "rom" ? 1 : v == "builtin" ? 2 : 0; font_cli_set = true; }
		else if (a == "--watch") { watch_cli.push_back((uint16_t)strtol(next("--watch"), nullptr, 16)); }
		else if (a == "--inp") { InpEv e; if (sscanf(next("--inp"), "%d:%d:%x", &e.f0, &e.f1, &e.bits) == 3) inps.push_back(e); }
		else if (a == "--poke-at") { int f, ad, v; if (sscanf(next("--poke-at"), "%d:%x:%x", &f, &ad, &v) == 3) pokeat.push_back({f, ad, v}); }
		else if (a == "--skip-at") { int f, d; if (sscanf(next("--skip-at"), "%d:%d", &f, &d) == 2) skipat.push_back({f, d, 0}); }
		else if (a == "--dump-ram") dump_ram = next("--dump-ram");
		else if (a == "--dump-font") dump_font = next("--dump-font");
		else if (a == "--vol-psg") vol_cli[0] = (float)atof(next("--vol-psg"));
		else if (a == "--vol-drums") vol_cli[1] = (float)atof(next("--vol-drums"));
		else if (a == "--bass-edge") bass_edge_cli = atoi(next("--bass-edge"));
		else if (a == "--vol-bass") vol_bass_cli = (float)atof(next("--vol-bass"));
		else if (a == "--vol-gorilla") vol_cli[2] = (float)atof(next("--vol-gorilla"));
		else if (a == "--cmd") {
			double t; int c;
			if (sscanf(next("--cmd"), "%lf:%i", &t, &c) == 2) cmds.push_back({t, c, false});
			else { fprintf(stderr, "bad --cmd, use TIME:COMMAND\n"); return 1; }
		}
		else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 1; }
		else paths.push_back(a);
	}

	/* ---- find and load the ROMs ---- */
	std::vector<RomFile> files;
	if (paths.empty()) {
		const char *def[] = { "roms", "." };
		for (const char *d : def) add_path(d, files);
		const char *bp = SDL_GetBasePath();
		if (bp) { add_path(std::string(bp) + "roms", files); add_path(bp, files); }
	} else for (auto &p : paths) add_path(p, files);

	static RomSet rs;
	if (!load_roms(files, rs, sound_only)) {
		fprintf(stderr,
			"\nCould not find all the ROMs. Put the MAME congo.zip (and, for the tiptop set,\n"
			"tiptop.zip as well) in a folder called 'roms' next to the program, or pass the\n"
			"zip / folder on the command line.  For sound only, try --sound-test.\n");
		return 1;
	}

	Machine &m = M;
	m.sound_only = sound_only;
	m.volume = volume;
	memcpy(m.srom, rs.d[R_SND].data(), 0x2000);
	if (!sound_only) {
		for (int i = 0; i < 4; i++) memcpy(m.mrom + i * 0x2000, rs.d[R_M1 + i].data(), 0x2000);
		m.decode_gfx(rs);
	}
	/* DIP switches */
	{
		int b = 0x03;
		if (bonus == 20000) b = 0x01; else if (bonus == 30000) b = 0x02; else if (bonus == 40000) b = 0x00;
		int d = difficulty == 0 ? 0x0c : difficulty == 2 ? 0x08 : difficulty == 3 ? 0x00 : 0x04;
		int l = free_play ? 0x00 : lives == 4 ? 0x10 : lives == 5 ? 0x20 : 0x30;
		m.dsw02 = (uint8_t)(b | d | l | 0x40);
	}
	m.reset();
	m.pokes = cli_pokes; m.sync_locks();
	if (bass_edge_cli >= 0) m.bass_soft = bass_edge_cli != 0;
	if (vol_bass_cli >= 0) m.vol_bass = vol_bass_cli;
	if (vol_cli[0] >= 0) m.vol_psg = vol_cli[0];
	if (vol_cli[1] >= 0) m.vol_perc = vol_cli[1];
	if (vol_cli[2] >= 0) m.vol_gor = vol_cli[2];

	/* ---- menu font: font.bmp > character ROM > built-in ---- */
	static MenuFont mfont;
	auto build_font = [&](int mode, int fa, int fd) {
		memset(mfont.g, 0, sizeof mfont.g);
		builtin_glyphs(mfont.g);
		snprintf(mfont.info, sizeof mfont.info, "menu font: built-in");
		if (sound_only) return;
		if (mode == 0) {
			const char *bp = SDL_GetBasePath();
			std::string cands[2] = { std::string(bp ? bp : "") + "font.bmp", "font.bmp" };
			for (auto &c : cands)
				if (load_font_bmp(c, mfont.g)) { snprintf(mfont.info, sizeof mfont.info, "menu font: %s", c.c_str()); return; }
		}
		if (mode != 2) {
			uint8_t tmp[128][8]; memcpy(tmp, mfont.g, sizeof tmp);
			double conf = 0;
			if (detect_rom_font(m.txpix, fa, fd, tmp, mfont.info, sizeof mfont.info, &conf)) memcpy(mfont.g, tmp, sizeof tmp);
		}
	};
	if (!dump_font.empty()) {
		SDL_Init(SDL_INIT_VIDEO);
		Settings ds; settings_load(ds);
		build_font(font_cli_set ? font_cli : ds.font_mode, ds.font_alpha, ds.font_digit);
		fprintf(stderr, "%s\n", mfont.info);
		/* sheet: ASCII 32..127, 16 per row, 4x enlarged, plus the raw 256 character-ROM tiles below */
		const int S = 4, W = 16 * 8 * S, H = (6 * 8 + 4 + 16 * 8) * S;
		std::vector<uint32_t> img((size_t)W * H, 0xff202020u);
		for (int c = 32; c < 128; c++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
			if ((mfont.g[c][y] >> (7 - x)) & 1)
				for (int yy = 0; yy < S; yy++) for (int xx = 0; xx < S; xx++)
					img[(size_t)(((c - 32) >> 4) * 8 * S + y * S + yy) * W + ((c & 15) * 8 + x) * S + xx] = 0xffffff00u;
		for (int t = 0; t < 256 && !sound_only; t++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
			int p = m.txpix[t * 64 + y * 8 + x]; if (!p) continue;
			uint32_t col = p == 1 ? 0xff80ff80u : p == 2 ? 0xffff8080u : 0xffffffffu;
			for (int yy = 0; yy < S; yy++) for (int xx = 0; xx < S; xx++)
				img[(size_t)((52 + (t >> 4) * 8 + y) * S + yy) * W + ((t & 15) * 8 + x) * S + xx] = col;
		}
		write_bmp(dump_font.c_str(), img.data(), W, H);
		return 0;
	}

	/* ================================================================= */
	/*  headless mode                                                    */
	/* ================================================================= */
	if (headless) {
		m.watch = watch_cli; bool crash_seen = false;
		std::vector<int16_t> all;
		std::vector<uint32_t> view; int w, h;
		if (sound_only) cmds.insert(cmds.begin(), Cmd{0.1, 0x30, false});
		for (int f = 0; f < frames_to_run; f++) {
			double t = (double)f / 60.0;
			for (auto &c : cmds) if (!c.done && t >= c.t) { m.ppi.latch = (uint8_t)c.cmd; c.done = true; }
			m.sw00 = m.sw01 = 0; m.sw100 = 0;
			if (f == coin_at && (m.l0 & 1)) m.coin_status[0] = 1;
			if (coin_at >= 0 && f > coin_at && f < coin_at + 3 && (m.l0 & 1)) m.coin_status[0] = 1;
			if (start_at >= 0 && f >= start_at && f < start_at + 10) m.sw100 |= 0x04;
			for (auto &p : skipat) if (f == p.f) fprintf(stderr, "skip f=%d dir=%d -> %s\n", f, p.a, m.request_skip(p.a) ? "ok" : "refused");
			for (auto &p : pokeat) if (f == p.f) m.mram[p.a & 0xfff] = (uint8_t)p.v;
			for (auto &e : inps) if (f >= e.f0 && f <= e.f1) { m.sw00 |= (uint8_t)(e.bits & 0xff); m.sw100 |= (uint8_t)(e.bits >> 8); }
			m.run_frame();
			if (!watch_cli.empty() && m.main.pc.w >= 0x8000 && !crash_seen) { crash_seen = true; fprintf(stderr, "CRASH f=%d pc=%04X sp=%04X\n", f, m.main.pc.w, m.main.sp.w); }
			all.insert(all.end(), m.audio.begin(), m.audio.end());
			if (!sound_only && shot_every > 0 && !shot_path.empty() && f % shot_every == 0) {
				make_view(m.fb, rotation, view, w, h);
				char n[512]; snprintf(n, sizeof n, "%s-%d.bmp", shot_path.c_str(), f);
				write_bmp(n, view.data(), w, h);
			}
		}
		if (!watch_cli.empty()) fprintf(stderr, "end: pc=%04X sp=%04X 800A=%02X 8009=%02X 8008=%02X int_en=%d iff=%d halted=%d\n", m.main.pc.w, m.main.sp.w, m.mram[0xA], m.mram[9], m.mram[8], m.int_en, m.main.iff1, m.main.halted);
		if (!dump_ram.empty()) { std::string vn = dump_ram + ".vram"; FILE *vf = fopen(vn.c_str(), "wb"); if (vf) { fwrite(m.vram, 1, 0x400, vf); fclose(vf); } }
		if (!dump_ram.empty()) { FILE *df = fopen(dump_ram.c_str(), "wb"); if (df) { fwrite(m.mram, 1, 0x1000, df); fclose(df); } }
		if (!shot_path.empty() && !sound_only) {
			make_view(m.fb, rotation, view, w, h);
			write_bmp(shot_path.c_str(), view.data(), w, h);
		}
		if (!wav_path.empty()) write_wav(wav_path.c_str(), all);
		fprintf(stderr, "ran %d frames\n", frames_to_run);
		return 0;
	}

	/* ================================================================= */
	/*  interactive mode                                                 */
	/* ================================================================= */
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
	const bool pad_ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);      /* controllers are optional */

	Settings st;
	settings_load(st);
	if (rotation == 0) st.rotate = 0;
	if (vol_given) st.vol_master = std::max(0, std::min(200, (int)std::lround(volume * 100.0f)));
	if (dev_crt >= 0) st.crt = dev_crt;
	if (vol_cli[0] >= 0) st.vol_psg = (int)std::lround(vol_cli[0] * 100);
	if (vol_cli[1] >= 0) st.vol_perc = (int)std::lround(vol_cli[1] * 100);
	if (vol_cli[2] >= 0) st.vol_gor = (int)std::lround(vol_cli[2] * 100);
	rotation = st.rotate;
	int last_treble = -1;
	auto apply_audio = [&]() {
		m.volume = st.vol_master / 100.0f;
		m.vol_psg = st.vol_psg / 100.0; m.vol_perc = st.vol_perc / 100.0; m.vol_gor = st.vol_gor / 100.0; m.vol_bass = st.vol_bass / 100.0; m.bass_soft = st.bass_soft != 0;
		if (st.treble != last_treble) { m.set_treble(st.treble); last_treble = st.treble; }
	};
	apply_audio();

	int vw = rotation ? SCR_H : SCR_W, vh = rotation ? SCR_W : SCR_H;
	SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");            /* let the window take the focus even when started from a terminal */
	SDL_Window *win = SDL_CreateWindow(sound_only ? "Congo Bongo - sound test" : "Congo Bongo", vw * (scale > 0 ? scale : 3), vh * (scale > 0 ? scale : 3),
	                                   SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | (st.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
	if (!win || !ren) { fprintf(stderr, "window/renderer: %s\n", SDL_GetError()); return 1; }
	SDL_SetRenderVSync(ren, 1);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
	SDL_Texture *tex_crt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 768, 768);
	if (!tex || !tex_crt) { fprintf(stderr, "texture: %s\n", SDL_GetError()); return 1; }
	SDL_SetTextureScaleMode(tex_crt, SDL_SCALEMODE_LINEAR);
	if (font_cli_set) st.font_mode = font_cli;
	build_font(st.font_mode, st.font_alpha, st.font_digit);
	fprintf(stderr, "%s\n", mfont.info);
	mfont.atlas = make_font_atlas(ren, mfont.g);
	SDL_Texture *logo_tex = nullptr; int logo_w = 1, logo_h = 1, logo_px = 0;   /* vector logo, re-rendered when the output size changes */
	auto apply_view = [&]() {
		vw = rotation ? SCR_H : SCR_W; vh = rotation ? SCR_W : SCR_H;
		SDL_SetRenderLogicalPresentation(ren, vw, vh,
			st.integer_scale ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetTextureScaleMode(tex, st.integer_scale ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
	};
	apply_view();

	/* window size: AUTO = the largest whole-number zoom that fits the usable desktop (taskbar and window borders
	 * excluded), otherwise the chosen 1x, 2x, ... (never bigger than the desktop) */
	auto max_window_scale = [&]() -> int {
		SDL_Rect ub; const SDL_DisplayID did = SDL_GetDisplayForWindow(win);
		if (!did || !SDL_GetDisplayUsableBounds(did, &ub) || ub.w <= 0 || ub.h <= 0) return 3;
		int bt = 0, bl = 0, bb = 0, br = 0;
		if (!SDL_GetWindowBordersSize(win, &bt, &bl, &bb, &br)) bt = 32;
		const int aw = ub.w - bl - br - 8, ah = ub.h - bt - bb - 8;
		return std::max(1, std::min(aw / vw, ah / vh));
	};
	auto apply_window_size = [&]() {
		if (st.fullscreen) return;
		int s;
		if (scale > 0) s = scale;                                   /* --scale: exactly what was asked */
		else { const int ms = max_window_scale(); s = st.window_scale > 0 ? std::min(st.window_scale, ms) : ms; }
		SDL_SetWindowSize(win, vw * s, vh * s);
		SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	};
	apply_window_size();
	SDL_ShowWindow(win); SDL_RaiseWindow(win);
	SDL_SyncWindow(win);
	apply_window_size();           /* again now that the border sizes are known */

	SDL_AudioSpec spec; SDL_zero(spec);
	spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = RATE;
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
	SDL_AudioStream *as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (!as) fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	/* the device starts paused; it is resumed after the queue has been pre-filled (see below), so the
	 * first buffers are never starved (that used to pop right at start-up) */
	const int Q_HIGH = 2000 * 2, Q_LOW = 1100 * 2;     /* bytes: 2000 / 1100 samples = 42 / 23 ms */

	int test_cmd = 0x91;
	bool paused = false, running = true;
	std::vector<SDL_Gamepad *> pads;
	auto open_pad = [&](SDL_JoystickID id) {
		SDL_Gamepad *g = SDL_OpenGamepad(id);
		if (g) { pads.push_back(g); fprintf(stderr, "controller: %s\n", SDL_GetGamepadName(g)); }
	};
	if (pad_ok) {
		int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
		for (int i = 0; ids && i < n; i++) open_pad(ids[i]);
		SDL_free(ids);
	}
	/* digital direction of a controller: d-pad or left stick (dead zone 45%) */
	auto pad_dir = [&](int &dx, int &dy) {
		dx = dy = 0;
		for (SDL_Gamepad *g : pads) {
			const int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
			if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || ax < -14500) dx = -1;
			if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > 14500) dx = 1;
			if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || ay < -14500) dy = -1;
			if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ay > 14500) dy = 1;
		}
	};
	Uint64 pad_next_ms = 0; int pad_held = 0;
	bool prev_coin = false;
	int shot_n = 0;
	Uint64 next_ns = SDL_GetTicksNS();
	std::vector<uint32_t> view, crtbuf;
	int vw_px = 0, vh_px = 0;
	bool view_dirty = true;
	if (sound_only) { m.ppi.latch = 0x30; }   /* the sound program waits for 0x30 before it un-mutes */

	auto refresh_view = [&]() { make_view(m.fb, rotation, view, vw_px, vh_px); view_dirty = true; };
	refresh_view();

	auto set_title = [&]() {
		if (!sound_only) return;
		char t[160];
		snprintf(t, sizeof t, "Congo Bongo sound test - command %02X  (arrows: change, Enter: play, 0: init 30)", test_cmd);
		SDL_SetWindowTitle(win, t);
	};
	set_title();

	if (as) {
		m.sw00 = m.sw01 = 0; m.sw100 = 0;
		for (int i = 0; i < 3; i++) { m.run_frame(); SDL_PutAudioStreamData(as, m.audio.data(), (int)(m.audio.size() * sizeof(int16_t))); }
		SDL_ResumeAudioStreamDevice(as);
	}

	/* ---------------- options menu ---------------- */
	enum { PG_MAIN, PG_VIDEO, PG_AUDIO, PG_CONTROLS, PG_CHEATS, PG_FIND, PG_RESULTS, PG_CRT };
	std::vector<Cheat> cheats;
	/* built-in cheats: the addresses are from the MAME cheat collection (mamecheat.co.uk, congo.xml) */
	{
		struct B { const char *name; uint16_t addr; uint8_t val; const char *desc; };
		static const B builtin[] = {
			{ "INFINITE CREDITS", 0x8012, 0x99, "CREDIT COUNTER STAYS AT 99" },
			{ "INFINITE LIVES",   0x8220, 0x06, "LIVES STAY AT 6" },
			{ "INFINITE BONUS",   0x8304, 0x99, "BONUS TIMER NEVER RUNS DOWN" },
		};
		for (auto &b : builtin) { Cheat c; c.name = b.name; c.pk.push_back({ b.addr, b.val }); c.desc = b.desc; cheats.push_back(c); }
		/* Found by tracing the real program (rev C): byte 840D is the player's collision result.  1 = killed, 4 = hit
		 * (dies a frame later); 2, 3 and 5 mean "standing on a hippo, fish, log or lily pad" and must stay.  The two
		 * handlers at 1DDF and 1DF9 are patched to clear the byte and carry on as if nothing was touched, so the
		 * platforms still work.  Same idea for drowning: the handler at 1E45 (player state 2: fell in the water or
		 * off a ledge) clears the state and the hunter just keeps walking. */
	/* the built-in ROM patches are written for the congo rev C program; the "tiptop" set has other code */
		const bool known_rom = m.mrom_orig[0x1DDF] == 0x3E && m.mrom_orig[0x1DE0] == 0x00 && m.mrom_orig[0x1DF9] == 0xAF &&
		                       m.mrom_orig[0x1E45] == 0x3E && m.mrom_orig[0x1E46] == 0x01 && m.mrom_orig[0x1DC9] == 0xEF;
		static const uint8_t NOHIT[7] = { 0xAF, 0x32, 0x0D, 0x84, 0xC3, 0x0E, 0x1E };          /* XOR A; LD (840D),A; JP 1E0E */
		static const uint8_t NOFALL[7] = { 0xAF, 0x32, 0x17, 0x84, 0xC3, 0xCD, 0x1E };         /* XOR A; LD (8417),A; JP 1ECD */
		Cheat inv; inv.name = "INVINCIBLE"; inv.desc = "ENEMIES CAN'T KILL. RIDING STILL WORKS";
		for (int i = 0; i < 7; i++) { inv.pk.push_back({ (uint16_t)(0x1DDF + i), NOHIT[i] }); inv.pk.push_back({ (uint16_t)(0x1DF9 + i), NOHIT[i] }); }
		if (known_rom) cheats.push_back(inv);
		Cheat wow; wow.name = "WALK ON WATER"; wow.desc = "NO DROWNING OR FALLING";
		for (int i = 0; i < 7; i++) wow.pk.push_back({ (uint16_t)(0x1E45 + i), NOFALL[i] });
		if (known_rom) cheats.push_back(wow);
	}
	cheats_load(cheats);
	/* RAM finder (like MAME's cheat search) over the 4 KB of main RAM */
	std::vector<uint8_t> snap(0x1000);
	std::vector<uint16_t> cand;
	bool find_started = false;
	int find_val = 0, lock_val = 0;
	char find_msg[40] = "";
	auto rebuild_pokes = [&]() {
		m.pokes = cli_pokes;
		for (auto &c : cheats) if (c.on) m.pokes.insert(m.pokes.end(), c.pk.begin(), c.pk.end());
		m.sync_locks();
	};
	rebuild_pokes();
	static const int TEST_CMDS[] = {
		0x91, 0x92, 0x18,                                   /* gorilla */
		0x07, 0x08, 0x09, 0x0A, 0x1A, 0x1B, 0x1C, 0x1D,    /* drum patterns */
		0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
		0x16, 0x17, 0x19, 0x20, 0x21, 0x31, 0x54, 0x55, 0x56 /* music / jingles */
	};
	const int N_TEST = (int)(sizeof TEST_CMDS / sizeof TEST_CMDS[0]);
	bool menu = false;
	int page = PG_MAIN, sel = 0, test_idx = 0;
	char cheat_msg[40] = "";
	int test_stage = 0;           /* 0 idle, >0 frames until the test command is sent after a stop */
	int ctl_view = 0;             /* controls page: 0 keyboard, 1 controller */
	double fps = 0; int fps_frames = 0; Uint64 fps_t0 = SDL_GetTicks();

	auto page_count = [&](int p) {
		switch (p) {
		case PG_MAIN: return 7;
		case PG_VIDEO: return 9;
		case PG_CRT: return 9;
		case PG_AUDIO: return 9;
		case PG_CHEATS: return (int)cheats.size() + 4;
		case PG_FIND: return 9;
		case PG_RESULTS: return std::max(1, std::min((int)cand.size(), 999));
		}
		return 1;
	};
	auto open_menu = [&]() {      /* the menu comes back on the page it was left on */
		menu = true; sel = std::max(0, std::min(sel, page_count(page) - 1)); if (as) SDL_ClearAudioStream(as);
	};
	auto close_menu = [&]() {
		menu = false; settings_save(st);
		if (page == PG_AUDIO) m.ppi.latch = 0x30;       /* idle command: stops a test sound (0xFF would mute the board for good) */
		if (as) SDL_ClearAudioStream(as);
	};
	auto set_fullscreen = [&](int on) { st.fullscreen = on; SDL_SetWindowFullscreen(win, on != 0); if (!on) { SDL_SyncWindow(win); apply_window_size(); } };
	auto set_rotate = [&](int on) { st.rotate = on; rotation = on; apply_view(); refresh_view(); apply_window_size(); };
	auto change_page = [&](int p, int newsel = 0) {
		if (page == PG_AUDIO && p != PG_AUDIO) m.ppi.latch = 0x30;
		page = p; sel = newsel; cheat_msg[0] = 0;
	};
	auto menu_back = [&]() {      /* one level up, with the entry we came from highlighted */
		switch (page) {
		case PG_MAIN: close_menu(); break;
		case PG_VIDEO: change_page(PG_MAIN, 1); break;
		case PG_AUDIO: change_page(PG_MAIN, 2); break;
		case PG_CONTROLS: change_page(PG_MAIN, 3); break;
		case PG_CHEATS: change_page(PG_MAIN, 4); break;
		case PG_FIND: change_page(PG_CHEATS, (int)cheats.size() + 2); break;
		case PG_RESULTS: change_page(PG_FIND, 7); break;
		case PG_CRT: change_page(PG_VIDEO, 5); break;
		default: change_page(PG_MAIN); break;
		}
	};
	auto menu_key = [&](SDL_Keycode key, bool repeat) {
		const int n = page_count(page);
		if (key == SDLK_UP || key == SDLK_DOWN) {
			sel = (sel + n + (key == SDLK_UP ? -1 : 1)) % n;
			if (page == PG_RESULTS && !cand.empty()) lock_val = m.mram[cand[std::min(sel, (int)cand.size() - 1)]];
			return;
		}
		int d = key == SDLK_LEFT ? -1 : key == SDLK_RIGHT ? 1 : 0;
		bool act = !repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_Z);
		if (key == SDLK_ESCAPE && !repeat) { close_menu(); return; }          /* Esc flips in and out of the game */
		if (key == SDLK_BACKSPACE && !repeat) { menu_back(); return; }       /* Backspace goes up one level */
		if (!d && !act) return;
		const int dd = d ? d : 1;
		auto vol = [&](int &v) { v = std::max(0, std::min(200, v + 10 * dd)); apply_audio(); };
		if (page == PG_MAIN) {
			if (!act) return;
			switch (sel) {
			case 0: close_menu(); break;
			case 1: change_page(PG_VIDEO); break;
			case 2: change_page(PG_AUDIO); break;
			case 3: change_page(PG_CONTROLS); break;
			case 4: change_page(PG_CHEATS); break;
			case 5: m.reset(); refresh_view(); close_menu(); break;
			case 6: running = false; break;
			}
		} else if (page == PG_VIDEO) {
			switch (sel) {
			case 0: set_fullscreen(!st.fullscreen); break;
			case 1: st.integer_scale ^= 1; apply_view(); view_dirty = true; break;
			case 2: {
				const int ms = std::min(9, max_window_scale()), cur = std::min(st.window_scale, ms);
				st.window_scale = (cur + ms + 1 + dd) % (ms + 1);          /* 0 = AUTO, 1..ms */
				apply_window_size(); break; }
			case 3: st.crt = (st.crt + 4 + dd) % 4; view_dirty = true; break;
			case 4: st.crt_look = (st.crt_look + 5 + dd) % 5; view_dirty = true; break;
			case 5: if (act) { if (st.crt < 2) st.crt = 3; view_dirty = true; change_page(PG_CRT); } break;
			case 6: set_rotate(!st.rotate); break;
			case 7: st.show_fps ^= 1; break;
			case 8: if (act) change_page(PG_MAIN, 1); break;
			}
		} else if (page == PG_CRT) {
			auto adj = [&](int CrtParams::*mp, int lo, int hi) {
				if (st.crt_look != 4) { st.crt_custom = crt_params(st); st.crt_look = 4; }
				int &v = st.crt_custom.*mp; v = std::max(lo, std::min(hi, v + 5 * dd)); view_dirty = true;
			};
			switch (sel) {
			case 0: st.crt_look = (st.crt_look + 5 + dd) % 5; view_dirty = true; break;
			case 1: adj(&CrtParams::curve, 0, 100); break;
			case 2: adj(&CrtParams::corner, 0, 100); break;
			case 3: adj(&CrtParams::scan, 0, 100); break;
			case 4: adj(&CrtParams::mask, 0, 100); break;
			case 5: adj(&CrtParams::glow, 0, 100); break;
			case 6: adj(&CrtParams::vig, 0, 100); break;
			case 7: adj(&CrtParams::bright, 50, 150); break;
			case 8: if (act) change_page(PG_VIDEO); break;
			}
		} else if (page == PG_AUDIO) {
			switch (sel) {
			case 0: vol(st.vol_master); break;
			case 1: vol(st.vol_psg); break;
			case 2: vol(st.vol_perc); break;
			case 3: vol(st.vol_bass); break;
			case 4: vol(st.vol_gor); break;
			case 5: st.treble = (st.treble + 5 + dd) % 5; apply_audio(); break;
			case 6: st.bass_soft ^= 1; apply_audio(); break;
			case 7:
				if (d) test_idx = (test_idx + N_TEST + d) % N_TEST;
				else { m.ppi.latch = 0x30; test_stage = 12; }     /* stop (command 0x30), then play */
				break;
			case 8: if (act) change_page(PG_MAIN); break;
			}
		} else if (page == PG_CHEATS) {
			const int nc = (int)cheats.size();
			if (sel < nc) { cheats[sel].on = !cheats[sel].on; rebuild_pokes(); }
			else if (sel == nc || sel == nc + 1) {
				if (act) {
					if (m.request_skip(sel == nc ? 1 : -1)) close_menu();
					else snprintf(cheat_msg, sizeof cheat_msg, "START A GAME FIRST");
				}
			}
			else if (sel == nc + 2) { if (act) change_page(PG_FIND); }
			else if (act) change_page(PG_MAIN, 4);
		} else if (page == PG_FIND) {
			auto count_msg = [&]() { snprintf(find_msg, sizeof find_msg, "%d LEFT", (int)cand.size()); };
			auto filter = [&](auto keep) {
				if (!find_started) { snprintf(find_msg, sizeof find_msg, "START A NEW SEARCH FIRST"); return; }
				std::vector<uint16_t> n;
				for (uint16_t i : cand) if (keep(m.mram[i], snap[i])) n.push_back(i);
				cand.swap(n);
				memcpy(snap.data(), m.mram, 0x1000);
				count_msg();
			};
			switch (sel) {
			case 0:
				if (!act) break;
				cand.clear(); for (int i = 0; i < 0x1000; i++) cand.push_back((uint16_t)i);
				memcpy(snap.data(), m.mram, 0x1000); find_started = true;
				snprintf(find_msg, sizeof find_msg, "4096 BYTES. PLAY, THEN FILTER"); break;
			case 1: find_val = (find_val + 256 + dd) & 255; break;
			case 2: if (act) filter([&](uint8_t cur, uint8_t) { return cur == find_val; }); break;
			case 3: if (act) filter([](uint8_t cur, uint8_t old) { return cur < old; }); break;
			case 4: if (act) filter([](uint8_t cur, uint8_t old) { return cur > old; }); break;
			case 5: if (act) filter([](uint8_t cur, uint8_t old) { return cur != old; }); break;
			case 6: if (act) filter([](uint8_t cur, uint8_t old) { return cur == old; }); break;
			case 7: if (act && find_started && !cand.empty()) { change_page(PG_RESULTS); lock_val = m.mram[cand[0]]; } break;
			case 8: if (act) change_page(PG_CHEATS); break;
			}
		} else if (page == PG_RESULTS) {
			if (!cand.empty() && sel < (int)cand.size()) {
				if (d) lock_val = (lock_val + 256 + dd) & 255;
				else if (act) {
					Cheat c; char nm[24]; snprintf(nm, sizeof nm, "FOUND %04X", 0x8000 + cand[sel]);
					c.name = nm; c.pk.push_back({ (uint16_t)(0x8000 + cand[sel]), (uint8_t)lock_val }); c.on = true;
					cheats.push_back(c); cheat_append(c); rebuild_pokes();
					snprintf(find_msg, sizeof find_msg, "ADDED %04X = %02X", 0x8000 + cand[sel], lock_val);
				}
			}
		} else if (page == PG_CONTROLS && d) { ctl_view ^= 1; }
		else if (act) menu_back();
	};

	auto text = [&](float x, float y, float sc, Uint8 r, Uint8 g, Uint8 b, const char *str) {
		if (!mfont.atlas) return;
		SDL_SetTextureColorMod(mfont.atlas, r, g, b);
		const float cw = 8.0f * sc;
		const float x0 = std::floor(x), y0 = std::floor(y);
		for (int i = 0; str[i]; i++) {
			unsigned c = (unsigned char)str[i]; if (c > 127 || c == ' ') continue;
			SDL_FRect src = { (float)((c & 15) * 8), (float)((c >> 4) * 8), 8, 8 };
			SDL_FRect dst = { x0 + i * cw, y0, cw, cw };
			SDL_RenderTexture(ren, mfont.atlas, &src, &dst);
		}
	};
	auto text_w = [](const char *str, float sc) { return (float)strlen(str) * 8.0f * sc; };
	auto cursor = [&](float x, float y, float sz) {
		SDL_Vertex v[3]; SDL_FColor c = { 1.0f, 1.0f, 0.0f, 1.0f };
		v[0].position = { x, y };           v[1].position = { x, y + sz };
		v[2].position = { x + sz * 0.85f, y + sz * 0.5f };
		for (auto &p : v) { p.color = c; p.tex_coord = { 0, 0 }; }
		SDL_RenderGeometry(ren, nullptr, v, 3, nullptr, 0);
	};
	auto draw_menu = [&]() {
		if (page == PG_CRT) {      /* a low panel so the picture stays visible while tuning */
			const float W = (float)vw, H = (float)vh, step = 11.0f, ph = 9 * step + 30, py = H - ph;
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(ren, 0, 0, 0, 220);
			SDL_FRect pr = { 0, py, W, ph };
			SDL_RenderFillRect(ren, &pr);
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
			text((W - text_w("CRT TUNING", 1)) / 2, py + 4, 1, 255, 255, 0, "CRT TUNING");
			const CrtParams cp = crt_params(st);
			static const char *lab[9] = { "PRESET", "CURVATURE", "CORNERS", "SCANLINES", "MASK", "GLOW", "VIGNETTE", "BRIGHTNESS", "BACK" };
			for (int i = 0; i < 9; i++) {
				char v[16] = "";
				switch (i) {
				case 0: snprintf(v, sizeof v, "%s", CRT_LOOK_NAMES[st.crt_look]); break;
				case 1: snprintf(v, sizeof v, "%d", cp.curve); break;
				case 2: snprintf(v, sizeof v, "%d", cp.corner); break;
				case 3: snprintf(v, sizeof v, "%d", cp.scan); break;
				case 4: snprintf(v, sizeof v, "%d", cp.mask); break;
				case 5: snprintf(v, sizeof v, "%d", cp.glow); break;
				case 6: snprintf(v, sizeof v, "%d", cp.vig); break;
				case 7: snprintf(v, sizeof v, "%d%%", cp.bright); break;
				}
				const bool on = i == sel; const float y = py + 18 + i * step;
				if (on) cursor(8, y, 8);
				text(22, y, 1, on ? 255 : 222, on ? 255 : 222, on ? 0 : 255, lab[i]);
				if (v[0]) text(W - 10 - text_w(v, 1), y, 1, on ? 255 : 150, 255, on ? 0 : 150, v);
			}
			text(2, H - 9, 1, 150, 150, 170, st.crt == 3 ? "LEFT/RIGHT CHANGES" : "CURVE NEEDS CURVED CRT");
			return;
		}
		SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 205);
		SDL_RenderFillRect(ren, nullptr);
		SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
		static const char *TITLES[] = { "CONGO BONGO", "VIDEO", "AUDIO", "CONTROLS", "CHEATS", "RAM FINDER", "RESULTS", "CRT TUNING" };
		const float W = (float)vw, H = (float)vh;
		const char *title = TITLES[page];
		float top = 10;                                   /* first free line below the header */
		{
			const float lwl = std::min(W - 20.0f, 200.0f);
			SDL_FRect pr = { 0, 0, W, H }; float sc = 1.0f;
			if (SDL_GetRenderLogicalPresentationRect(ren, &pr) && pr.w > 0) sc = pr.w / W;
			const int want = std::max(64, std::min(2400, (int)std::lround(lwl * sc)));
			if (want != logo_px || !logo_tex) {
				std::vector<uint32_t> lp; logo_render(want, lp, logo_w, logo_h);
				if (logo_tex) SDL_DestroyTexture(logo_tex);
				logo_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, logo_w, logo_h);
				if (logo_tex) {
					SDL_UpdateTexture(logo_tex, nullptr, lp.data(), logo_w * 4);
					SDL_SetTextureBlendMode(logo_tex, SDL_BLENDMODE_BLEND);
					SDL_SetTextureScaleMode(logo_tex, SDL_SCALEMODE_LINEAR);
				}
				logo_px = want;
			}
		}
		if (logo_tex) {
			const float lw = std::min(W - 20.0f, 200.0f), lh = lw * logo_h / logo_w;
			SDL_FRect dst = { std::floor((W - lw) / 2), 8, lw, lh };
			SDL_RenderTexture(ren, logo_tex, nullptr, &dst);
			top = 8 + lh + 6;
		} else if (page == PG_MAIN) { text((W - text_w(title, 1)) / 2, 16, 1, 255, 255, 0, title); top = 30; }
		if (page != PG_MAIN) { text((W - text_w(title, 1)) / 2, top, 1, 255, 255, 0, title); top += 16; }
		else top += 6;

		char buf[96]; const char *desc = "";
		if (page == PG_MAIN) {
			static const char *items[] = { "PLAY", "VIDEO", "AUDIO", "CONTROLS", "CHEATS", "RESET GAME", "QUIT" };
			static const char *descs[] = { "CLOSE THE MENU", "WINDOW, SCALING AND CRT", "SET THE VOLUMES", "KEYS AND JOYSTICK",
			                               "CHEATS AND RAM FINDER", "START THE GAME OVER", "EXIT THE EMULATOR" };
			float step = std::min(16.0f, (H - top - 44) / 7.0f), y0 = top;
			for (int i = 0; i < 7; i++) {
				bool on = i == sel;
				if (on) cursor(52, y0 + i * step, 8);
				text(66, y0 + i * step, 1, on ? 255 : 222, on ? 255 : 222, on ? 0 : 255, items[i]);
			}
			desc = descs[sel];
		} else if (page == PG_VIDEO || page == PG_AUDIO) {
			struct Row { const char *label; char val[24]; };
			Row rows[9]; const char *descs[9];
			const int nrows = 9;
			auto setv = [&](int i, const char *l, const char *v, const char *d) {
				rows[i].label = l; snprintf(rows[i].val, sizeof rows[i].val, "%s", v); descs[i] = d; };
			if (page == PG_VIDEO) {
				static const char *crtn[] = { "OFF", "SCANLINES", "CRT", "CURVED CRT" };
				setv(0, "FULLSCREEN", st.fullscreen ? "ON" : "OFF", "FULLSCREEN OR WINDOWED");
				setv(1, "INTEGER SCALING", st.integer_scale ? "ON" : "OFF", "SHARP PIXELS, WHOLE ZOOM");
				{ char wsz[24]; if (st.window_scale > 0) snprintf(wsz, sizeof wsz, "%dX", st.window_scale); else snprintf(wsz, sizeof wsz, "AUTO %dX", max_window_scale());
				  setv(2, "WINDOW SIZE", wsz, "AUTO = BIGGEST THAT FITS"); }
				setv(3, "CRT EFFECT", crtn[st.crt], "SCANLINES, MASK, CURVED");
				setv(4, "CRT LOOK", CRT_LOOK_NAMES[st.crt_look], "PRESET FOR CRT AND CURVED CRT");
				setv(5, "CRT TUNING", "", "ADJUST CURVE, LINES, MASK");
				setv(6, "ROTATE VIEW", st.rotate ? "ON" : "OFF", "ON = TALL LIKE THE ARCADE");
				setv(7, "SHOW FPS", st.show_fps ? "ON" : "OFF", "FRAME RATE COUNTER");
				setv(8, "BACK", "", "");
			} else {
				char t[24];
				snprintf(t, sizeof t, "%d%%", st.vol_master); setv(0, "MASTER VOLUME", t, "OVERALL LEVEL");
				snprintf(t, sizeof t, "%d%%", st.vol_psg);    setv(1, "MUSIC AND BEEPS", t, "THE TWO SN76489 CHIPS");
				snprintf(t, sizeof t, "%d%%", st.vol_perc);   setv(2, "DRUMS", t, "BASS, CONGAS, RIM SHOT");
				snprintf(t, sizeof t, "%d%%", st.vol_bass);   setv(3, "BASS DRUM", t, "0 = SILENT, TIMING KEPT");
				snprintf(t, sizeof t, "%d%%", st.vol_gor);    setv(4, "GORILLA", t, "THE GORILLA GROWL");
				static const char *trn[] = { "OFF", "12 KHZ", "8 KHZ", "5 KHZ", "3 KHZ" };
				setv(5, "TREBLE CUT", trn[st.treble], "SOFTENS HARSH HISS");
				setv(6, "BASS EDGE", st.bass_soft ? "SOFT" : "SHARP", "SOFT REMOVES THE CLICK");
				snprintf(t, sizeof t, "%02X", TEST_CMDS[test_idx]);
				setv(7, "TEST SOUND", t, "<> PICK  ENTER PLAYS");
				setv(8, "BACK", "", "");
			}
			const float step = std::min(14.0f, (H - top - 44) / (float)nrows), y0 = top;
			for (int i = 0; i < nrows; i++) {
				bool on = i == sel;
				float y = y0 + i * step;
				if (on) cursor(8, y, 8);
				Uint8 r = on ? 255 : 222, g = on ? 255 : 222, b = on ? 0 : 255;
				text(22, y, 1, r, g, b, rows[i].label);
				if (rows[i].val[0]) text(W - 10 - text_w(rows[i].val, 1), y, 1, on ? 255 : 150, on ? 255 : 255, on ? 0 : 150, rows[i].val);
			}
			desc = descs[sel];
		} else if (page == PG_CHEATS || page == PG_FIND || page == PG_RESULTS) {
			struct Row { char label[40]; char val[24]; };
			std::vector<Row> rows; std::vector<std::string> descs;
			auto add = [&](const char *l, const char *v, const char *d) {
				Row r; snprintf(r.label, sizeof r.label, "%s", l); snprintf(r.val, sizeof r.val, "%s", v); rows.push_back(r); descs.push_back(d); };
			char t[40];
			if (page == PG_CHEATS) {
				for (auto &c : cheats) add(c.name.c_str(), c.on ? "ON" : "OFF", c.desc.empty() ? "FROM CHEATS.TXT" : c.desc.c_str());
				add("NEXT SCENE", "", cheat_msg[0] ? cheat_msg : "SKIP TO THE NEXT SCENE (F5)");
				add("PREVIOUS SCENE", "", cheat_msg[0] ? cheat_msg : "GO BACK A SCENE (F4)");
				add("RAM FINDER", "", "FIND LIVES, TIME, ETC.");
				add("BACK", "", "");
			} else if (page == PG_FIND) {
				add("NEW SEARCH", "", "REMEMBER ALL OF RAM");
				snprintf(t, sizeof t, "%d", find_val);
				add("VALUE", t, "<> CHANGES THE NUMBER");
				add("EQUALS VALUE", "", "KEEP BYTES SET TO VALUE");
				add("DECREASED", "", "LOST A LIFE, TIME WENT DOWN");
				add("INCREASED", "", "GAINED SOMETHING");
				add("CHANGED", "", "ANY DIFFERENCE");
				add("UNCHANGED", "", "SAME AS BEFORE");
				snprintf(t, sizeof t, "%d", find_started ? (int)cand.size() : 0);
				add("SHOW RESULTS", t, "PICK ONE TO LOCK");
				add("BACK", "", "");
			} else {
				for (size_t i = 0; i < cand.size() && i < 999; i++) {
					snprintf(t, sizeof t, "ADDR %04X", 0x8000 + cand[i]);
					char v[24]; snprintf(v, sizeof v, "NOW %02X", m.mram[cand[i]]);
					add(t, v, "");
				}
				if (rows.empty()) add("NO RESULTS", "", "");
			}
			const int n = (int)rows.size(), vis = std::min(n, page == PG_RESULTS ? 8 : 9);
			const float step = std::min(14.0f, (H - top - 44) / 9.0f), y0 = top;
			int top = std::max(0, std::min(n - vis, sel - vis / 2));
			for (int k = 0; k < vis; k++) {
				int i = top + k; bool on = i == sel; float y = y0 + k * step;
				if (on) cursor(8, y, 8);
				Uint8 r = on ? 255 : 222, g = on ? 255 : 222, b = on ? 0 : 255;
				text(22, y, 1, r, g, b, rows[i].label);
				if (rows[i].val[0]) text(W - 10 - text_w(rows[i].val, 1), y, 1, on ? 255 : 150, on ? 255 : 255, on ? 0 : 150, rows[i].val);
			}
			if (n > vis) { snprintf(buf, sizeof buf, "%d/%d", sel + 1, n); text(W - 10 - text_w(buf, 1), top - 16, 1, 150, 150, 170, buf); }
			if (page == PG_RESULTS && !cand.empty()) {
				snprintf(t, sizeof t, "LOCK TO %02X (<> CHANGES)", lock_val);
				text((W - text_w(t, 1)) / 2, H - 42, 1, 255, 255, 0, t);
			}
			desc = descs[std::min(sel, n - 1)].c_str();
			if (find_msg[0] && page != PG_CHEATS) desc = find_msg;
			snprintf(buf, sizeof buf, "%s", desc); desc = buf;
		} else if (page == PG_CONTROLS) {
			static const char *keys[][2] = { { "ARROWS", "MOVE" }, { "Z/CTRL/SPC", "JUMP" }, { "5", "COIN" }, { "1 / 2", "START" },
				{ "ESC", "MENU ON/OFF" }, { "BACKSPACE", "MENU BACK" }, { "P", "PAUSE" }, { "R", "ROTATE" }, { "F3", "RESET" },
				{ "F11", "FULLSCREEN" }, { "F12", "SCREENSHOT" }, { "+ / -", "VOLUME" } };
			static const char *pad[][2] = { { "D-PAD/STICK", "MOVE" }, { "A / B / X", "JUMP" }, { "BACK", "COIN" }, { "START", "1 PLAYER" },
				{ "R BUMPER", "2 PLAYERS" }, { "Y / GUIDE", "MENU ON/OFF" }, { "IN MENU:", "" }, { "D-PAD/STICK", "MOVE, CHANGE" },
				{ "A", "SELECT" }, { "B / BACK", "BACK" }, { "START", "CLOSE MENU" } };
			const bool gp = ctl_view != 0;
			const int nk = gp ? 11 : 12;
			text((W - text_w(gp ? "< CONTROLLER >" : "< KEYBOARD >", 1)) / 2, top - 2, 1, 150, 255, 150, gp ? "< CONTROLLER >" : "< KEYBOARD >");
			const float st0 = std::min(12.0f, (H - top - 56) / (float)nk);
			for (int i = 0; i < nk; i++) {
				float y = top + 12 + i * st0;
				const char **kv = gp ? pad[i] : keys[i];
				text(8, y, 1, 222, 222, 255, kv[0]);
				text(108, y, 1, 255, 255, 0, kv[1]);
			}
			cursor(8, H - 40, 8); text(22, H - 40, 1, 255, 255, 0, "BACK");
			desc = "LEFT/RIGHT SWITCHES THE LIST";
		}
		if (desc[0]) { char d2[96]; snprintf(d2, sizeof d2, "%s", desc); text((W - text_w(d2, 1)) / 2, H - 28, 1, 120, 255, 120, d2); }
		text(2, H - 10, 1, 150, 150, 170, "ARROWS ENTER BKSP ESC");
	};

	auto draw_frame = [&](bool with_menu) {
		if (view_dirty) {
			if (st.crt) {
				crt_filter(view.data(), vw_px, vh_px, st.crt, crt_params(st), crtbuf);
				SDL_Rect r = { 0, 0, vw_px * 3, vh_px * 3 };
				SDL_UpdateTexture(tex_crt, &r, crtbuf.data(), vw_px * 3 * 4);
			} else {
				SDL_Rect r = { 0, 0, vw_px, vh_px };
				SDL_UpdateTexture(tex, &r, view.data(), vw_px * 4);
			}
			view_dirty = false;
		}
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		if (st.crt) {
			SDL_FRect src = { 0, 0, (float)(vw_px * 3), (float)(vh_px * 3) };
			SDL_RenderTexture(ren, tex_crt, &src, nullptr);
		} else {
			SDL_FRect src = { 0, 0, (float)vw_px, (float)vh_px };
			SDL_RenderTexture(ren, tex, &src, nullptr);
		}
		if (with_menu) draw_menu();
		if (st.show_fps) {
			char t[24]; snprintf(t, sizeof t, "%d FPS", (int)std::lround(fps));
			text((float)vw - text_w(t, 1) - 4, 3, 1, 100, 255, 100, t);
		}
		if (!dev_out.empty() && fps_frames >= 5 && kidx >= keyscript.size() && kwait == 0) {
			SDL_Surface *sf = SDL_RenderReadPixels(ren, nullptr);
			if (sf) {
				SDL_Surface *c = SDL_ConvertSurface(sf, SDL_PIXELFORMAT_ARGB8888);
				if (c) { write_bmp(dev_out.c_str(), (const uint32_t *)c->pixels, c->w, c->h); SDL_DestroySurface(c); }
				SDL_DestroySurface(sf);
			}
			running = false;
		}
		SDL_RenderPresent(ren);
		fps_frames++;
		Uint64 now = SDL_GetTicks();
		if (now - fps_t0 >= 1000) { fps = fps_frames * 1000.0 / (double)(now - fps_t0); fps_frames = 0; fps_t0 = now; }
	};

	if (dev_page >= 0) { menu = true; page = dev_page; sel = dev_page == PG_MAIN ? 0 : 1; }
	int raise_in = 4;      /* raise again a few frames in: some window managers ignore the very first request */
	while (running) {
		if (raise_in > 0 && --raise_in == 0) { SDL_RaiseWindow(win); }
		if (kidx < keyscript.size() || kwait > 0) {
			if (kwait > 0) kwait--;
			else {
				const std::string &t = keyscript[kidx++];
				if (isdigit((unsigned char)t[0])) kwait = atoi(t.c_str());
				else {
					SDL_Event e; SDL_zero(e); e.type = SDL_EVENT_KEY_DOWN; e.key.repeat = false;
					if (t == "pa" || t == "pb" || t == "py" || t == "ps") {      /* controller buttons: A, B, Y, Start */
						e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
						e.gbutton.button = t == "pa" ? SDL_GAMEPAD_BUTTON_SOUTH : t == "pb" ? SDL_GAMEPAD_BUTTON_EAST : t == "py" ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_START;
					} else
					e.key.key = t == "bksp" ? SDLK_BACKSPACE : t == "up" ? SDLK_UP : t == "down" ? SDLK_DOWN : t == "left" ? SDLK_LEFT : t == "right" ? SDLK_RIGHT
					          : t == "enter" ? SDLK_RETURN : t == "esc" ? SDLK_ESCAPE : (SDL_Keycode)t[0];
					SDL_PushEvent(&e);
				}
			}
		}
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) running = false;
			else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) open_pad(ev.gdevice.which);
			else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
				for (size_t i = 0; i < pads.size(); i++)
					if (SDL_GetGamepadID(pads[i]) == ev.gdevice.which) { SDL_CloseGamepad(pads[i]); pads.erase(pads.begin() + i); break; }
			}
			else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !sound_only) {
				const int b = ev.gbutton.button;
				if (menu) {
					if (b == SDL_GAMEPAD_BUTTON_SOUTH) menu_key(SDLK_RETURN, false);
					else if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK) menu_back();
					else if (b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_GUIDE) close_menu();
				} else if (b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_GUIDE) open_menu();
			}
			else if (ev.type == SDL_EVENT_KEY_DOWN && menu) {
				menu_key(ev.key.key, ev.key.repeat);
			}
			else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
				switch (ev.key.key) {
				case SDLK_ESCAPE: if (sound_only) running = false; else open_menu(); break;
				case SDLK_P: paused = !paused; break;
				case SDLK_R: set_rotate(!st.rotate); break;
				case SDLK_F3: m.reset(); break;
				case SDLK_F5: if (!sound_only) m.request_skip(1); break;
				case SDLK_F4: if (!sound_only) m.request_skip(-1); break;
				case SDLK_F11: set_fullscreen(!st.fullscreen); settings_save(st); break;
				case SDLK_F12: {
					std::vector<uint32_t> v; int w, h;
					make_view(m.fb, rotation, v, w, h);
					char n[64]; snprintf(n, sizeof n, "congobongo-%03d.bmp", shot_n++);
					write_bmp(n, v.data(), w, h);
					fprintf(stderr, "saved %s\n", n);
					break; }
				case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: st.vol_master = std::min(200, st.vol_master + 10); apply_audio(); break;
				case SDLK_MINUS: case SDLK_KP_MINUS: st.vol_master = std::max(0, st.vol_master - 10); apply_audio(); break;
				default: break;
				}
				if (sound_only) {
					switch (ev.key.key) {
					case SDLK_RIGHT: test_cmd = (test_cmd + 1) & 0xff; break;
					case SDLK_LEFT:  test_cmd = (test_cmd - 1) & 0xff; break;
					case SDLK_UP:    test_cmd = (test_cmd + 16) & 0xff; break;
					case SDLK_DOWN:  test_cmd = (test_cmd - 16) & 0xff; break;
					case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: m.ppi.latch = (uint8_t)test_cmd; break;
					case SDLK_0: m.ppi.latch = 0x30; break;
					default: break;
					}
					set_title();
				}
			}
		}
		if (!running) break;

		/* keep about two frames of audio queued; that paces the emulation
		 * (without an audio device, fall back to a plain 60 Hz timer) */
		if (as) {
			while (SDL_GetAudioStreamQueued(as) > Q_HIGH && running) SDL_Delay(1);
		} else {
			Uint64 now = SDL_GetTicksNS();
			if (now < next_ns) SDL_DelayNS(next_ns - now);
			next_ns += 16666667;
			if (SDL_GetTicksNS() > next_ns + 100000000ULL) next_ns = SDL_GetTicksNS();
		}

		if (menu) {
			{   /* controller direction with auto-repeat */
				int dx, dy; pad_dir(dx, dy);
				const Uint64 now = SDL_GetTicks();
				if (!dx && !dy) pad_held = 0;
				else if (!pad_held || now >= pad_next_ms) {
					menu_key(dy < 0 ? SDLK_UP : dy > 0 ? SDLK_DOWN : dx < 0 ? SDLK_LEFT : SDLK_RIGHT, pad_held != 0);
					pad_next_ms = now + (pad_held ? 90 : 350); pad_held = 1;
				}
			}
			if (page == PG_AUDIO) {
				/* the sound board keeps running (game frozen) so the TEST SOUND item and level changes are audible */
				if (test_stage > 0 && --test_stage == 0) m.ppi.latch = (uint8_t)TEST_CMDS[test_idx];
				bool so = m.sound_only; m.sound_only = true;
				m.run_frame();
				m.sound_only = so;
				if (as) SDL_PutAudioStreamData(as, m.audio.data(), (int)(m.audio.size() * sizeof(int16_t)));
			}
			draw_frame(true);
			continue;
		}
		if (paused) { SDL_Delay(16); continue; }

		const bool *k = SDL_GetKeyboardState(nullptr);
		uint8_t s0 = 0, s100 = 0;
		if (k[SDL_SCANCODE_RIGHT]) s0 |= 0x01;
		if (k[SDL_SCANCODE_LEFT])  s0 |= 0x02;
		if (k[SDL_SCANCODE_UP])    s0 |= 0x04;
		if (k[SDL_SCANCODE_DOWN])  s0 |= 0x08;
		if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_SPACE]) s0 |= 0x10;
		if (k[SDL_SCANCODE_1]) s100 |= 0x04;
		if (k[SDL_SCANCODE_2]) s100 |= 0x08;
		bool coin = k[SDL_SCANCODE_5];
		{
			int dx, dy; pad_dir(dx, dy);
			if (dx > 0) s0 |= 0x01; if (dx < 0) s0 |= 0x02; if (dy < 0) s0 |= 0x04; if (dy > 0) s0 |= 0x08;
			for (SDL_Gamepad *g : pads) {
				if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST)) s0 |= 0x10;
				if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) s100 |= 0x04;
				if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) s100 |= 0x08;
				if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) coin = true;
			}
		}
		if (coin && !prev_coin && (m.l0 & 1)) m.coin_status[0] = 1;
		prev_coin = coin;
		if (!sound_only) { m.sw00 = s0; m.sw01 = 0; m.sw100 = s100; }

		m.run_frame();
		if (as) SDL_PutAudioStreamData(as, m.audio.data(), (int)(m.audio.size() * sizeof(int16_t)));
		/* if the display (vsync) held us up and the audio queue ran low, emulate extra frames so the sound never starves */
		for (int extra = 0; extra < 3 && as && SDL_GetAudioStreamQueued(as) < Q_LOW; extra++) {
			m.run_frame();
			SDL_PutAudioStreamData(as, m.audio.data(), (int)(m.audio.size() * sizeof(int16_t)));
		}

		refresh_view();
		draw_frame(false);
	}

	if (dev_stats) fprintf(stderr, "emulated %ld frames in %.2f s (%.1f fps)\n", m.frames, SDL_GetTicks() / 1000.0, m.frames * 1000.0 / std::max<Uint64>(1, SDL_GetTicks()));
	settings_save(st);
	for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
	if (as) SDL_DestroyAudioStream(as);
	SDL_DestroyTexture(tex);
	SDL_DestroyTexture(tex_crt);
	if (mfont.atlas) SDL_DestroyTexture(mfont.atlas);
	if (logo_tex) SDL_DestroyTexture(logo_tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
#endif /* !CB_LIBRETRO */
