/* obs-live-captions: audio filter that transcribes a mic source with
 * SpeechAnalyzer (via libstt_live.dylib) and writes the text into a
 * user-selected text source as a roll-up caption window.
 *
 * The audio thread only downmixes into an SPSC ring buffer; a worker thread
 * feeds the speech engine. UI updates are posted with obs_queue_task.
 */

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/bmem.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define COLS 32
#define MAX_ROWS 3
#define RING_CAP (1 << 20) /* ~21.8 s of mono f32 @48k */

typedef void (*stt_word_cb)(void *ctx, const char *word, double start, double end);
typedef void (*stt_volatile_cb)(void *ctx, const char *text);
typedef int32_t (*stt_start_fn)(double, stt_word_cb, stt_volatile_cb, void *);
typedef void (*stt_push_fn)(const float *, uint32_t);
typedef void (*stt_stop_fn)(void);

struct lc_filter {
	obs_source_t *source;
	char target_name[256];
	obs_weak_source_t *target;
	uint32_t rows;
	bool show_progress;
	bool mono_style;

	uint32_t rate; /* OBS mixer rate, frames/sec */
	uint32_t channels;

	/* SPSC ring: audio thread writes, worker reads */
	float *ring;
	_Atomic size_t ring_r, ring_w;

	pthread_t worker;
	_Atomic bool running;

	/* speech engine */
	void *dl;
	stt_start_fn stt_start;
	stt_push_fn stt_push;
	stt_stop_fn stt_stop;
	bool engine_on;

	/* caption window state, owned by the callbacks/UI posts */
	pthread_mutex_t mu;
	char lines[MAX_ROWS][COLS + 1];
	int nlines;
	size_t col;
	char volatile_line[512];
	int mode; /* 0 = roll-up window, 1 = single word */
	char single_word[128];

	int latency_mode; /* 0 = stable (finals only), 1 = fast (stable
			     prefix of volatile stream), 2 = instant */
	int hold_ms;

	/* hold queue: committed words spaced by hold_ms before display */
	char hold_q[64][COLS + 1];
	int hold_n;
	int64_t shown_ms; /* mono ms when last word was applied, 0 = none */

	/* speculative commits from the volatile stream awaiting final dedup */
	char spec_q[64][COLS + 1];
	int spec_n;
	/* volatile stream tracking: previous update's words + count already
	 * committed from the current utterance */
	char prev_vol[64][COLS + 1];
	int prev_vol_n;
	int vol_committed;
};

static void lc_queue_text_update(struct lc_filter *f);
static void lc_update_(struct lc_filter *f, obs_data_t *settings);

/* ---------------- roll-up window ---------------- */

static void lc_roll(struct lc_filter *f)
{
	memmove(f->lines[0], f->lines[1], sizeof(f->lines) - sizeof(f->lines[0]));
	f->lines[f->nlines - 1][0] = '\0';
	f->col = 0;
}

/* Append one word, wrapping word-first to the next row on overflow. */
static void lc_put_word(struct lc_filter *f, const char *w)
{
	size_t len = strlen(w);
	/* strip anything the caption can't show anyway */
	if (len == 0)
		return;
	while (len > COLS) { /* overlong word: emit full rows of it */
		if (f->col > 0)
			lc_roll(f);
		size_t n = COLS;
		memcpy(f->lines[f->nlines - 1], w, n);
		f->lines[f->nlines - 1][n] = '\0';
		lc_roll(f);
		w += n;
		len -= n;
	}
	size_t need = len + (f->col > 0 ? 1 : 0);
	if (f->col + need > COLS)
		lc_roll(f);
	char *dst = f->lines[f->nlines - 1];
	if (f->col > 0)
		dst[f->col++] = ' ';
	memcpy(dst + f->col, w, len);
	f->col += len;
	dst[f->col] = '\0';
}

/* Trim and take the last <= COLS characters, cut at a word boundary. */
static void lc_tail32(const char *in, char *out)
{
	size_t len = strlen(in);
	while (len && isspace((unsigned char)in[len - 1]))
		len--;
	size_t start = 0;
	while (start < len && isspace((unsigned char)in[start]))
		start++;
	len -= start;
	if (len > COLS) {
		start += len - COLS;
		len = COLS;
		/* don't split a word mid-way: drop the partial leading word */
		while (len > 0 && !isspace((unsigned char)in[start])) {
			start++;
			len--;
		}
		while (len && isspace((unsigned char)in[start])) {
			start++;
			len--;
		}
	}
	memcpy(out, in + start, len);
	out[len] = '\0';
}

