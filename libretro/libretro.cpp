/*
 * Congo Bongo / Tip Top - libretro core.
 *
 * A thin libretro front end around the emulation in ../congobongo.cpp (Machine: both Z80 boards, video,
 * SN76489A sound and the synthesized drums and gorilla).  That file is compiled here with CB_LIBRETRO
 * defined, which leaves out SDL, the options menu, the CRT filter and main().
 *
 * Content: the MAME "congo" zip (rev C) or "tiptop" zip (tiptop needs congo.zip in the same folder).
 * ROMs are matched by CRC32, so renamed files inside the zip are fine.  No samples are needed.
 */
#define CB_LIBRETRO 1
#include "../congobongo.cpp"

#include "libretro.h"

#include <cstdarg>

using namespace cb;

#ifndef CB_VERSION
#define CB_VERSION "1.0"
#endif

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static const double FPS = MAIN_HZ / FRAME_CYC;          /* 59.9994 Hz */

static bool     game_loaded = false;
static bool     rotate_view = true;                     /* tall picture, like the arcade monitor */
static bool     start_coins = true;                     /* Start also drops a coin (TV remotes have no Select) */
static uint32_t view[SCR_W * SCR_H];
static std::vector<int16_t> stereo;

/* cheats from the options; the ROM patches only fit the congo rev C / tiptop program */
static bool known_rom = false;

/* input state */
static bool prev_coin = false, prev_start = false;
static int  auto_start = 0;                             /* frames left of the automatic "start" press after a coin */

static void logf(enum retro_log_level lvl, const char *fmt, ...)
{
	char buf[512];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	if (log_cb) log_cb(lvl, "[CongoBongo] %s", buf);
	else fprintf(stderr, "[CongoBongo] %s", buf);
}

static void show_msg(const char *text, unsigned frames)
{
	struct retro_message msg = { text, frames };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

/* ------------------------------------------------------------------ */
/*  Core options                                                       */
/* ------------------------------------------------------------------ */
static const struct retro_variable VARS[] = {
	{ "congobongo_rotate",      "Screen orientation; vertical (arcade)|horizontal" },
	{ "congobongo_start_coin",  "Start button inserts a coin; enabled|disabled" },
	{ "congobongo_lives",       "Lives; 3|4|5|free play" },
	{ "congobongo_bonus",       "Bonus life at; 10000|20000|30000|40000" },
	{ "congobongo_difficulty",  "Difficulty; medium|easy|hard|hardest" },
	{ "congobongo_inf_lives",   "Cheat: infinite lives; disabled|enabled" },
	{ "congobongo_inf_credits", "Cheat: infinite credits; disabled|enabled" },
	{ "congobongo_inf_bonus",   "Cheat: bonus timer never runs down; disabled|enabled" },
	{ "congobongo_invincible",  "Cheat: invincible (congo rev C only); disabled|enabled" },
	{ "congobongo_walk_water",  "Cheat: walk on water (congo rev C only); disabled|enabled" },
	{ "congobongo_treble",      "Treble cut (speaker low-pass); 8 kHz|12 kHz|5 kHz|3 kHz|off" },
	{ "congobongo_bass_soft",   "Soften bass drum edge; enabled|disabled" },
	{ "congobongo_vol_music",   "Music and beeps volume; 100%|0%|25%|50%|75%|125%|150%|175%|200%" },
	{ "congobongo_vol_drums",   "Drums volume; 100%|0%|25%|50%|75%|125%|150%|175%|200%" },
	{ "congobongo_vol_gorilla", "Gorilla volume; 100%|0%|25%|50%|75%|125%|150%|175%|200%" },
	{ NULL, NULL },
};

static const char *getvar(const char *key)
{
	struct retro_variable v = { key, NULL };
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) return v.value;
	return NULL;
}

static bool var_on(const char *key, bool def)
{
	const char *v = getvar(key);
	if (!v) return def;
	return strcmp(v, "enabled") == 0;
}

static double var_pct(const char *key)
{
	const char *v = getvar(key);
	return v ? atoi(v) / 100.0 : 1.0;
}

static void set_geometry(bool initial);

