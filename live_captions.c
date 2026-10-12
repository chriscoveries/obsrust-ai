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

/* ---------------- callbacks from libstt_live (worker-side threads) -------- */

static void on_word(void *opaque, const char *word, double start, double end)
{
	(void)start;
	(void)end;
	struct lc_filter *f = opaque;
	pthread_mutex_lock(&f->mu);
	lc_put_word(f, word);
	pthread_mutex_unlock(&f->mu);
	lc_queue_text_update(f);
}

static void on_volatile(void *opaque, const char *text)
{
	struct lc_filter *f = opaque;
	pthread_mutex_lock(&f->mu);
	strncpy(f->volatile_line, text, sizeof(f->volatile_line) - 1);
	f->volatile_line[sizeof(f->volatile_line) - 1] = '\0';
	bool changed = f->show_progress;
	pthread_mutex_unlock(&f->mu);
	if (changed)
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
