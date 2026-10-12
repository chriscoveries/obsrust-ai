import AVFoundation
import CoreMedia
import Foundation
import Speech

// Latency measurement: feeds a WAV at real-time cadence through
// SpeechAnalyzer and logs JSONL to stdout:
//   {"ev":"cfg","name":"base"|"opts"|"dict"}
//   {"ev":"v","wall":<s>,"text":"..."}           volatile result
//   {"ev":"w","wall":<s>,"s":..,"e":..,"w":".."} finalized word (audio range)
// wall = seconds since feeding began.

func emit(_ o: [String: Any]) {
    let d = try! JSONSerialization.data(withJSONObject: o, options: [.sortedKeys])
    FileHandle.standardOutput.write(d)
    FileHandle.standardOutput.write(Data([10]))
}

nonisolated(unsafe) var epoch = ProcessInfo.processInfo.systemUptime
func wall() -> Double { ProcessInfo.processInfo.systemUptime - epoch }

func emitRuns(_ text: AttributedString, runs: AttributedString.Runs,
              ev: String = "w") {
    for run in runs {
        let t = String(text[run.range].characters)
        var s = 0.0, d = 0.0
        if let r = run.audioTimeRange, r.start.isValid, r.duration.isValid {
            s = CMTimeGetSeconds(r.start)
            d = CMTimeGetSeconds(r.duration)
        }
        let ws = t.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        for (i, w) in ws.enumerated() where !w.isEmpty {
            let n = Double(ws.count)
            emit(["ev": ev, "wall": wall(), "s": s + d * Double(i) / n,
                  "e": s + d * Double(i + 1) / n, "w": w])
        }
    }
}

func runConfig(name: String, wavPath: String) async throws {
    let locale = await SpeechTranscriber.supportedLocale(
        equivalentTo: Locale(identifier: "en-US"))!

    var modules: [any SpeechModule]
    var transcriber: SpeechTranscriber?
    var dictation: DictationTranscriber?

    if name == "dict" {
        let t = DictationTranscriber(
            locale: locale, contentHints: [], transcriptionOptions: [],
            reportingOptions: [.volatileResults, .frequentFinalization],
            attributeOptions: [.audioTimeRange])
        dictation = t
        modules = [t]
    } else {
        let t = SpeechTranscriber(
            locale: locale, transcriptionOptions: [],
            reportingOptions: [.volatileResults, .fastResults],
            attributeOptions: [.audioTimeRange])
        transcriber = t
        modules = [t]
    }

    var status = await AssetInventory.status(forModules: modules)
    if status != .installed, status != .unsupported,
       let req = try await AssetInventory.assetInstallationRequest(supporting: modules) {
        try await req.downloadAndInstall()
        status = await AssetInventory.status(forModules: modules)
    }
    guard status == .installed else {
        emit(["ev": "E", "name": name, "err": "assets \(status)"]); return
    }

    let file = try AVAudioFile(forReading: URL(fileURLWithPath: wavPath))
    let sourceFormat = AVAudioFormat(standardFormatWithSampleRate:
        file.fileFormat.sampleRate, channels: 1)!
    guard let analyzerFormat = await SpeechAnalyzer.bestAvailableAudioFormat(
        compatibleWith: modules, considering: sourceFormat)
    else { emit(["ev": "E", "name": name, "err": "no format"]); return }
    let converter = AnalyzerInputConverter(analyzerFormat: analyzerFormat)

    let analyzer: SpeechAnalyzer
    if name == "opts" {
        analyzer = SpeechAnalyzer(modules: modules, options:
            SpeechAnalyzer.Options(priority: .high, modelRetention: .processLifetime,
                                   ignoresResourceLimits: true))
    } else {
        analyzer = SpeechAnalyzer(modules: modules)
    }
    try await analyzer.prepareToAnalyze(in: analyzerFormat)
    let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()
    try await analyzer.start(inputSequence: stream)

    emit(["ev": "cfg", "name": name])
    epoch = ProcessInfo.processInfo.systemUptime

    let collector = Task {
        do {
            if let t = transcriber {
                for try await r in t.results {
                    if !r.isFinal {
                        emitRuns(r.text, runs: r.text.runs, ev: "vw")
                    } else {
                        emitRuns(r.text, runs: r.text.runs, ev: "w")
                    }
                }
            } else if let t = dictation {
                for try await r in t.results {
                    if !r.isFinal {
                        emitRuns(r.text, runs: r.text.runs, ev: "vw")
                    } else {
                        emitRuns(r.text, runs: r.text.runs, ev: "w")
                    }
                }
            }
        } catch {}
    }

    let totalFrames = file.length
    let rate = sourceFormat.sampleRate
    var offset: AVAudioFramePosition = 0
    while offset < totalFrames {
        let count = min(AVAudioFramePosition(4096), totalFrames - offset)
        let deadline = Double(offset + count) / rate
        let rem = deadline - wall()
        if rem > 0 { try await Task.sleep(nanoseconds: UInt64(rem * 1e9)) }
        let buf = AVAudioPCMBuffer(pcmFormat: sourceFormat,
                                   frameCapacity: AVAudioFrameCount(count))!
        try file.read(into: buf, frameCount: AVAudioFrameCount(count))
        let at = AVAudioTime(sampleTime: offset, atRate: rate)
        for input in try converter.convert(buf, at: at) { continuation.yield(input) }
        offset += count
    }
    for input in try converter.flush() { continuation.yield(input) }
    continuation.finish()
    try await analyzer.finalizeAndFinishThroughEndOfInput()
    _ = await collector.value
}

@main
struct Measure {
    static func main() async {
        var configs = ["base", "opts", "dict"]
        var wav = ""
        for a in CommandLine.arguments.dropFirst() {
            if a.hasPrefix("--") { configs = [String(a.dropFirst(2))] } else { wav = a }
        }
        for name in configs {
            do { try await runConfig(name: name, wavPath: wav) }
            catch { emit(["ev": "E", "name": name, "err": "\(error)"]) }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
    }
}
