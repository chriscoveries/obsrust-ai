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
   - **Rows**: 2 or 3 (the roll-up window height).
   - **Show in-progress words**: on = the transcriber's volatile text shows
     as an extra line under the window until it finalizes.
5. Speak into the mic — words roll up the text source as they're finalized.

The first launch downloads the ~600 MB on-device speech model silently if it
isn't installed; there is no permission prompt (audio comes from OBS, not the
mic API, and SpeechTranscriber is exempt from speech authorization).

## Layout

- `stt_live.swift` → `libstt_live.dylib`: `@_cdecl` C ABI
  (`stt_live_start` / `stt_live_push` / `stt_live_stop`) over
  SpeechAnalyzer + SpeechTranscriber. Model installs on demand via
  AssetInventory.
- `live_captions.c`: the filter. `filter_audio` downmixes planar float32 to
  mono into a lock-free SPSC ring; a worker thread feeds `stt_live_push`.
  Finalized words roll up a rows×32 window; updates post to the text source
  via `obs_queue_task(OBS_TASK_UI)` + `obs_source_update`.
- `test_host.c`: `make test-host` dlopens the dylib and pushes a WAV through
  it — headless verification without launching OBS.