static void apply_options(bool initial)
{
	Machine &m = M;

	bool rot = true;
	if (const char *v = getvar("congobongo_rotate")) rot = strcmp(v, "horizontal") != 0;
	if (rot != rotate_view || initial) { rotate_view = rot; if (!initial) set_geometry(false); }

	start_coins = var_on("congobongo_start_coin", true);

	/* DIP switch 02: bonus (bits 0-1), difficulty (2-3), lives (4-5), 0x40 = upright cabinet */
	int b = 0x03, d = 0x04, l = 0x30;
	if (const char *v = getvar("congobongo_bonus")) {
		if (!strcmp(v, "20000")) b = 0x01; else if (!strcmp(v, "30000")) b = 0x02; else if (!strcmp(v, "40000")) b = 0x00;
	}
	if (const char *v = getvar("congobongo_difficulty")) {
		if (!strcmp(v, "easy")) d = 0x0c; else if (!strcmp(v, "hard")) d = 0x08; else if (!strcmp(v, "hardest")) d = 0x00;
	}
	if (const char *v = getvar("congobongo_lives")) {
		if (!strcmp(v, "4")) l = 0x10; else if (!strcmp(v, "5")) l = 0x20; else if (!strcmp(v, "free play")) l = 0x00;
	}
	m.dsw02 = (uint8_t)(b | d | l | 0x40);

	int tr = 2;
	if (const char *v = getvar("congobongo_treble")) {
		if (!strcmp(v, "off")) tr = 0; else if (!strcmp(v, "12 kHz")) tr = 1; else if (!strcmp(v, "5 kHz")) tr = 3; else if (!strcmp(v, "3 kHz")) tr = 4;
	}
	m.set_treble(tr);
	m.bass_soft = var_on("congobongo_bass_soft", true);
	m.vol_psg = var_pct("congobongo_vol_music");
	m.vol_perc = var_pct("congobongo_vol_drums");
	m.vol_gor = var_pct("congobongo_vol_gorilla");

	/* cheats (same addresses and patches as the standalone emulator's built-in cheats) */
	m.pokes.clear();
	if (var_on("congobongo_inf_credits", false)) m.pokes.push_back({ 0x8012, 0x99 });
	if (var_on("congobongo_inf_lives", false))   m.pokes.push_back({ 0x8220, 0x06 });
	if (var_on("congobongo_inf_bonus", false))   m.pokes.push_back({ 0x8304, 0x99 });
	static const uint8_t NOHIT[7]  = { 0xAF, 0x32, 0x0D, 0x84, 0xC3, 0x0E, 0x1E };   /* XOR A; LD (840D),A; JP 1E0E */
	static const uint8_t NOFALL[7] = { 0xAF, 0x32, 0x17, 0x84, 0xC3, 0xCD, 0x1E };   /* XOR A; LD (8417),A; JP 1ECD */
	if (var_on("congobongo_invincible", false)) {
		if (known_rom) for (int i = 0; i < 7; i++) { m.pokes.push_back({ (uint16_t)(0x1DDF + i), NOHIT[i] }); m.pokes.push_back({ (uint16_t)(0x1DF9 + i), NOHIT[i] }); }
		else if (!initial) show_msg("Invincible cheat needs the congo rev C program", 180);
	}
	if (var_on("congobongo_walk_water", false)) {
		if (known_rom) for (int i = 0; i < 7; i++) m.pokes.push_back({ (uint16_t)(0x1E45 + i), NOFALL[i] });
		else if (!initial) show_msg("Walk on water cheat needs the congo rev C program", 180);
	}
	if (game_loaded || initial) m.sync_locks();
}

/* ------------------------------------------------------------------ */
/*  libretro API                                                       */
/* ------------------------------------------------------------------ */
RETRO_API void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;
	environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)VARS);
	bool no_game = false;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
	struct retro_log_callback lc;
	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc)) log_cb = lc.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) { game_loaded = false; }
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof *info);
	info->library_name     = "Congo Bongo";
	info->library_version  = CB_VERSION;
	info->valid_extensions = "zip";
	info->need_fullpath    = true;     /* the core opens the zip itself (and congo.zip beside tiptop.zip) */
	info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof *info);
	info->timing.fps         = FPS;
	info->timing.sample_rate = RATE;
	info->geometry.base_width   = rotate_view ? SCR_H : SCR_W;
	info->geometry.base_height  = rotate_view ? SCR_W : SCR_H;
	info->geometry.max_width    = SCR_W;
	info->geometry.max_height   = SCR_W;
	info->geometry.aspect_ratio = rotate_view ? 3.0f / 4.0f : 4.0f / 3.0f;
}

static void set_geometry(bool)
{
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
}