static void lc_compose(struct lc_filter *f, char *out, size_t out_sz)
{
	/* Fixed grid: exactly `rows` caption lines plus (when enabled) one
	 * in-progress line, every line padded to COLS with trailing spaces.
	 * Trailing spaces DO count toward ft2 width (text-functionality.c
	 * get_ft2_text_width sums slot->advance.x per glyph incl. spaces), so a
	 * space-padded line keeps the box a constant 32 columns wide. */
	(void)out_sz;
	char *p = out;
	if (f->mode == 1) {
		/* single word, centred in the fixed 32-column box. Instant
		 * latency shows the volatile tail word; Stable/Fast show the
		 * last committed word. */
		const char *w = f->single_word;
		char vtail[COLS + 1];
		if (f->latency_mode == 2) {
			lc_tail32(f->volatile_line, vtail);
			char *sp = strrchr(vtail, ' ');
			const char *last = sp ? sp + 1 : vtail;
			if (last[0])
				w = last;
		}
		size_t n = strlen(w);
		if (n > COLS)
			w += n - COLS, n = COLS;
		size_t lead = (COLS - n) / 2;
		memset(p, ' ', lead);
		memcpy(p + lead, w, n);
		memset(p + lead + n, ' ', COLS - lead - n);
		p += COLS;
		*p = '\0';
		return;
	}
	int first = f->nlines - (int)f->rows;
	for (int i = first; i < f->nlines; i++) {
		size_t n = strlen(f->lines[i]);
		memcpy(p, f->lines[i], n);
		memset(p + n, ' ', COLS - n);
		p += COLS;
		*p++ = '\n';
	}
	if (f->show_progress) {
		char tail[COLS + 1];
		lc_tail32(f->volatile_line, tail);
		size_t n = strlen(tail);
		memcpy(p, tail, n);
		memset(p + n, ' ', COLS - n);
		p += COLS;
	}
	*p = '\0';
}

/* ---------------- commit path: speculation, dedup, hold ---------------- */

static int64_t lc_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Apply a word to the display buffer. Call with f->mu held. */
static void lc_apply_word(struct lc_filter *f, const char *w)
{
	if (f->mode == 1) {
		strncpy(f->single_word, w, sizeof(f->single_word) - 1);
		f->single_word[sizeof(f->single_word) - 1] = '\0';
	} else {
		lc_put_word(f, w);
	}
	f->shown_ms = lc_now_ms();
}

/* Commit a word for display, honouring the hold delay. Call with mu held.
 * Returns true when the display changed (caller should post a UI update). */
static bool lc_commit(struct lc_filter *f, const char *w, bool speculative)
{
	if (!w[0])
		return false;
	if (speculative && f->spec_n < 64)
		strcpy(f->spec_q[f->spec_n++], w);
	/* hold only applies to single-word display (roll-up wants every
	 * word); at most one word waits, and a newer word supersedes it so
	 * fast speech can't build a lagging backlog */
	if (f->mode == 1 && f->hold_ms > 0 && f->shown_ms > 0 &&
	    lc_now_ms() - f->shown_ms < f->hold_ms) {
		strcpy(f->hold_q[0], w);
		f->hold_n = 1;
		return false;
	}
	lc_apply_word(f, w);
	return true;
}

/* Pop due words from the hold queue. Call with mu held. */
static bool lc_hold_tick(struct lc_filter *f)
{
	if (f->hold_n <= 0 || lc_now_ms() - f->shown_ms < f->hold_ms)
		return false;
	char w[COLS + 1];
	strcpy(w, f->hold_q[0]);
	memmove(f->hold_q[0], f->hold_q[1], (size_t)--f->hold_n * sizeof(f->hold_q[0]));
	lc_apply_word(f, w);
	return true;
}

/* Split text into words (bounded). Returns count. */
static int lc_words(const char *text, char out[][COLS + 1], int max)
{
	int n = 0;
	const char *p = text;
	while (*p && n < max) {
		while (isspace((unsigned char)*p))
			p++;
		const char *s = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		size_t len = (size_t)(p - s);
		if (len > COLS)
			len = COLS;
		if (len) {
			memcpy(out[n], s, len);
			out[n][len] = '\0';
			n++;
		}
	}
	return n;
}

/* ---------------- callbacks from libstt_live (worker-side threads) -------- */

