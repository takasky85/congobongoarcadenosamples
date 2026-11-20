// license:BSD-3-Clause
// Zaxxon discrete sound circuits (main PCB 834-0214, schematic sheets 11 and 12 of manual 420-0724).
// No samples.  Zaxxon has no sound CPU: an 8255 (U23) switches a set of analogue effect circuits directly.
//
// What this is, and what it is not
// --------------------------------
// This is a *behavioural* model, not a transient simulation of the whole analogue board.  For every effect
// the signal path was read from the schematic (which 8255 pin starts it, which one-shot / 555 / op-amp
// network shapes it, which filter the noise goes through) and the time constants and corner frequencies were
// computed from the part values on the drawing.  Parts without public documentation (the MB4391 voltage
// controlled attenuators, the CdS photocell PC1) are modelled as ideal linear devices.  Nothing here was
// compared with a recording of a real machine, so levels and timbre are approximations; every figure that
// came from a reading of the schematic is marked with the part names so it can be checked and tuned.
//
// 8255 pins (all sound outputs are active low, the game writes FF when idle):
//   PA0,PA1  U30 7406 -> R10..R16 divider -> U8 -> LED of PC1: three pitch steps of the "flying" whoosh
//   PA2,PA3  U32 74LS139: PA3 low enables the flying sound, PA2 picks one of two band-pass colours
//   PA4 homing missile (U30 -> relaxation oscillator U5/C43 + 555 U6 chopper)
//   PA5 base missile   (U22 74123, 36K/15uF = 154 ms -> C49 envelope -> VCA U14)
//   PA6 "laser"        (U30 -> 555 U7 sweep -> VCO U8/Q3)
//   PA7 battleship / robot (U30 -> two relaxation oscillators U9,U3,U10 on sheet 11)
//   PB4 small explosion (U22 sheet 11: 36K/1uF), PB5 large explosion (U21: 47K/3.3uF), PB7 cannon (U45)
//   PC0 enemy shot (U21), PC2 alarm 2 (U46), PC3 alarm 3 (U46): beeps from the 556 U50 + 74393 U49 divider
//
// Noise: MM5837 (U2), the MAME 17-bit LFSR (taps 14/17) at 103.2 kHz for a 12 V supply, as in the Congo Bongo
// gorilla model.
#ifndef ZAXXON_SYNTH_H
#define ZAXXON_SYNTH_H
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace zxs {

struct Bq {   // RBJ biquad
	double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	void set(int kind, double fs, double f, double q)   // kind 0 low-pass, 1 band-pass (0 dB peak)
	{
		f = std::min(std::max(f, 5.0), fs * 0.45);
		const double w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q), a0 = 1 + al;
		if (kind == 0) { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
		else { b0 = al / a0; b1 = 0; b2 = -al / a0; }
		a1 = -2 * c / a0; a2 = (1 - al) / a0;
	}
	double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// monostable -> RC envelope: while the one-shot is on the capacitor charges with tau_att, afterwards it decays
struct PulseEnv {
	double v = 0, on = 0, ka = 0, kd = 0, ton = 0;
	void setup(double fs, double t_on, double tau_att, double tau_dec) { ton = t_on; ka = 1 - std::exp(-1 / (fs * tau_att)); kd = std::exp(-1 / (fs * tau_dec)); }
	void trig() { on = ton; }
	double run(double dt) { if (on > 0) { on -= dt; v += (1 - v) * ka; } else v *= kd; return v; }
};

class Synth {
public:
	double rate = 48000;

	void reset(double r)
	{
		rate = r; dt = 1.0 / r;
		lfsr = 0x1ffff; bit_left = 1.0 / FCLK; gate = 0;
		pa = pb = pc = pa_prev = pb_prev = pc_prev = 0xff;
		// small explosion (U22/C60 36K,1uF = 10 ms; C61 2.2uF behind 1K+D7 = 2.3 ms; decays through R104||R105 = 235K: 0.52 s)
		e_sexp.setup(r, 0.010, 0.0023, 0.52); lp_sexp.set(0, r, 320, 0.707);        // U3 R116/R113 15K, C70/C71 .033uF
		// large explosion (U21/C62 47K,3.3uF = 43 ms; C63 1uF behind 470 ohm; R109 1M)
		e_mexp.setup(r, 0.043, 0.0005, 1.0);  lp_mexp.set(0, r, 226, 0.707);        // R122/R119 15K, C72/C73 .047uF
		// cannon (U45/C78 47K,1uF = 13 ms; C79 6.8uF behind R126 330 ohm; R127 100K = 0.68 s)
		e_cann.setup(r, 0.013, 0.0022, 0.68); bp_cann.set(1, r, 900, 0.8);
		// base missile (U22 sheet 12: 36K/15uF = 154 ms; C49 15uF behind 470 ohm; recovers through R59+R60 = 440K)
		e_base.setup(r, 0.154, 0.007, 1.7);   bp_base.set(1, r, 480, 1.2);          // R61/R62 15K, C50/C137 .022uF
		bp_hom.set(1, r, 700, 1.0);
		bp_bs.set(0, r, 900, 0.707);
		bp_fly1.set(1, r, 400, 3); bp_fly2.set(1, r, 480, 2.5);
		fly_g = hom_g = bs_g = 0; fly_i = 1.0; fly_n = 0;
		laser_t = -1; shot_t = -1; al2_t = -1; al3_t = -1;
		hom_lfo = hom_chop = 0; bs_lfo = bs_osc = 0; ctr = 0;
		set_fly_led();
	}

	void ports(uint8_t a, uint8_t b, uint8_t c) { pa = a; pb = b; pc = c; }

	double tick()
	{
		// falling edges start the one-shots.  The 8255 mode-set at power-on drives every pin low for a few microseconds, which
		// would fire all of them at once; that boot-time blip is ignored for the first 100 ms.
		const bool settle = ctr < (unsigned)(rate * 0.1);
		const uint8_t fa = settle ? 0 : (pa_prev & ~pa), fb = settle ? 0 : (pb_prev & ~pb), fc = settle ? 0 : (pc_prev & ~pc);
		pa_prev = pa; pb_prev = pb; pc_prev = pc;
		if (fb & 0x10) e_sexp.trig();
		if (fb & 0x20) e_mexp.trig();
		if (fb & 0x80) e_cann.trig();
		if (fa & 0x20) e_base.trig();
		if (fa & 0x40) laser_t = 0;
		if (fc & 0x01) { shot_t = 0; }
		if (fc & 0x04) al2_t = 0;
		if (fc & 0x08) al3_t = 0;

		const double n = noise(dt);          // -1 .. +1
		double mix = 0;

		// ---- flying (PA0..PA3): noise -> LDR tuned stage (U5a, C26/C27) -> colour band-pass -> VCA U14/U15
		const bool fly_on = !(pa & 0x08);
		fly_g += ((fly_on ? 1.0 : 0.0) - fly_g) * (1 - std::exp(-dt / 0.15));       // R29 220K * C34 .68uF
		if ((ctr & 31) == 0) {
			set_fly_led();
			const double f = 227.0 * std::pow(std::max(fly_i, 0.05) / 0.7, 0.53);   // PC1: ~1/(R_LDR + 10K), C26/C27 .01uF
			bp_fly1.set(1, rate, f, 3.0);
			bp_fly2.set(1, rate, (pa & 0x04) ? 723.0 : 482.0, 2.5);                  // C30/C31 2200pF or C39/C40 3300pF with 100K
		}
		if (fly_g > 1e-4) mix += 8.0 * fly_g * bp_fly2.run(bp_fly1.run(n));
		else { bp_fly1.run(0); bp_fly2.run(0); }

		// ---- explosions and cannon: noise through the fixed low-pass / band-pass, VCA driven by the RC envelope
		mix += 7.7 * e_sexp.run(dt) * lp_sexp.run(n);
		mix += 9.0 * e_mexp.run(dt) * lp_mexp.run(n);
		mix += 2.7 * e_cann.run(dt) * bp_cann.run(n);
		mix += 4.0 * e_base.run(dt) * bp_base.run(n);

		// ---- homing missile (PA4): C43 6.8uF/R44 6.8K relaxation oscillator (~8 Hz) chopping noise, 555 U6 at 785 Hz
		const bool hom_on = !(pa & 0x10);
		hom_g += ((hom_on ? 1.0 : 0.0) - hom_g) * (1 - std::exp(-dt / 0.068));
		hom_lfo += dt * 8.0; hom_lfo -= std::floor(hom_lfo);
		hom_chop += dt * 785.0; hom_chop -= std::floor(hom_chop);
		if (hom_g > 1e-4) {
			const double lfo = hom_lfo < 0.5 ? 1.0 : 0.35;
			const double chop = hom_chop < 0.63 ? 1.0 : 0.0;
			mix += 4.5 * hom_g * lfo * (0.6 + 0.4 * chop) * bp_hom.run(n);
		} else bp_hom.run(0);

		// ---- laser (PA6): 555 U7 sweep (R65+R66 = 27K, C53 10uF: ~0.3 s) controlling the relaxation VCO U8
		if (laser_t >= 0) {
			laser_t += dt;
			const double f = 280.0 + 2700.0 * std::exp(-laser_t / 0.11);
			laser_ph += dt * f; laser_ph -= std::floor(laser_ph);
			const double env = laser_t < 0.30 ? 1.0 : std::exp(-(laser_t - 0.30) / 0.03);
			mix += 1.0 * env * (laser_ph < 0.5 ? 1.0 : -1.0);
			if (laser_t > 0.5) laser_t = -1;
		}

		// ---- battleship / robot (PA7): ~28 Hz triangle (U9/U3: R82 30K, C56 3.3uF, hysteresis R81/R80) modulating a VCO (U10, C58 .01uF)
		const bool bs_on = !(pa & 0x80);
		bs_g += ((bs_on ? 1.0 : 0.0) - bs_g) * (1 - std::exp(-dt / 0.1));
		if (bs_g > 1e-4) {
			bs_lfo += dt * 28.0; bs_lfo -= std::floor(bs_lfo);
			const double tri = bs_lfo < 0.5 ? 4 * bs_lfo - 1 : 3 - 4 * bs_lfo;
			bs_osc += dt * (260.0 + 130.0 * tri); bs_osc -= std::floor(bs_osc);
			mix += 1.0 * bs_g * bp_bs.run(bs_osc < 0.5 ? 1.0 : -1.0);
		} else bp_bs.run(0);

		// ---- enemy shot (PC0): falling blip, U21 sheet 11 (R142 18K, C87 2.2uF), R145/C88 .047uF sets the sweep
		if (shot_t >= 0) {
			shot_t += dt;
			const double f = 500.0 + 1700.0 * std::exp(-shot_t / 0.05);
			shot_ph += dt * f; shot_ph -= std::floor(shot_ph);
			mix += 1.1 * std::exp(-shot_t / 0.10) * (shot_ph < 0.5 ? 1.0 : -1.0);
			if (shot_t > 0.5) shot_t = -1;
		}

		// ---- alarms (PC2 target, PC3 fuel): U46 monostables (R167 47K, C96 10uF = 132 ms) gate the 556 U50 (20 kHz)
		//      divided by the 74393 U49; ALARM 2 takes the 2.5 kHz tap, ALARM 3 the 1.25 kHz tap
		if (al2_t >= 0) { al2_t += dt; al2_ph += dt * 2500.0; al2_ph -= std::floor(al2_ph); mix += 0.9 * (al2_ph < 0.5 ? 1.0 : -1.0); if (al2_t > 0.132) al2_t = -1; }
		if (al3_t >= 0) { al3_t += dt; al3_ph += dt * 1250.0; al3_ph -= std::floor(al3_ph); mix += 0.9 * (al3_ph < 0.5 ? 1.0 : -1.0); if (al3_t > 0.132) al3_t = -1; }

		++ctr;
		return mix * 0.16;
	}

private:
	static constexpr double FCLK = 103200.0;
	double dt = 1.0 / 48000;
	uint32_t lfsr = 0x1ffff; double bit_left = 1.0 / FCLK; int gate = 0;
	uint8_t pa = 0xff, pb = 0xff, pc = 0xff, pa_prev = 0xff, pb_prev = 0xff, pc_prev = 0xff;
	PulseEnv e_sexp, e_mexp, e_cann, e_base;
	Bq lp_sexp, lp_mexp, bp_cann, bp_base, bp_hom, bp_bs, bp_fly1, bp_fly2;
	double fly_g = 0, hom_g = 0, bs_g = 0, fly_i = 1.0, fly_n = 0, hom_lfo = 0, hom_chop = 0, bs_lfo = 0, bs_osc = 0;
	double laser_t = -1, laser_ph = 0, shot_t = -1, shot_ph = 0, al2_t = -1, al2_ph = 0, al3_t = -1, al3_ph = 0;
	unsigned ctr = 0;

	// PC1 LED current from PA0/PA1: 7406 outputs pull the R10..R16 network (+12 V) down; U8 buffers the node,
	// R17 390 ohm sets the LED current.  C25 15uF behind ~17K smooths it (about 0.26 s).
	void set_fly_led()
	{
		const double a_low = (pa & 0x01) ? 1 : 0, b_low = (pa & 0x02) ? 1 : 0;
		double vA, vB;
		if (a_low) { vA = 0.2; vB = 0.2; }
		else if (b_low) { vA = 6.1; vB = 0.2; }
		else { vA = 12.0; vB = 12.0; }
		const double N = (12.0 / 82 + vA / 68 + vB / 100) / (1.0 / 82 + 1.0 / 68 + 1.0 / 100 + 1.0 / 36 + 1.0 / 56);
		const double target = std::max(0.0, (N - 1.5) / 0.39);   // mA
		fly_i += (target - fly_i) * (1 - std::exp(-32 * dt / 0.26));
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
