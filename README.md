# obs-live-captions

macOS (arm64) OBS plugin: an audio filter that transcribes a source's audio
on-device with Apple's SpeechAnalyzer and writes live captions into a text
source as a roll-up window.

## Build & install

```sh
make            # builds libstt_live.dylib + obs-live-captions.plugin in build/
make install    # copies into ~/Library/Application Support/obs-studio/plugins/
```

Builds against the libobs fork build at `~/obsrust/obs-studio`
(`OBS_SRC`/`LIBOBS_FW` are Makefile variables if it lives elsewhere).

## Use in OBS

1. Launch OBS.
2. Add a **Text (FreeType 2)** source to your scene if you don't have one —
   this is where the captions appear. Give it a big font and a name you'll
   recognize.
3. Right-click your mic/audio source → **Filters** → **+** →
   **Live Captions (Apple Speech)**.
4. In the filter properties:
   - **Caption text source**: pick the text source.
   - **Display**: *Roll-up window* (2–3 scrolling lines) or *Single word*
     (the current word, centred in a fixed 32-column box).
   - **Latency**: *Stable* shows only finalized words; *Fast (stable
     prefix)* — the default — shows a word as soon as the transcriber's
     volatile text stops revising it; *Instant* shows volatile text
     immediately (it may be revised).
   - **Hold (ms)**: minimum time each committed word stays before the next
     replaces it (default 250). Applies to **Single word** display only;
     roll-up wants every word. If more than one word arrives while a word is
     being held, only the newest is kept (skip-to-latest), so fast speech
     can't build a lagging backlog.
   - **Rows**: 2 or 3 (the roll-up window height).
   - **Show in-progress words**: on = the transcriber's volatile text shows
     as an extra line under the window until it finalizes.
   - **Caption style (monospace)**: on = force a constant-width (Menlo)
     font so the 32-column grid stays aligned.
5. Speak into the mic — words roll up the text source as they're finalized.

The first launch downloads the ~600 MB on-device speech model silently if it
isn't installed; there is no permission prompt (audio comes from OBS, not the
mic API, and SpeechTranscriber is exempt from speech authorization).

## Latency (measured)

Measured on Apple M1 Max, macOS 27, arm64, feeding a 30 s LibriVox clip
(`speech-30s.wav`, 48 kHz mono) through the same real-time cadence a live OBS
filter produces. Latency = callback wall time minus the word's audio **end**
time. Full method and raw data: `LATENCY-REPORT.md`.

**Per word (n = 42 words) — what a live caption mode experiences:**

| metric | min | p50 | p90 | max |
|---|---|---|---|---|
| volatile-first appearance | -1.80 s* | **0.78 s** | 1.26 s | 1.61 s |
| stable-commit (text stops changing) | -0.64 s* | **0.97 s** | 1.37 s | 1.61 s |
| finalized | 0.76 s | 3.18 s | 6.02 s | 7.58 s |

\* volatile can appear before the word's (revised) final audio-end estimate.

**By configuration (finalized, utterance-batched):**

| config | first volatile | final p50 | final p90 | final max |
|---|---|---|---|---|
| SpeechTranscriber + opts | 4.02 s | 2.85 s | 5.69 s | 7.25 s |
| SpeechTranscriber (base) | 4.34 s | 3.15 s | 6.01 s | 7.57 s |
| DictationTranscriber | 3.77 s | 6.36 s | 13.29 s | 14.49 s |

Takeaways:

- Per word, live latency is **~0.8–1.3 s after each word's audio end** for
  both volatile-first and stable-commit. The volatile "in-progress" line (or
  Single word fed from volatile text) is what makes captions feel live.
- Finalized (batch) latency is still seconds — a whole utterance finalizes
  ~1.3 s after its last word ends, so Stable mode trails speech by ~3–7 s.
- SpeechTranscriber beats DictationTranscriber ~2× at p50/p90; the
  high-priority analyzer options are nearly free (~0.3 s at p50) and are the
  shipped default.

## Roadmap: CEA-608 captions in the stream

Next step (fork-only, nothing upstream): feed this plugin's append-only
committed-word stream (Fast mode, no corrections — 608 can't retract) through
`obs-captions`' roll-up emitter into `obs_output_caption()` as CEA-608
`cc_data` on the active streaming/recording output, so captions are embedded
in the stream for players and recorders — not just drawn on a text source.
Proven headlessly by recording a local file and decoding the captions back
with ffmpeg/ccextractor to compare against the on-screen text.

## Layout

- `stt_live.swift` → `libstt_live.dylib`: `@_cdecl` C ABI
  (`stt_live_start` / `stt_live_push` / `stt_live_stop`) over
  SpeechAnalyzer + SpeechTranscriber. Model installs on demand via
  AssetInventory.
- `live_captions.c`: the filter. `filter_audio` downmixes planar float32 to
  mono into a lock-free SPSC ring; a worker thread feeds `stt_live_push`.
  Finalized words roll up a rows×32 window; updates post to the text source
  via `obs_queue_task(OBS_TASK_UI)` + `obs_source_update`. User-facing
  strings go through `obs_module_text()` and live in
  `data/locale/en-US.ini`.
- `data/locale/en-US.ini`: default (en-US) locale; `make` copies it into the
  bundle's `Contents/Resources/locale/` where libobs loads it.
- `test_host.c`: `make test-host` dlopens the dylib and pushes a WAV through
  it — headless verification without launching OBS.
