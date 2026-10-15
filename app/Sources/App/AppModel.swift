import Foundation
import SwiftUI

/// The launcher's state: where the game is, which languages it can run in,
/// the unpacking of the Chinese patch and the running engine.
@MainActor
final class AppModel: ObservableObject {
    enum Phase: Equatable {
        case launcher
        case preparing(String)
        case playing
        case ended(Int32)
    }

    @Published var phase: Phase = .launcher
    @Published var gameFolder: URL?
    @Published var hasChinesePatch = false
    @Published var overlayReady = false
    @Published var progress: Double = 0
    @Published var errorMessage: String?
    @AppStorage("language") var languageRaw: String = GameLanguage.chinese.rawValue

    let transfer = TransferServer()

    var language: GameLanguage {
        get { GameLanguage(rawValue: languageRaw) ?? .japanese }
        set { languageRaw = newValue.rawValue }
    }

    init() {
        transfer.onFileReceived = { [weak self] in
            Task { @MainActor in self?.refresh() }
        }
        refresh()
        DebugScript.redirectLogIfRequested()
        if let lang = DebugScript.autostart {
            language = lang
            DispatchQueue.main.async { self.start() }
        }
    }

    /// Coming back to the front: files may have been copied in meanwhile.
    func refreshIfIdle() {
        if phase == .launcher { refresh() }
    }

    /// Look for the game files and the patch again (after a copy or a transfer).
    func refresh() {
        gameFolder = GamePaths.findGameFolder()
        if let folder = gameFolder {
            hasChinesePatch = GamePaths.fileExists(named: "BGI-CHS.exe", in: folder) || overlayIsCurrent(for: folder)
            overlayReady = overlayIsCurrent(for: folder)
        } else {
            hasChinesePatch = false
            overlayReady = false
        }
        if !hasChinesePatch && language == .chinese && gameFolder != nil {
            language = .japanese
        }
    }

    /// The unpacked patch matches the BGI-CHS.exe of this folder (size and date recorded at unpacking).
    private func overlayIsCurrent(for folder: URL) -> Bool {
        let marker = GamePaths.chineseOverlay.appendingPathComponent(".source")
        guard let recorded = try? String(contentsOf: marker, encoding: .utf8) else { return false }
        guard let exe = GamePaths.file(named: "BGI-CHS.exe", in: folder) else {
            return true // the patch was unpacked earlier and the executable removed since: keep using it
        }
        return recorded == Self.signature(of: exe)
    }

    private static func signature(of url: URL) -> String {
        let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
        let date = (attrs?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        return "\(size)-\(Int(date))"
    }

    func start() {
        guard let folder = gameFolder else { return }
        errorMessage = nil
        if language == .chinese && !overlayReady {
            unpackChinesePatch(folder: folder)
            return
        }
        launch(folder: folder)
    }

    private func launch(folder: URL) {
        let overlay = language == .chinese ? GamePaths.chineseOverlay : nil
        EngineBridge.onEnd = { [weak self] code in
            self?.phase = .ended(code)
        }
        transfer.stop()
        if EngineBridge.start(gameFolder: folder, language: language, overlay: overlay) {
            phase = .playing
            DebugScript.runIfRequested()
        } else {
            errorMessage = "游戏引擎无法启动：请确认游戏文件完整。"
            phase = .launcher
        }
    }

    /// Unpack BGI-CHS.exe's virtual file system (the translated scripts and pictures).
    private func unpackChinesePatch(folder: URL) {
        guard let exe = GamePaths.file(named: "BGI-CHS.exe", in: folder) else {
            errorMessage = "没有找到 BGI-CHS.exe，无法使用汉化。"
            return
        }
        phase = .preparing("正在解开汉化补丁…")
        progress = 0
        let out = GamePaths.chineseOverlay
        let signature = Self.signature(of: exe)
        Task.detached(priority: .userInitiated) {
            try? FileManager.default.removeItem(at: out)
            var err = [CChar](repeating: 0, count: 256)
            let box = await ProgressBox(model: self)
            let count = withExtendedLifetime(box) {
                Evb_Extract(exe.path, out.path, { user, done, total in
                    let box = Unmanaged<ProgressBox>.fromOpaque(user!).takeUnretainedValue()
                    let fraction = total > 0 ? Double(done) / Double(total) : 0
                    box.report(fraction)
                    return 1
                }, Unmanaged.passUnretained(box).toOpaque(), &err, err.count)
            }
            let message = String(cString: err)
            await MainActor.run {
                if count > 0 {
                    try? signature.write(to: out.appendingPathComponent(".source"), atomically: true, encoding: .utf8)
                    self.overlayReady = true
                    self.launch(folder: folder)
                } else {
                    self.errorMessage = "汉化补丁解包失败：\(message)"
                    self.phase = .launcher
                }
            }
        }
    }

    /// Back to the launcher after the engine ended: the engine cannot run twice
    /// in one process, so the app quits when the player asks to play again.
    func quitApp() {
        exit(0)
    }
}

/// Carries the unpacker's progress from its C callback to the model.
private final class ProgressBox: @unchecked Sendable {
    private weak var model: AppModel?

    @MainActor init(model: AppModel) {
        self.model = model
    }

    func report(_ fraction: Double) {
        Task { @MainActor in self.model?.progress = fraction }
    }
}
