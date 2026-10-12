# SpeechAnalyzer latency modes — measured

Date: 2026-10-13. Host: Apple M1 Max, macOS 27, arm64.
Input: `spikes/2026-10-12/speech-rollup/speech-30s.wav` (LibriVox, 30 s, 48 kHz
mono) fed through `measure.swift` at real-time cadence — the same pacing a live
OBS audio filter produces. Wall clock = seconds since feeding began.

## Configurations

| config | module | analyzer options | reporting options |
|---|---|---|---|
| base | SpeechTranscriber | default | volatileResults, fastResults, audioTimeRange |
| opts | SpeechTranscriber | priority=.high, modelRetention=.processLifetime, ignoresResourceLimits | same as base |
| dict | DictationTranscriber | default | volatileResults, frequentFinalization, audioTimeRange |

Raw data: `build/measure.jsonl` (opts+dict), `build/measure-base.jsonl` (base).
Note: in the first combined run `base` was skipped — the en-US asset was only
`supported`, not yet `installed`; after the opts/dict run installed it, the
standalone base run succeeded.

## Results

Latency = wall time of the callback minus the word's audio **end** time.
All three configs finalized the same 42 words; speech onset is ~2.7 s in.

| config | first volatile (wall) | final p50 | final p90 | final max | final min |
|---|---|---|---|---|---|
| opts | 4.02 s | **2.85 s** | **5.69 s** | **7.25 s** | 0.08 s |
| base | 4.34 s | 3.15 s | 6.01 s | 7.57 s | 0.30 s |
| dict | 3.77 s | 6.36 s | 13.29 s | 14.49 s | 2.01 s |

## Findings

- **SpeechTranscriber ≫ DictationTranscriber for live captions.**
  DictationTranscriber's final-word latency is ~2× at the median and ~2× at
  p90 (13.3 s vs 5.7 s) despite `frequentFinalization`. Its volatile text
  arrives no earlier either (first volatile 3.77 s vs 4.02 s).
- **The `opts` analyzer options are nearly free**: ~0.3 s faster at p50 vs
  base (2.85 vs 3.15 s), 7.25 vs 7.57 s worst case. `ignoresResourceLimits`
  does not hurt; keep `.high` priority + `.processLifetime` retention since
  OBS runs long sessions.
- **Final-word latency is still seconds, not sub-second.** Even the best
  config lands p50 ≈ 2.9 s after a word's audio ends. The roll-up window can
  only show finalized words, so on-screen captions trail speech by ~3–7 s.
  The volatile "in-progress" line (or single-word display fed from volatile
  text) is what makes the caption feel live — volatiles appear ~1.3 s after
  speech onset.
- Volatile results from SpeechTranscriber arrive in dense bursts (median
  inter-volatile gap ≈ 0 s — they stream as fast as the analyzer produces
  them), so volatile text is cheap to display at 60 Hz.

## Recommendation for obs-live-captions

Keep `stt_live.swift` on SpeechTranscriber (already is). If not already set,
add `SpeechAnalyzer.Options(priority: .high, modelRetention: .processLifetime,
ignoresResourceLimits: true)` — measured, small but consistent win.

## Reproduce

```sh
xcrun swiftc -parse-as-library -swift-version 6 -O measure.swift -o build/measure
./build/measure /path/to/speech-30s.wav > build/measure.jsonl   # all three
./build/measure --base /path/to/speech-30s.wav                  # one config
```
