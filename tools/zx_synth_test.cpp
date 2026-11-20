// developer tool: fire each Zaxxon sound-circuit model in turn and write a WAV + level/frequency summary
#include "../zaxxon_synth.h"
#include <cstdio>
#include <vector>
#include <complex>
static void wav(const char *n, const std::vector<int16_t> &s)
{
	FILE *f = fopen(n, "wb"); uint32_t r = 48000, d = (uint32_t)s.size() * 2, t = 36 + d, br = r * 2; uint16_t one = 1, ch = 1, bits = 16, ba = 2; uint32_t sixteen = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&t, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sixteen, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&ch, 2, 1, f);
	fwrite(&r, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f); fwrite(s.data(), 2, s.size(), f); fclose(f);
}
int main(int argc, char **argv)
{
	const char *outdir = argc > 1 ? argv[1] : nullptr; int idx = 0;
	struct T { const char *name; int port; uint8_t mask; double hold, total; } tests[] = {
		{ "engine start-of-life (PA2+PA3 low)", 0, 0x0c, 2.0, 3.0 }, { "engine flight (PA3 low)", 0, 0x08, 2.0, 3.0 },
		{ "homing missile PA4", 0, 0x10, 0.4, 0.9 }, { "base missile PA5", 0, 0x20, 0.02, 1.0 }, { "laser PA6", 0, 0x40, 0.6, 1.0 },
		{ "battleship PA7", 0, 0x80, 0.4, 0.9 }, { "S-EXP PB4", 1, 0x10, 0.02, 2.0 }, { "M-EXP PB5", 1, 0x20, 0.02, 5.0 },
		{ "cannon PB7", 1, 0x80, 0.02, 2.0 }, { "shot PC0", 2, 0x01, 0.02, 1.0 }, { "alarm2 PC2", 2, 0x04, 0.02, 0.6 }, { "alarm3 PC3", 2, 0x08, 0.02, 0.6 } };
	zxs::Synth s; s.reset(48000); std::vector<int16_t> all;
	for (auto &t : tests) {
		s.reset(48000);
		uint8_t p[3] = { 0xfc, 0xff, 0xff };
		if (t.port == 0 && (t.mask & 0x08)) { p[0] = 0xfc; }
		s.ports(p[0], p[1], p[2]);
		for (int i = 0; i < 4800; i++) s.tick();
		int n = (int)(t.total * 48000), nh = (int)(t.hold * 48000);
		double peak = 0, sq = 0; int zc = 0; double prev = 0; std::vector<double> o;
		for (int i = 0; i < n; i++) {
			if (i == 0) { if (t.mask & 0x08 && t.port == 0) { p[0] = 0xfc & ~t.mask; } else p[t.port] &= ~t.mask; s.ports(p[0], p[1], p[2]); }
			if (i == nh) { p[0] = 0xfc; p[1] = p[2] = 0xff; s.ports(p[0], p[1], p[2]); }
			double y = s.tick(); o.push_back(y); peak = std::max(peak, std::fabs(y)); sq += y * y; if ((y >= 0) != (prev >= 0)) zc++; prev = y;
			all.push_back((int16_t)std::lrint(std::max(-1.0, std::min(1.0, y * 1.4)) * 30000));
		}
		// level per 0.25 s window
		printf("%-28s peak %.3f rms %.3f  zero-cross/s %.0f  rms by 0.25s:", t.name, peak, std::sqrt(sq / n), zc / t.total);
		for (int w = 0; w + 12000 <= n && w < 12000 * 8; w += 12000) { double q = 0; for (int i = 0; i < 12000; i++) q += o[w + i] * o[w + i]; printf(" %.3f", std::sqrt(q / 12000)); }
		printf("\n");
		for (int i = 0; i < 12000; i++) all.push_back(0);
		if (outdir) { std::vector<int16_t> one; for (double y : o) one.push_back((int16_t)std::lrint(std::max(-1.0, std::min(1.0, y * 1.4)) * 30000)); char fn[300]; snprintf(fn, sizeof fn, "%s/%02d.wav", outdir, idx); wav(fn, one); }
		idx++;
	}
	wav("/home/claude/zx_out/synth_all.wav", all);
}