static void on_word(void *opaque, const char *word, double start, double end)
{
	(void)start;
	(void)end;
	struct lc_filter *f = opaque;
	pthread_mutex_lock(&f->mu);
	bool dirty;
	if (f->spec_n > 0) {
		/* a speculative commit reached final: confirm and drop it */
		bool match = strcmp(f->spec_q[0], word) == 0;
		memmove(f->spec_q[0], f->spec_q[1],
			(size_t)--f->spec_n * sizeof(f->spec_q[0]));
		/* on mismatch the speculation was wrong: show the final */
		dirty = !match && lc_commit(f, word, false);
	} else {
		dirty = lc_commit(f, word, false);
	}
	/* utterance boundary: the next volatile stream starts fresh */
	f->prev_vol_n = 0;
	f->vol_committed = 0;
	pthread_mutex_unlock(&f->mu);
	if (dirty)
		lc_queue_text_update(f);
}

static void on_volatile(void *opaque, const char *text)
{
	struct lc_filter *f = opaque;
	pthread_mutex_lock(&f->mu);
	strncpy(f->volatile_line, text, sizeof(f->volatile_line) - 1);
	f->volatile_line[sizeof(f->volatile_line) - 1] = '\0';

	char wv[64][COLS + 1];
	int n = lc_words(f->volatile_line, wv, 64);
	int c = 0; /* common word prefix with the previous update */
	while (c < n && c < f->prev_vol_n &&
	       strcmp(wv[c], f->prev_vol[c]) == 0)
		c++;

	bool dirty = f->show_progress;
	if (f->latency_mode == 2 && f->mode != 1) {
		/* instant, roll-up: commit each word as it first appears */
		for (int i = f->vol_committed; i < n; i++)
			dirty |= lc_commit(f, wv[i], true);
		f->vol_committed = n;
	} else if (f->latency_mode == 1) {
		/* fast: commit words once the volatile prefix is stable */
		for (int i = f->vol_committed; i < c; i++)
			dirty |= lc_commit(f, wv[i], true);
		if (c < f->vol_committed)
			f->vol_committed = c;
	}
	if (f->latency_mode == 2 && f->mode == 1)
		dirty = true; /* compose shows the volatile tail */
	if (c < f->vol_committed)
		f->vol_committed = c;
	if (f->latency_mode == 2)
		f->vol_committed = n > f->vol_committed ? n : f->vol_committed;

	memcpy(f->prev_vol, wv, sizeof(wv));
	f->prev_vol_n = n;
	pthread_mutex_unlock(&f->mu);
	if (dirty)
		lc_queue_text_update(f);
}

/* ---------------- UI task ---------------- */

struct text_job {
	obs_weak_source_t *target;
	char *text;
	bool mono_style;
};

static void lc_apply_text(void *param)
{
	struct text_job *job = param;
	obs_source_t *target = obs_weak_source_get_source(job->target);
	if (target) {
		obs_data_t *s = obs_source_get_settings(target);
		obs_data_set_string(s, "text", job->text);
		if (job->mono_style) {
			/* Menlo for a constant-width 32-column grid; keep the
			 * source's own size/style, turn the outline on. */
			obs_data_t *font = obs_data_get_obj(s, "font");
			if (font) {
				obs_data_set_string(font, "face", "Menlo");
				obs_data_set_obj(s, "font", font);
				obs_data_release(font);
			}
			obs_data_set_bool(s, "outline", true);
		}
		obs_source_update(target, s);
		obs_data_release(s);
		obs_source_release(target);
	}
	obs_weak_source_release(job->target);
	bfree(job->text);
	bfree(job);
}

static void lc_queue_text_update(struct lc_filter *f)
{
	pthread_mutex_lock(&f->mu);
	char buf[4 * 1024];
	lc_compose(f, buf, sizeof(buf));
	char *copy = bstrdup(buf);
	obs_weak_source_t *weak = f->target;
	bool mono = f->mono_style;
	if (weak)
		obs_weak_source_addref(weak);
	pthread_mutex_unlock(&f->mu);

	if (weak) {
		struct text_job *job = bmalloc(sizeof(*job));
		job->target = weak;
		job->text = copy;
		job->mono_style = mono;
		obs_queue_task(OBS_TASK_UI, lc_apply_text, job, false);
	} else {
		bfree(copy);
	}
}

/* ---------------- worker: ring -> stt_live_push ---------------- */

