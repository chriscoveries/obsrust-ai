/* Headless check for libstt_live.dylib: dlopen it, push a WAV's samples
 * through stt_live_push, and count finalized words coming back. Exit 0 only
 * if at least one word arrives — proves the engine works without OBS or a mic.
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*stt_word_cb)(void *ctx, const char *word, double start, double end);
typedef void (*stt_volatile_cb)(void *ctx, const char *text);
typedef int32_t (*stt_start_fn)(double, stt_word_cb, stt_volatile_cb, void *);
typedef void (*stt_push_fn)(const float *, uint32_t);
typedef void (*stt_stop_fn)(void);

static int words, volatiles;
static void on_word(void *ctx, const char *w, double s, double e)
{
	(void)ctx;
	printf("W\t%.3f\t%s\n", s, w);
	fflush(stdout);
	words++;
}
static void on_volatile(void *ctx, const char *t)
{
	(void)ctx;
	(void)t;
	volatiles++;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.wav\n", argv[0]);
		return 2;
	}
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) {
		perror("wav");
		return 2;
	}
	/* walk RIFF chunks: fmt for rate/channels/bits, data for the PCM */
	uint8_t riff[12];
	if (fread(riff, 1, 12, fp) != 12 || memcmp(riff, "RIFF", 4) ||
	    memcmp(riff + 8, "WAVE", 4)) {
		fprintf(stderr, "not a WAV\n");
		return 2;
	}
	uint32_t rate = 0, bytes = 0;
	uint16_t bits = 0, ch = 0;
	uint8_t *pcm = NULL;
	for (;;) {
		uint8_t chdr[8];
		if (fread(chdr, 1, 8, fp) != 8)
			break;
		uint32_t sz = *(uint32_t *)(chdr + 4);
		if (!memcmp(chdr, "fmt ", 4)) {
			uint8_t fmt[16];
			if (sz < 16 || fread(fmt, 1, 16, fp) != 16)
				return 2;
			ch = *(uint16_t *)(fmt + 2);
			rate = *(uint32_t *)(fmt + 4);
			bits = *(uint16_t *)(fmt + 14);
			fseek(fp, sz - 16 + (sz & 1), SEEK_CUR);
		} else if (!memcmp(chdr, "data", 4)) {
			bytes = sz;
			pcm = malloc(bytes);
			if (fread(pcm, 1, bytes, fp) != bytes) {
				fprintf(stderr, "short wav\n");
				return 2;
			}
			break;
		} else {
			fseek(fp, sz + (sz & 1), SEEK_CUR);
		}
	}
	fclose(fp);
	if (!pcm || bits != 16) {
		fprintf(stderr, "expected pcm_s16le, got %u-bit\n", bits);
		return 2;
	}
	uint32_t frames = bytes / 2 / ch;
	float *mono = malloc(frames * sizeof(float));
	const int16_t *s16 = (const int16_t *)pcm;
	for (uint32_t i = 0; i < frames; i++) {
		float acc = 0.f;
		for (uint16_t c = 0; c < ch; c++)
			acc += s16[i * ch + c] / 32768.f;
		mono[i] = acc / ch;
	}

	void *dl = dlopen("libstt_live.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!dl) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}
	stt_start_fn stt_start = (stt_start_fn)dlsym(dl, "stt_live_start");
	stt_push_fn stt_push = (stt_push_fn)dlsym(dl, "stt_live_push");
	stt_stop_fn stt_stop = (stt_stop_fn)dlsym(dl, "stt_live_stop");
	if (!stt_start || !stt_push || !stt_stop) {
		fprintf(stderr, "missing exports\n");
		return 2;
	}

	int32_t rc = stt_start(rate, on_word, on_volatile, NULL);
	if (rc) {
		fprintf(stderr, "stt_live_start: %d\n", rc);
		return 1;
	}
	for (uint32_t off = 0; off < frames; off += 4096) {
		uint32_t n = frames - off > 4096 ? 4096 : frames - off;
		stt_push(mono + off, n);
		usleep(10000); /* keep the queue shallow */
	}
	/* give the analyzer time to finalize the tail */
	for (int i = 0; i < 300 && words < 5; i++)
		usleep(100000);
	stt_stop();
	printf("words=%d volatile_updates=%d\n", words, volatiles);
	return words > 0 ? 0 : 1;
}