static void set_input_descriptors()
{
	static const struct retro_input_descriptor desc[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Jump" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Jump" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Jump" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Insert coin" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "1 player start" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "2 player start" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Insert coin" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Move (analog)" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Move (analog)" },
		{ 0, 0, 0, 0, NULL },
	};
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

static bool file_exists(const std::string &p)
{
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	if (!game || !game->path) return false;

	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) { logf(RETRO_LOG_ERROR, "XRGB8888 is not supported\n"); return false; }
	set_input_descriptors();

	std::string path = game->path;
	std::vector<RomFile> files;
	add_path(path, files);
	/* a clone set (tiptop.zip) holds only its own ROMs: the shared ones come from congo.zip in the same folder */
	size_t sl = path.find_last_of("/\\");
	std::string dir = sl == std::string::npos ? std::string() : path.substr(0, sl + 1);
	std::string fname = sl == std::string::npos ? path : path.substr(sl + 1);
	if (!ends_with_nocase(fname, "congo.zip") && file_exists(dir + "congo.zip")) add_path(dir + "congo.zip", files);
	/* also look in RetroArch's system folder, in case the parent set lives there */
	const char *sys = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) && sys) {
		std::string s = std::string(sys) + "/congo.zip";
		if (!ends_with_nocase(fname, "congo.zip") && file_exists(s)) add_path(s, files);
	}

	static RomSet rs;
	if (!load_roms(files, rs, false)) {
		logf(RETRO_LOG_ERROR, "missing ROMs in %s (needs the MAME congo or tiptop set)\n", path.c_str());
		show_msg("Congo Bongo: ROMs missing. Load the MAME congo.zip (tiptop.zip needs congo.zip beside it).", 600);
		return false;
	}

	Machine &m = M;
	m.sound_only = false;
	m.volume = 1.0f;
	memcpy(m.srom, rs.d[R_SND].data(), 0x2000);
	for (int i = 0; i < 4; i++) memcpy(m.mrom + i * 0x2000, rs.d[R_M1 + i].data(), 0x2000);
	m.decode_gfx(rs);
	m.mrom_saved = false;
	m.pokes.clear();
	m.sync_locks();       /* keeps an unpatched copy of the program for the ROM-patch cheats */
	known_rom = m.mrom_orig[0x1DDF] == 0x3E && m.mrom_orig[0x1DE0] == 0x00 && m.mrom_orig[0x1DF9] == 0xAF &&
	            m.mrom_orig[0x1E45] == 0x3E && m.mrom_orig[0x1E46] == 0x01 && m.mrom_orig[0x1DC9] == 0xEF;

	apply_options(true);
	m.reset();
	m.sync_locks();
	prev_coin = prev_start = false; auto_start = 0;
	game_loaded = true;
	logf(RETRO_LOG_INFO, "loaded %s (%s program)\n", path.c_str(), known_rom ? "congo rev C" : "other");
	return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) { return false; }

RETRO_API void retro_unload_game(void) { game_loaded = false; }

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void retro_reset(void)
{
	M.reset();
	M.sync_locks();
	prev_coin = prev_start = false; auto_start = 0;
}

static void read_input()
{
	Machine &m = M;
	input_poll_cb();
	auto btn = [](unsigned id) { return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0; };
	uint8_t s0 = 0, s100 = 0;
	if (btn(RETRO_DEVICE_ID_JOYPAD_RIGHT)) s0 |= 0x01;
	if (btn(RETRO_DEVICE_ID_JOYPAD_LEFT))  s0 |= 0x02;
	if (btn(RETRO_DEVICE_ID_JOYPAD_UP))    s0 |= 0x04;
	if (btn(RETRO_DEVICE_ID_JOYPAD_DOWN))  s0 |= 0x08;
	int ax = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	int ay = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
	const int DZ = 0x4000;
	if (ax > DZ) s0 |= 0x01; if (ax < -DZ) s0 |= 0x02;
	if (ay < -DZ) s0 |= 0x04; if (ay > DZ) s0 |= 0x08;
	if (btn(RETRO_DEVICE_ID_JOYPAD_B) || btn(RETRO_DEVICE_ID_JOYPAD_A) || btn(RETRO_DEVICE_ID_JOYPAD_Y)) s0 |= 0x10;

	bool start = btn(RETRO_DEVICE_ID_JOYPAD_START);
	bool coin = btn(RETRO_DEVICE_ID_JOYPAD_SELECT) || btn(RETRO_DEVICE_ID_JOYPAD_L);
	if (start) s100 |= 0x04;
	if (btn(RETRO_DEVICE_ID_JOYPAD_R)) s100 |= 0x08;

	/* Start on its own: drop a coin, then press start for the game a moment later (when no game is running) */
	bool playing = m.mram[0x004] == 0 && m.mram[0x00A] == 4;   /* same test the scene-skip debug key uses */
	if (start_coins && start && !prev_start && !playing && (m.dsw02 & 0x30)) {
		coin = true;
		auto_start = 30;
	}
	prev_start = start;
	if (auto_start > 0) { if (auto_start <= 15) s100 |= 0x04; auto_start--; }

	if (coin && !prev_coin && (m.l0 & 1)) m.coin_status[0] = 1;
	prev_coin = coin;
	m.sw00 = s0; m.sw01 = 0; m.sw100 = s100;
}