static void *lc_worker(void *opaque)
{
	struct lc_filter *f = opaque;
	float buf[4096];
	while (atomic_load(&f->running) ||
	       atomic_load(&f->ring_r) != atomic_load(&f->ring_w)) {
		size_t r = atomic_load_explicit(&f->ring_r, memory_order_acquire);
		size_t w = atomic_load_explicit(&f->ring_w, memory_order_acquire);
		size_t avail = w - r;
		pthread_mutex_lock(&f->mu);
		bool dirty = lc_hold_tick(f);
		pthread_mutex_unlock(&f->mu);
		if (dirty)
			lc_queue_text_update(f);
		if (avail == 0) {
			usleep(5000);
			continue;
		}
		size_t n = avail > sizeof(buf) / sizeof(buf[0])
				   ? sizeof(buf) / sizeof(buf[0])
				   : avail;
		for (size_t i = 0; i < n; i++)
			buf[i] = f->ring[(r + i) & (RING_CAP - 1)];
		atomic_store_explicit(&f->ring_r, r + n, memory_order_release);
		f->stt_push(buf, (uint32_t)n);
	}
	return NULL;
}

/* ---------------- obs filter ---------------- */

static const char *lc_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Live Captions (Apple Speech)";
}

static bool lc_start_engine(struct lc_filter *f)
{
	if (f->engine_on || !f->stt_start)
		return f->engine_on;
	if (f->stt_start((double)f->rate, on_word, on_volatile, f) != 0)
		return false;
	f->engine_on = true;
	return true;
}

static void lc_stop_engine(struct lc_filter *f)
{
	if (f->engine_on && f->stt_stop)
		f->stt_stop();
	f->engine_on = false;
}

