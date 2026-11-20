// license:BSD-3-Clause
// Zaxxon sound effects, generated: no samples are stored or loaded.
// Zaxxon has no sound CPU.  An 8255 (U23, main PCB 834-0214, manual 420-0724 sheets 11 and 12) switches a set of
// analogue effect circuits directly.  Each output is active low; the game writes FF when idle.
//
// What this is, and what it is not
// --------------------------------
// This is a *behavioural* model, not a transient simulation of the analogue board:
//   * Which 8255 pin starts which effect, and whether the effect is a one-shot (starts on the falling edge and
//     runs to its end) or a loop (runs while the pin is held low), follows the schematic and the way the MAME
//     driver treats the same pins.
//   * Pitch, timbre, length and relative loudness of every effect were set by measuring a set of recordings of
//     the real machine (the sample files that go with MAME's "zaxxon" romset): the pitch of the tones, the sweep
//     paths, the shape of the noise spectra and the decay times.  Only those measured numbers are used here;
//     none of the recordings, or anything derived sample-by-sample from them, is included.
//   * The effect circuits are not simulated, so details that depend on the analogue parts (the exact shape of
//     the noise filters, how effects overlap) are approximations.
//
//   PA0,PA1  altitude, 0..3: the engine sound gets louder as the ship climbs (level 0.5 + 0.157 * v)
//   PA2,PA3  engine: a 2-bit code into a 74LS139.  00 = mid band 450..750 Hz (start of a life), 01 (D low, C high) =
//            low band around 300 Hz (flight), 10 / 11 = silent.  Loops, level set by PA0/PA1.
//   PA4      homing missile, ~1 kHz pulse tone (loop)
//   PA5      base missile, band-limited noise (one-shot, 0.72 s)
//   PA6      force-field laser, falling square sweep (repeats while held)
//   PA7      battleship / robot, 131 Hz square (loop)
//   PB4      small explosion, PB5 large explosion (noise through low-pass; one-shots)
//   PB7      cannon (the player's shot): falling sine sweep (one-shot)
//   PC0      enemy shot, noise burst (one-shot)
//   PC2      alarm 2 (target lock) 1.13 kHz beep, PC3 alarm 3 (low fuel) 2.26 kHz beep (one-shots)
//
// Noise: MM5837 (U2), the MAME 17-bit LFSR (taps 14/17) at 103.2 kHz for a 12 V supply.
#ifndef ZAXXON_SYNTH_H
#define ZAXXON_SYNTH_H
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace zxs {

struct Bq {   // RBJ biquad
	double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	void set(int kind, double fs, double f, double q)   // kind 0 low-pass, 1 band-pass (0 dB peak), 2 high-pass
	{
		f = std::min(std::max(f, 5.0), fs * 0.45);
		const double w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q), a0 = 1 + al;
		if (kind == 0) { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
		else if (kind == 1) { b0 = al / a0; b1 = 0; b2 = -al / a0; }
		else { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; }
		a1 = -2 * c / a0; a2 = (1 - al) / a0;
	}
	double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// up to three filters in a row, with a gain that brings the noise passing through it to a given RMS
struct Chain {
	Bq s[3]; int kind[3] = {}, n = 0; double f[3] = {}, q[3] = {}, gain = 1;
	void clear() { n = 0; }
	void add(int k, double freq, double qq) { kind[n] = k; f[n] = freq; q[n] = qq; n++; }
	void setup(double fs, double scale = 1.0) { for (int i = 0; i < n; i++) s[i].set(kind[i], fs, f[i] * scale, q[i]); }
	double run(double x) { for (int i = 0; i < n; i++) x = s[i].run(x); return x * gain; }
};

class Synth {
public:
	double rate = 48000;

	void reset(double r)
	{
		rate = r; dt = 1.0 / r;
		pa = pb = pc = pa_prev = pb_prev = pc_prev = 0xff; ea = eb = ec = 0;
		ctr = 0; pitch_ctr = 0;

		// noise colours (measured on the recordings); target = RMS of the effect while it is at full level
		mk(fly_lo,   .62, 1, 300, 1.3, 2, 120, .707, 0, 650, .707);   // engine, low band (PA3): noise around 300 Hz
		mk(fly_hi,   .71, 2, 430, .9, 0, 750, .707);             // engine, mid band (PA2): 450..750 Hz
		mk(base,     .62, 1, 300, 1.3, 2, 120, .707, 0, 650, .707);   // base missile: same band as the low engine
		mk(sexp,     .60, 0, 270, .707, 2, 80, .707);            // small explosion: 2-pole low-pass 270 Hz
		mk(mexp,     .83, 0, 190, .707, 2, 45, .707);            // large explosion: 2-pole low-pass 190 Hz
		mk(shot,     .26, 2, 200, .707, 0, 1900, .707);          // enemy shot: 200 Hz .. 1.9 kHz
		mk(jit,      1.0, 0, 150, .707);                         // slow wobble of the homing tone
		fly_lo.setup(r); fly_hi.setup(r); base.setup(r); sexp.setup(r); mexp.setup(r); shot.setup(r); jit.setup(r);
		calibrate();

		g_lo = g_hi = g_hom = g_las = g_bs = 0;
		t_base = t_sexp = t_mexp = t_cann = t_shot = t_al2 = t_al3 = -1;
		ph_hom = ph_las = ph_bs = ph_cann = ph_al2 = ph_al3 = 0; t_las = 0;
		set_pitch(true);
	}

	// Called on every write to the 8255.  Falling edges are latched here, not looked for in tick(): the game drives some
	// pins (the coin blips, for one) low for only a few microseconds, far less than one output sample, so sampling the
	// pin levels once per sample would miss most of those pulses.
	void ports(uint8_t a, uint8_t b, uint8_t c)
	{
		if (ctr >= (unsigned)(rate * 0.1)) { ea |= (uint8_t)(pa_prev & ~a); eb |= (uint8_t)(pb_prev & ~b); ec |= (uint8_t)(pc_prev & ~c); }
		pa_prev = a; pb_prev = b; pc_prev = c;
		pa = a; pb = b; pc = c;
	}

	double tick()
	{
		// Falling edges start one-shots.  The 8255 mode-set at power-on drives every pin low for a few microseconds,
		// which would fire all of them at once; that boot-time blip is ignored for the first 100 ms.
		const uint8_t fa = ea, fb = eb, fc = ec;
		ea = eb = ec = 0;
		if (fa & 0x20) t_base = 0;
		if (fa & 0x40) { t_las = 0; ph_las = 0; }
		if (fb & 0x10) t_sexp = 0;
		if (fb & 0x20) t_mexp = 0;                              // every trigger restarts the envelope (the game re-triggers it each frame while the ship blows up)
		if (fb & 0x80) { t_cann = 0; ph_cann = 0; }
		if (fc & 0x01) t_shot = 0;
		if (fc & 0x04) { t_al2 = 0; ph_al2 = 0; }
		if ((fc & 0x08) && t_al3 < 0) { t_al3 = 0; ph_al3 = 0; }

		const double n = noise(dt);          // -1 .. +1
		double mix = 0;
		const double kg = 1 - std::exp(-dt / 0.003);       // 3 ms: no clicks when a loop starts or stops

		// ---- engine (PA2 = mid band, PA3 = low band; loops).  PA0/PA1 = altitude: louder
		if ((pitch_ctr++ & 63) == 0) set_pitch(false);
		// PA2 (ship C) and PA3 (ship D) are the two address bits of a 74LS139 decoder (U32), not two separate switches:
		// both low selects output 0 (the 500..750 Hz start-of-life noise), D low with C high selects output 1 (the flight
		// noise around 300 Hz); C low with D high selects an output that goes nowhere.
		const bool y0 = (pa & 0x0c) == 0x00, y1 = (pa & 0x0c) == 0x04;
		g_hi += ((y0 ? 1.0 : 0.0) - g_hi) * kg;
		g_lo += ((y1 ? 1.0 : 0.0) - g_lo) * kg;
		{
			const double v = alt_gain;
			const double a = fly_lo.run(n), b = fly_hi.run(n);
			if (g_lo > 1e-4) mix += v * g_lo * a * fly_lo_comp;
			if (g_hi > 1e-4) mix += v * g_hi * b * fly_hi_comp;
		}

		// ---- homing missile (PA4 loop): ~1 kHz pulse tone with a little frequency wobble
		g_hom += (((pa & 0x10) ? 0.0 : 1.0) - g_hom) * kg;
		{
			const double w = jit.run(n);
			ph_hom += dt * 1000.0 * (1.0 + 0.045 * w); ph_hom -= std::floor(ph_hom);
			if (g_hom > 1e-4) mix += g_hom * 0.48 * (ph_hom < 0.4 ? 0.6 : -0.4);
		}

		// ---- base missile (PA5): 0.72 s of band-limited noise, ends with a short fade
		{
			const double x = base.run(n);
			if (t_base >= 0) {
				t_base += dt;
				const double e = std::min(1.0, t_base / 0.005) * std::min(1.0, (0.72 - t_base) / 0.04);
				if (t_base >= 0.72) t_base = -1; else mix += e * x;
			}
		}

		// ---- laser (PA6): a falling square sweep (500 -> 280 Hz, 0.2 s) that repeats while the pin stays low
		g_las += (((pa & 0x40) ? 0.0 : 1.0) - g_las) * kg;
		if (g_las > 1e-4) {
			t_las += dt; if (t_las >= 0.2) t_las -= 0.2;
			const double f = 275.0 + 225.0 * std::exp(-t_las / 0.08);
			ph_las += dt * f; ph_las -= std::floor(ph_las);
			mix += g_las * 0.36 * (ph_las < 0.5 ? 1.0 : -1.0);
		}

		// ---- battleship / robot (PA7 loop): 131 Hz square
		g_bs += (((pa & 0x80) ? 0.0 : 1.0) - g_bs) * kg;
		if (g_bs > 1e-4) {
			ph_bs += dt * 131.0; ph_bs -= std::floor(ph_bs);
			mix += g_bs * 0.32 * (ph_bs < 0.5 ? 1.0 : -1.0);
		}

		// ---- explosions and enemy shot: noise through the fixed filters, held, then decaying
		{
			const double xs = sexp.run(n), xm = mexp.run(n), xh = shot.run(n);
			if (t_sexp >= 0) { t_sexp += dt; mix += env(t_sexp, 0.3, 0.40) * xs; if (t_sexp > 2.0) t_sexp = -1; }
			if (t_mexp >= 0) { t_mexp += dt; mix += mexp_env(t_mexp) * xm; if (t_mexp > 3.9) t_mexp = -1; }
			if (t_shot >= 0) { t_shot += dt; mix += env(t_shot, 0.4, 0.14) * xh; if (t_shot > 1.2) t_shot = -1; }
		}

		// ---- cannon, the player's shot (PB7): sine sweep 4.9 kHz -> 2.1 kHz in about 0.14 s, then a long fade
		if (t_cann >= 0) {
			t_cann += dt;
			double f;
			if (t_cann < 0.08) f = 4900.0;
			else if (t_cann < 0.22) f = 4900.0 - 2800.0 * (t_cann - 0.08) / 0.14;
			else f = 2100.0;
			ph_cann += dt * f; ph_cann -= std::floor(ph_cann);
			const double a = cann_env(t_cann);
			mix += a * 1.414 * (std::sin(2 * M_PI * ph_cann) + 0.12 * std::sin(4 * M_PI * ph_cann));
			if (t_cann > 1.15) t_cann = -1;
		}

		// ---- alarms: PC2 target lock (1.13 kHz, 78 ms), PC3 low fuel (2.26 kHz, 47 ms)
		if (t_al2 >= 0) { t_al2 += dt; ph_al2 += dt * 1127.0; ph_al2 -= std::floor(ph_al2); mix += 0.19 * (ph_al2 < 0.5 ? 1.0 : -1.0); if (t_al2 >= 0.078) t_al2 = -1; }
		if (t_al3 >= 0) { t_al3 += dt; ph_al3 += dt * 2255.0; ph_al3 -= std::floor(ph_al3); mix += 0.19 * (ph_al3 < 0.5 ? 1.0 : -1.0); if (t_al3 >= 0.047) t_al3 = -1; }

		++ctr;
		return mix * 0.30;
	}

private:
	static constexpr double FCLK = 103200.0;
	double dt = 1.0 / 48000;
	uint32_t lfsr = 0x1ffff; double bit_left = 1.0 / FCLK; int gate = 0;
	uint8_t pa = 0xff, pb = 0xff, pc = 0xff, pa_prev = 0xff, pb_prev = 0xff, pc_prev = 0xff, ea = 0, eb = 0, ec = 0;
	Chain fly_lo, fly_hi, base, sexp, mexp, shot, jit;
	double g_lo = 0, g_hi = 0, g_hom = 0, g_las = 0, g_bs = 0;
	double ph_hom = 0, ph_las = 0, ph_bs = 0, ph_cann = 0, ph_al2 = 0, ph_al3 = 0, t_las = 0;
	double t_base = -1, t_sexp = -1, t_mexp = -1, t_cann = -1, t_shot = -1, t_al2 = -1, t_al3 = -1;
	double alt_gain = 0.5;
	unsigned ctr = 0, pitch_ctr = 0;

	void mk(Chain &c, double target, int k1, double f1, double q1, int k2 = -1, double f2 = 0, double q2 = 0, int k3 = -1, double f3 = 0, double q3 = 0)
	{
		c.clear(); c.add(k1, f1, q1); if (k2 >= 0) c.add(k2, f2, q2); if (k3 >= 0) c.add(k3, f3, q3);
		c.gain = target;     // multiplied by the calibration factor below
	}

	// bring the RMS of each noise chain (white noise in) to its target
	void calibrate()
	{
		Chain *all[] = { &fly_lo, &fly_hi, &base, &sexp, &mexp, &shot, &jit };
		const uint32_t sl = lfsr; const double sb = bit_left; const int sg = gate;
		lfsr = 0x1ffff; bit_left = 1.0 / FCLK; gate = 0;
		double acc[7] = {}; const int N = (int)(rate * 4);
		for (int i = 0; i < N; i++) {
			const double x = noise(dt);
			for (int k = 0; k < 7; k++) { double y = x; for (int j = 0; j < all[k]->n; j++) y = all[k]->s[j].run(y); if (i > N / 8) acc[k] += y * y; }
		}
		for (int k = 0; k < 7; k++) {
			const double rms = std::sqrt(acc[k] / (N - N / 8));
			all[k]->gain = rms > 0 ? all[k]->gain / rms : 1.0;
			for (int j = 0; j < all[k]->n; j++) all[k]->s[j].z1 = all[k]->s[j].z2 = 0;
		}
		lfsr = sl; bit_left = sb; gate = sg;
	}

	// engine: altitude 0..3 from PA0/PA1 -> level 0.5 .. 0.97 (the two bits only change the loudness of both engine loops)
	void set_pitch(bool force)
	{
		const int v = pa & 3;
		if (!force && v == alt_v) return;
		alt_v = v;
		alt_gain = 0.5 + 0.157 * v;
		fly_lo.setup(rate); fly_hi.setup(rate);
		fly_lo_comp = fly_hi_comp = 1.0;
	}
	int alt_v = -1; double fly_lo_comp = 1, fly_hi_comp = 1;

	// held at full level for 'hold' seconds, then an exponential decay; short ramp-in
	static double env(double t, double hold, double tau)
	{
		const double a = std::min(1.0, t / 0.003);
		return t < hold ? a : a * std::exp(-(t - hold) / tau);
	}

	// large explosion: full level for about 1.6 s, then a knee and a fall of roughly 25 dB per second (dB points, linear in between)
	static double mexp_env(double t)
	{
		static const double kt[] = { 0, 1.5, 1.625, 1.875, 2.125, 2.375, 2.625, 2.875, 3.125, 3.375, 3.625, 3.9 };
		static const double kd[] = { 0, 0, -0.3, -1.1, -2.5, -6.3, -12, -17.5, -24.4, -32, -38, -60 };
		const int n = (int)(sizeof kt / sizeof kt[0]);
		if (t >= kt[n - 1]) return 0.0;
		int i = 0; while (i + 2 < n && t > kt[i + 1]) ++i;
		const double db = kd[i] + (kd[i + 1] - kd[i]) * (t - kt[i]) / (kt[i + 1] - kt[i]);
		return std::min(1.0, t / 0.003) * std::pow(10.0, db / 20.0);
	}

	// level of the cannon sine: the 50 ms RMS values of the recording, as straight lines in between
	static double cann_env(double t)
	{
		static const double pts[] = { 0.30, 0.28, 0.24, 0.18, 0.15, 0.15, 0.15, 0.12, 0.13, 0.11, 0.11, 0.10, 0.09, 0.08, 0.06, 0.05, 0.04, 0.03, 0.02, 0.01, 0.01, 0.0, 0.0 };
		const int N = (int)(sizeof pts / sizeof pts[0]);
		const double x = std::max(0.0, t / 0.05 - 0.5);
		const int i = (int)x; if (i >= N - 1) return 0.0;
		const double f = x - i;
		return pts[i] * (1 - f) + pts[i + 1] * f;
	}

	// MM5837: average of the +-1 output over h seconds
	double noise(double h)
	{
		double rem = h, acc = 0;
		while (rem > 1e-15) {
			if (bit_left > rem) { acc += (gate ? 1.0 : -1.0) * rem; bit_left -= rem; rem = 0; }
			else {
				acc += (gate ? 1.0 : -1.0) * bit_left; rem -= bit_left; bit_left = 1.0 / FCLK;
				const uint32_t t14 = (lfsr >> 13) & 1, t17 = (lfsr >> 16) & 1;
				lfsr = ((lfsr << 1) | (t14 ^ t17 ^ (lfsr == 0 ? 1 : 0))) & 0x1ffff;
				gate = (lfsr >> 16) & 1;
			}
		}
		return acc / h;
	}
};

}  // namespace zxs

#endif
