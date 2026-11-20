/* Developer harness for the Zaxxon board (no SDL needed):
 *   g++ -O2 -std=gnu++17 -I.. tools/zx_test.cpp -o zx_test
 *   ./zx_test ROMDIR SETNAME [options]     ROMDIR = folder of the extracted ROM files
 * Runs the board headless and writes screenshots, a WAV file and a log of the writes to the sound 8255. */
#include <cstdlib>
#include <filesystem>
#include <string>
#include "z80.c"
#include "zaxxon_hw.h"

struct RomFile { std::string name; std::vector<uint8_t> data; uint32_t crc; };
static uint32_t crc32(const uint8_t *p, size_t n)
{
	static uint32_t t[256]; static bool init = false;
	if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
	uint32_t c = 0xffffffffu; while (n--) c = t[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb"); if (!f) return;
	uint32_t rowb = (w * 3 + 3) & ~3u, sz = 54 + rowb * h;
	uint8_t hd[54] = { 'B', 'M' }; auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hd[o + i] = (uint8_t)(v >> (8 * i)); };
	p32(2, sz); p32(10, 54); p32(14, 40); p32(18, w); p32(22, h); hd[26] = 1; hd[28] = 24; p32(34, rowb * h);
	fwrite(hd, 1, 54, f);
	std::vector<uint8_t> row(rowb);
	for (int y = h - 1; y >= 0; y--) { for (int x = 0; x < w; x++) { uint32_t c = px[y * w + x]; row[x * 3] = c & 255; row[x * 3 + 1] = (c >> 8) & 255; row[x * 3 + 2] = (c >> 16) & 255; } fwrite(row.data(), 1, rowb, f); }
	fclose(f);
}
static void write_wav(const char *path, const std::vector<int16_t> &s)
{
	FILE *f = fopen(path, "wb"); if (!f) return;
	uint32_t n = (uint32_t)(s.size() * 2), rate = zx::RATE, br = rate * 2;
	uint8_t h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a' };
	auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (uint8_t)(v >> (8 * i)); };
	p32(4, 36 + n); p32(24, rate); p32(28, br); p32(40, n);
	fwrite(h, 1, 44, f); fwrite(s.data(), 2, s.size(), f); fclose(f);
}
/* rotate the 256x224 frame 90 degrees (as the arcade monitor is) */
static void rotated(const uint32_t *fb, std::vector<uint32_t> &out, int &w, int &h)
{
	w = zx::SCR_H; h = zx::SCR_W; out.resize((size_t)w * h);
	for (int y = 0; y < zx::SCR_H; y++) for (int x = 0; x < zx::SCR_W; x++) out[(size_t)x * w + (zx::SCR_H - 1 - y)] = fb[y * zx::SCR_W + x];   /* same as make_view() in congobongo.cpp */
}
static FILE *ppi_log = nullptr; static long cur_frame = 0;
/* developer: record which ROM addresses are fetched as opcodes (M1) and which are read as data/operands */
static uint8_t tr_m1[0x6000], tr_rd[0x6000];
static uint8_t trace_m1(void *ud, uint16_t a) { if (a < 0x6000) tr_m1[a] = 1; return zx::Board::bus_rd(ud, a); }
static uint8_t trace_rd(void *ud, uint16_t a) { if (a < 0x6000) tr_rd[a] = 1; return zx::Board::bus_rd(ud, a); }
static void hook(void *, uint64_t cyc, uint8_t a, uint8_t b, uint8_t c) { if (ppi_log) fprintf(ppi_log, "f%ld c%llu  A=%02X B=%02X C=%02X\n", cur_frame, (unsigned long long)cyc, a, b, c); }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: zx_test ROMDIR SET [--frames N] [--shot PREFIX] [--shot-every N] [--wav F] [--ppi-log F] [--coin-at N] [--start-at N] [--inp F0:F1:HEX] [--service F0:F1] [--no-rotate] [--dsw02 HEX] [--dsw03 HEX]\n"); return 1; }
	std::string dir = argv[1], setname = argv[2], shot, wav, plog, m1log;
	int frames = 600, shot_every = 0, coin_at = -1, start_at = -1; bool rot = true;
	struct Inp { int f0, f1; unsigned bits; }; std::vector<Inp> inps; struct Rng { int f0, f1; }; std::vector<Rng> svc;
	int dsw02 = -1, dsw03 = -1;
	for (int i = 3; i < argc; i++) {
		std::string a = argv[i];
		auto nx = [&]() { return argv[++i]; };
		if (a == "--frames") frames = atoi(nx()); else if (a == "--shot") shot = nx(); else if (a == "--shot-every") shot_every = atoi(nx());
		else if (a == "--wav") wav = nx(); else if (a == "--ppi-log") plog = nx(); else if (a == "--coin-at") coin_at = atoi(nx()); else if (a == "--start-at") start_at = atoi(nx());
		else if (a == "--no-rotate") rot = false; else if (a == "--m1-trace") m1log = nx();
		else if (a == "--inp") { Inp e; if (sscanf(nx(), "%d:%d:%x", &e.f0, &e.f1, &e.bits) == 3) inps.push_back(e); }
		else if (a == "--service") { Rng r; if (sscanf(nx(), "%d:%d", &r.f0, &r.f1) == 2) svc.push_back(r); }
		else if (a == "--dsw02") dsw02 = (int)strtol(nx(), nullptr, 16); else if (a == "--dsw03") dsw03 = (int)strtol(nx(), nullptr, 16);
	}
	std::vector<RomFile> files;
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (!e.is_regular_file()) continue;
		FILE *f = fopen(e.path().string().c_str(), "rb"); if (!f) continue;
		RomFile r; r.name = e.path().filename().string(); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		r.data.resize(n); if (fread(r.data.data(), 1, n, f) != (size_t)n) { fclose(f); continue; } fclose(f);
		r.crc = crc32(r.data.data(), r.data.size()); files.push_back(std::move(r));
	}
	int si = zx::detect_set(files);
	if (si < 0) { fprintf(stderr, "no Zaxxon set found in %s\n", dir.c_str()); return 1; }
	fprintf(stderr, "detected: %s (%s)\n", zx::SETS[si].name, zx::SETS[si].title);
	static zx::Board bd; std::string err;
	if (!bd.load(files, si, &err)) { fprintf(stderr, "%s", err.c_str()); return 1; }
	if (dsw02 >= 0) bd.dsw02 = (uint8_t)dsw02; if (dsw03 >= 0) bd.dsw03 = (uint8_t)dsw03;
	if (!plog.empty()) { ppi_log = fopen(plog.c_str(), "w"); bd.ppi_hook = hook; }
	bd.reset();
	if (!m1log.empty()) { bd.cpu.rd = trace_rd; bd.cpu.rdm1 = trace_m1; }
	std::vector<int16_t> all; std::vector<uint32_t> view; int w, h;
	for (int f = 0; f < frames; f++) {
		cur_frame = f;
		bd.sw00 = bd.sw01 = 0; bd.sw100 = 0;
		if (coin_at >= 0 && f >= coin_at && f < coin_at + 3) bd.insert_coin(0);
		if (start_at >= 0 && f >= start_at && f < start_at + 10) bd.sw100 |= 0x04;
		for (auto &e : inps) if (f >= e.f0 && f <= e.f1) { bd.sw00 |= (uint8_t)(e.bits & 0xff); bd.sw100 |= (uint8_t)(e.bits >> 8); }
		bd.service = false; for (auto &r : svc) if (f >= r.f0 && f <= r.f1) bd.service = true;
		bd.run_frame();
		all.insert(all.end(), bd.audio.begin(), bd.audio.end());
		if (shot_every > 0 && !shot.empty() && f % shot_every == 0) {
			char n[512]; snprintf(n, sizeof n, "%s-%04d.bmp", shot.c_str(), f);
			if (rot) { rotated(bd.fb, view, w, h); write_bmp(n, view.data(), w, h); } else write_bmp(n, bd.fb, zx::SCR_W, zx::SCR_H);
		}
	}
	if (!shot.empty() && shot_every == 0) { if (rot) { rotated(bd.fb, view, w, h); write_bmp(shot.c_str(), view.data(), w, h); } else write_bmp(shot.c_str(), bd.fb, zx::SCR_W, zx::SCR_H); }
	if (!wav.empty()) write_wav(wav.c_str(), all);
	if (ppi_log) fclose(ppi_log);
	if (!m1log.empty()) { FILE *t = fopen(m1log.c_str(), "wb"); if (t) { fwrite(tr_m1, 1, sizeof tr_m1, t); fwrite(tr_rd, 1, sizeof tr_rd, t); fclose(t); } }
	fprintf(stderr, "ran %d frames; pc=%04X int_en=%d bg_en=%d\n", frames, bd.cpu.pc.w, bd.int_en, bd.bg_enable);
	return 0;
}