static void *lc_create(obs_data_t *settings, obs_source_t *source)
{
	struct lc_filter *f = bzalloc(sizeof(*f));
	f->source = source;
	f->rows = 2;
	f->show_progress = true;
	pthread_mutex_init(&f->mu, NULL);

	struct obs_audio_info oai;
	if (obs_get_audio_info(&oai)) {
		f->rate = oai.samples_per_sec;
		f->channels = get_audio_channels(oai.speakers);
	}
	if (!f->rate)
		f->rate = 48000;
	if (!f->channels)
		f->channels = 2;

	f->ring = bmalloc(RING_CAP * sizeof(float));

	/* resolve libstt_live.dylib inside the .plugin bundle */
	const char *mpath = obs_get_module_data_path(obs_current_module());
	char dlpath[1024];
	if (mpath)
		snprintf(dlpath, sizeof(dlpath), "%s/libstt_live.dylib", mpath);
	f->dl = dlopen(mpath ? dlpath : "libstt_live.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!f->dl)
		f->dl = dlopen("libstt_live.dylib", RTLD_NOW | RTLD_LOCAL);
	if (f->dl) {
		f->stt_start = (stt_start_fn)dlsym(f->dl, "stt_live_start");
		f->stt_push = (stt_push_fn)dlsym(f->dl, "stt_live_push");
		f->stt_stop = (stt_stop_fn)dlsym(f->dl, "stt_live_stop");
	}

	atomic_store(&f->ring_r, 0);
	atomic_store(&f->ring_w, 0);
	f->nlines = MAX_ROWS;
	for (int i = 0; i < MAX_ROWS; i++)
		f->lines[i][0] = '\0';
	f->col = 0;

	atomic_store(&f->running, true);
	if (f->stt_push && lc_start_engine(f)) {
		pthread_create(&f->worker, NULL, lc_worker, f);
	} else {
		atomic_store(&f->running, false);
		blog(LOG_WARNING, "[live-captions] speech engine failed to start "
				 "(dl=%p stt_start=%p)", f->dl, (void *)f->stt_start);
	}

	lc_update_(f, settings);
	return f;
}

static void lc_update_(struct lc_filter *f, obs_data_t *settings)
{
	const char *name = obs_data_get_string(settings, "target_source");
	strncpy(f->target_name, name ? name : "", sizeof(f->target_name) - 1);
	f->target_name[sizeof(f->target_name) - 1] = '\0';

	obs_weak_source_t *new_target = NULL;
	if (f->target_name[0]) {
		obs_source_t *t = obs_get_source_by_name(f->target_name);
		if (t) {
			new_target = obs_source_get_weak_source(t);
			obs_source_release(t);
		}
	}
	pthread_mutex_lock(&f->mu);
	if (f->target)
		obs_weak_source_release(f->target);
	f->target = new_target;
	pthread_mutex_unlock(&f->mu);
	int new_mode = (int)obs_data_get_int(settings, "display_mode");
	if (new_mode != 0 && new_mode != 1)
		new_mode = 0;
	int new_latency = (int)obs_data_get_int(settings, "latency_mode");
	if (new_latency < 0 || new_latency > 2)
		new_latency = 1;
	pthread_mutex_lock(&f->mu);
	if (new_mode != f->mode || new_latency != f->latency_mode) {
		/* reset pending speculation so a mode switch can't replay it */
		f->hold_n = 0;
		f->spec_n = 0;
		f->prev_vol_n = 0;
		f->vol_committed = 0;
	}
	f->mode = new_mode;
	f->latency_mode = new_latency;
	f->hold_ms = (int)obs_data_get_int(settings, "hold_ms");
	pthread_mutex_unlock(&f->mu);
	f->rows = (uint32_t)obs_data_get_int(settings, "rows");
	if (f->rows != 2 && f->rows != 3)
		f->rows = 2;
	f->show_progress = obs_data_get_bool(settings, "show_in_progress");
	f->mono_style = obs_data_get_bool(settings, "caption_style");
}

static void lc_update(void *data, obs_data_t *settings)
{
	lc_update_(data, settings);
}

static void lc_destroy(void *data)
{
	struct lc_filter *f = data;
	atomic_store(&f->running, false);
	if (f->engine_on) {
		pthread_join(f->worker, NULL);
		lc_stop_engine(f);
	}
	if (f->dl)
		dlclose(f->dl);
	if (f->target)
		obs_weak_source_release(f->target);
	pthread_mutex_destroy(&f->mu);
	bfree(f->ring);
	bfree(f);
}

static struct obs_audio_data *lc_filter_audio(void *data,
					      struct obs_audio_data *audio)
{
	struct lc_filter *f = data;
	if (!f->engine_on)
		return audio;

	uint32_t frames = audio->frames;
	size_t r = atomic_load_explicit(&f->ring_r, memory_order_acquire);
	size_t w = atomic_load_explicit(&f->ring_w, memory_order_relaxed);
	if (w - r + frames > RING_CAP)
		return audio; /* drop: speech engine behind by >~21 s */

	uint32_t ch = f->channels ? f->channels : 1;
	for (uint32_t i = 0; i < frames; i++) {
		float mono = 0.f;
		for (uint32_t c = 0; c < ch && c < MAX_AV_PLANES; c++) {
			const float *plane = (const float *)audio->data[c];
			if (plane)
				mono += plane[i];
		}
		f->ring[(w + i) & (RING_CAP - 1)] = mono / (float)ch;
	}
	atomic_store_explicit(&f->ring_w, w + frames, memory_order_release);
	return audio;
}

static bool lc_enum_text_sources(void *opaque, obs_source_t *source)
{
	obs_property_t *p = opaque;
	const char *id = obs_source_get_id(source);
	if (id && strncmp(id, "text", 4) == 0) {
		const char *name = obs_source_get_name(source);
		if (name)
			obs_property_list_add_string(p, name, name);
	}
	return true;
}

static obs_properties_t *lc_properties(void *data)
{
	struct lc_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(
		props, "target_source", "Caption text source",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(lc_enum_text_sources, list);
	if (f && f->target_name[0])
		obs_property_list_add_string(list, f->target_name, f->target_name);

	obs_property_t *mode = obs_properties_add_list(
		props, "display_mode", "Display", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "Roll-up window", 0);
	obs_property_list_add_int(mode, "Single word", 1);
	obs_property_t *lat = obs_properties_add_list(
		props, "latency_mode", "Latency", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(lat, "Stable (finalized words)", 0);
	obs_property_list_add_int(lat, "Fast (stable prefix)", 1);
	obs_property_list_add_int(lat, "Instant (volatile)", 2);
	obs_properties_add_int_slider(props, "hold_ms", "Hold (ms)", 0, 3000,
				    50);
	obs_property_t *rows = obs_properties_add_list(
		props, "rows", "Rows", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rows, "2", 2);
	obs_property_list_add_int(rows, "3", 3);
	obs_properties_add_bool(props, "show_in_progress",
				"Show in-progress words");
	obs_properties_add_bool(props, "caption_style",
				"Caption style (monospace)");
	return props;
}

static void lc_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "rows", 2);
	obs_data_set_default_int(settings, "display_mode", 0);
	obs_data_set_default_int(settings, "latency_mode", 1);
	obs_data_set_default_int(settings, "hold_ms", 250);
	obs_data_set_default_bool(settings, "show_in_progress", true);
	obs_data_set_default_bool(settings, "caption_style", true);
}

struct obs_source_info live_captions_filter = {
	.id = "live_captions",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = lc_name,
	.create = lc_create,
	.destroy = lc_destroy,
	.update = lc_update,
	.filter_audio = lc_filter_audio,
	.get_properties = lc_properties,
	.get_defaults = lc_defaults,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-live-captions", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Live Captions (Apple Speech)";
}

bool obs_module_load(void)
{
	obs_register_source(&live_captions_filter);
	return true;
}
