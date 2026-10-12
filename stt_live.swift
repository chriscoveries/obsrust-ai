import AVFoundation
import CoreMedia
import Foundation
import Speech

// Live speech-to-text engine for the OBS "obs-live-captions" plugin.
// C ABI over SpeechAnalyzer/SpeechTranscriber; audio arrives via
// stt_live_push from the plugin's worker thread — no AVAudioEngine, no
// microphone access, no authorization request.
//
//   int32_t stt_live_start(double sample_rate, stt_word_cb, stt_volatile_cb, void* ctx)
//   void    stt_live_push(const float* mono, uint32_t frames)
//   void    stt_live_stop(void)
//
// word_cb(ctx, word, start_sec, end_sec)      — finalized words, audio order
// volatile_cb(ctx, text)                      — current in-progress line

public typealias SttWordCb = @convention(c) (
    UnsafeMutableRawPointer?, UnsafePointer<CChar>?, Double, Double
) -> Void
public typealias SttVolatileCb = @convention(c) (
    UnsafeMutableRawPointer?, UnsafePointer<CChar>?
) -> Void

// @unchecked Sendable: the C callbacks and ctx are plain pointer values the
// caller owns for the session lifetime; mutable fields are only touched on
// the session's own serial queue.
final class Session: @unchecked Sendable {
    let wordCb: SttWordCb
    let volatileCb: SttVolatileCb
    let ctx: UnsafeMutableRawPointer?
    let queue = DispatchQueue(label: "stt_live.feed", qos: .userInitiated)
    var analyzer: SpeechAnalyzer?
    var converter: AnalyzerInputConverter?
    var sourceFormat: AVAudioFormat?
    var continuation: AsyncStream<AnalyzerInput>.Continuation?
    var nextSampleTime: AVAudioFramePosition = 0
    var running = false

    init(w: SttWordCb, v: SttVolatileCb, ctx: UnsafeMutableRawPointer?) {
        wordCb = w
        volatileCb = v
        self.ctx = ctx
    }
    func word(_ w: String, _ s: Double, _ e: Double) {
        w.withCString { cstr in wordCb(ctx, cstr, s, e) }
    }
    func volatileText(_ t: String) {
        t.withCString { cstr in volatileCb(ctx, cstr) }
    }
}

// @unchecked: written under stt_live_start/stop; reads happen on the queue
// the session itself owns. Single-session library.
nonisolated(unsafe) var session: Session?

func startSession(sampleRate: Double, emit: Session) async throws -> Int32 {
    guard SpeechTranscriber.isAvailable else { return -2 }
    guard let locale = await SpeechTranscriber.supportedLocale(
        equivalentTo: Locale(identifier: "en-US"))
    else { return -3 }
    let transcriber = SpeechTranscriber(
        locale: locale, transcriptionOptions: [],
        reportingOptions: [.volatileResults, .fastResults],
        attributeOptions: [.audioTimeRange])
    let modules: [any SpeechModule] = [transcriber]
    var status = await AssetInventory.status(forModules: modules)
    if status != .installed {
        // Silent per-app model install on demand; no prompt, no mic access.
        if let request = try await AssetInventory.assetInstallationRequest(supporting: modules) {
            try await request.downloadAndInstall()
        }
        status = await AssetInventory.status(forModules: modules)
    }
    guard status == .installed else { return -4 }
    guard let sourceFormat = AVAudioFormat(
        commonFormat: .pcmFormatFloat32, sampleRate: sampleRate,
        channels: 1, interleaved: false)
    else { return -5 }
    guard let analyzerFormat = await SpeechAnalyzer.bestAvailableAudioFormat(
        compatibleWith: modules, considering: sourceFormat)
    else { return -5 }
    let converter = AnalyzerInputConverter(analyzerFormat: analyzerFormat)
    let analyzer = SpeechAnalyzer(modules: modules)
    try await analyzer.prepareToAnalyze(in: analyzerFormat)
    let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()
    try await analyzer.start(inputSequence: stream)

    emit.analyzer = analyzer
    emit.converter = converter
    emit.sourceFormat = sourceFormat
    emit.continuation = continuation
    emit.running = true

    // Result consumer: finalized words carry their audio ranges; volatile
    // results carry the current in-progress phrase.
    Task { [emit, transcriber] in
        do {
            for try await result in transcriber.results {
                if !result.isFinal {
                    emit.volatileText(String(result.text.characters))
                    continue
                }
                for run in result.text.runs {
                    let text = String(result.text[run.range].characters)
                    var start = 0.0
                    var duration = 0.0
                    if let r = run.audioTimeRange, r.start.isValid, r.duration.isValid {
                        start = CMTimeGetSeconds(r.start)
                        duration = CMTimeGetSeconds(r.duration)
                    }
                    let words = text.split(whereSeparator: { $0.isWhitespace }).map(String.init)
                    for (i, w) in words.enumerated() where !w.isEmpty {
                        let ws = start + duration * Double(i) / Double(words.count)
                        let we = start + duration * Double(i + 1) / Double(words.count)
                        emit.word(w, ws, we)
                    }
                }
                emit.volatileText("")
            }
        } catch {
            // Stream ends on cancel/finalize; nothing to report through C ABI.
        }
    }
    return 0
}

@_cdecl("stt_live_start")
public func sttLiveStart(
    _ sampleRate: Double,
    _ wordCb: SttWordCb?,
    _ volatileCb: SttVolatileCb?,
    _ ctx: UnsafeMutableRawPointer?
) -> Int32 {
    guard let wordCb, let volatileCb else { return -1 }
    guard session == nil else { return -1 }
    let emit = Session(w: wordCb, v: volatileCb, ctx: ctx)
    session = emit
    let sem = DispatchSemaphore(value: 0)
    final class RC: @unchecked Sendable { var v = Int32(-7) }
    let rc = RC()
    Task {
        do { rc.v = try await startSession(sampleRate: sampleRate, emit: emit) }
        catch { rc.v = -7 }
        sem.signal()
    }
    sem.wait()
    if rc.v != 0 { session = nil }
    return rc.v
}

@_cdecl("stt_live_push")
public func sttLivePush(_ samples: UnsafePointer<Float>?, _ frames: UInt32) {
    guard let session, session.running, let samples, frames > 0 else { return }
    // Copy now: the caller's buffer is transient, and the closure must not
    // capture a non-Sendable pointer.
    let copy = Array(UnsafeBufferPointer(start: samples, count: Int(frames)))
    // Hop onto the session queue: conversion + yield off the caller's thread.
    session.queue.async {
        guard let converter = session.converter,
              let format = session.sourceFormat else { return }
        guard let buf = AVAudioPCMBuffer(pcmFormat: format,
                                       frameCapacity: AVAudioFrameCount(frames))
        else { return }
        buf.frameLength = AVAudioFrameCount(frames)
        memcpy(buf.floatChannelData![0], copy, copy.count * 4)
        let at = AVAudioTime(sampleTime: session.nextSampleTime, atRate: format.sampleRate)
        session.nextSampleTime += AVAudioFramePosition(frames)
        do {
            for input in try converter.convert(buf, at: at) {
                session.continuation?.yield(input)
            }
        } catch {
            // Conversion failure: drop this chunk, keep the session alive.
        }
    }
}

@_cdecl("stt_live_stop")
public func sttLiveStop() {
    guard let s = session else { return }
    s.running = false
    s.queue.sync {
        s.continuation?.finish()
        s.continuation = nil
    }
    Task { [s] in
        if let analyzer = s.analyzer {
            await analyzer.cancelAndFinishNow()
        }
    }
    session = nil
}