RETRO_API void retro_run(void)
{
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) apply_options(false);

	Machine &m = M;
	read_input();
	m.run_frame();

	unsigned w, h;
	if (rotate_view) {
		/* 90 degrees clockwise, as the monitor sits in the cabinet */
		w = SCR_H; h = SCR_W;
		for (int y = 0; y < SCR_H; y++)
			for (int x = 0; x < SCR_W; x++)
				view[x * SCR_H + (SCR_H - 1 - y)] = m.fb[y * SCR_W + x];
		video_cb(view, w, h, w * sizeof(uint32_t));
	} else {
		w = SCR_W; h = SCR_H;
		video_cb(m.fb, w, h, w * sizeof(uint32_t));
	}

	size_t n = m.audio.size();
	stereo.resize(n * 2);
	for (size_t i = 0; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = m.audio[i];
	size_t done = 0;
	while (done < n) {
		size_t k = audio_batch_cb(stereo.data() + 2 * done, n - done);
		if (k == 0) break;
		done += k;
	}
}

/* ------------------------------------------------------------------ */
/*  Save states                                                        */
/* ------------------------------------------------------------------ */
static const uint32_t STATE_MAGIC = 0x43424e47;   /* "CBNG" */
static const uint32_t STATE_VER = 1;

struct Ser {
	uint8_t *w; const uint8_t *r; size_t pos, cap; bool ok;
	void raw(void *p, size_t n)
	{
		if (w) { if (pos + n <= cap) memcpy(w + pos, p, n); else ok = false; }
		else if (r) { if (pos + n <= cap) memcpy(p, r + pos, n); else ok = false; }
		pos += n;
	}
	template <class T> void v(T &x) { raw(&x, sizeof x); }
};

static void sync_state(Ser &s)
{
	Machine &m = M;
	uint32_t magic = STATE_MAGIC, ver = STATE_VER;
	s.v(magic); s.v(ver);
	if (s.r && (magic != STATE_MAGIC || ver != STATE_VER)) { s.ok = false; return; }
	s.v(m.main); s.v(m.snd);
	s.v(m.mram); s.v(m.vram); s.v(m.cram); s.v(m.spr); s.v(m.sram);
	s.v(m.l0); s.v(m.l1); s.v(m.int_en); s.v(m.irq_line); s.v(m.coin_status);
	s.v(m.bg_enable); s.v(m.flip); s.v(m.bg_color); s.v(m.bg_pos); s.v(m.fg_color); s.v(m.fg_bank); s.v(m.color_bank);
	s.v(m.custom); s.v(m.soundlatch);
	s.v(m.psg1); s.v(m.psg2); s.v(m.ppi); s.v(m.perc); s.v(m.gorilla);
	s.v(m.mute); s.v(m.armed); s.v(m.last_pb); s.v(m.last_pc); s.v(m.next_snd_irq); s.v(m.dc_x); s.v(m.dc_y);
	s.v(m.bs_z1); s.v(m.bs_z2); s.v(m.lp_z1); s.v(m.lp_z2);
	s.v(m.main_target); s.v(m.snd_target); s.v(m.sample_no); s.v(m.frame_cyc); s.v(m.vb_done); s.v(m.frame_done);
	s.v(m.frames); s.v(m.skip_pending);
	s.v(prev_coin); s.v(prev_start); s.v(auto_start);
}

RETRO_API size_t retro_serialize_size(void)
{
	Ser s = { NULL, NULL, 0, 0, true };
	sync_state(s);
	return s.pos;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
	Ser s = { (uint8_t *)data, NULL, 0, size, true };
	sync_state(s);
	return s.ok;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	Machine &m = M;
	if (!data || size < retro_serialize_size()) return false;
	/* the Z80 structs carry callback pointers: keep this run's ones */
	Z80 main0 = m.main, snd0 = m.snd;
	Ser s = { NULL, (const uint8_t *)data, 0, size, true };
	sync_state(s);
	m.main.ud = main0.ud; m.main.rd = main0.rd; m.main.wr = main0.wr; m.main.in = main0.in; m.main.out = main0.out;
	m.snd.ud = snd0.ud; m.snd.rd = snd0.rd; m.snd.wr = snd0.wr; m.snd.in = snd0.in; m.snd.out = snd0.out;
	if (!s.ok) { logf(RETRO_LOG_WARN, "save state rejected\n"); return false; }
	return true;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}

RETRO_API void *retro_get_memory_data(unsigned id)
{
	return id == RETRO_MEMORY_SYSTEM_RAM ? M.mram : NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	return id == RETRO_MEMORY_SYSTEM_RAM ? sizeof M.mram : 0;
}
