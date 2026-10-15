import Foundation

#if canImport(UIKit)
import UIKit
#endif

/// The Swift side of engine/openbgi/inc/bgi/apple_host.h: starting the
/// engine thread and answering what it asks the application.
enum EngineBridge {
    /// Called on the main thread when the engine has returned (the player
    /// chose to quit, or it failed to start its game).
    static var onEnd: ((Int32) -> Void)?

    /// Shows a message box for the engine and returns the button chosen;
    /// installed by the game screen, called on the main thread.
    static var presentMessageBox: ((String, String, UInt32, @escaping (Int32) -> Void) -> Void)?

    static func start(gameFolder: URL, language: GameLanguage, overlay: URL?) -> Bool {
        AppleHost_SetEndHandler { code in
            DispatchQueue.main.async { EngineBridge.onEnd?(code) }
        }
        AppleHost_SetMessageBoxHandler { text, caption, flags in
            EngineBridge.askOnMainThread(text: text.map { String(cString: $0) } ?? "",
                                         caption: caption.map { String(cString: $0) } ?? "",
                                         flags: flags)
        }
        let saves = GamePaths.saves(for: language)
        let font = language == .chinese ? GamePaths.file(named: "font.ttf", in: gameFolder) : nil
        return gameFolder.path.withCString { game in
            saves.path.withCString { save in
                withOptionalCString(overlay?.path) { over in
                    withOptionalCString(font?.path) { fontPath in
                        "--engine=1.647".withCString { args in
                            var cfg = AppleHostConfig_t(gameDir: game, overlayDir: over, saveDir: save,
                                                        fontFile: fontPath, chinese: language == .chinese ? 1 : 0,
                                                        engineArgs: args)
                            return AppleHost_Start(&cfg) == 1
                        }
                    }
                }
            }
        }
    }

    /// The engine thread waits here while the main thread shows the box.
    private static func askOnMainThread(text: String, caption: String, flags: UInt32) -> Int32 {
        let done = DispatchSemaphore(value: 0)
        var answer: Int32 = -1
        DispatchQueue.main.async {
            guard let present = presentMessageBox else {
                done.signal()
                return
            }
            present(text, caption, flags) { button in
                answer = button
                done.signal()
            }
        }
        done.wait()
        return answer
    }

    private static func withOptionalCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
        guard let s else { return body(nil) }
        return s.withCString { body($0) }
    }
}

/// The MessageBox buttons of the engine (os.h).
enum MessageBoxButtons {
    static let ok: Int32 = 1, cancel: Int32 = 2, retry: Int32 = 4, yes: Int32 = 6, no: Int32 = 7

    /// The buttons of a MessageBox flags value, first the default one's position.
    static func buttons(for flags: UInt32) -> [(title: String, id: Int32)] {
        switch flags & 0xf {
        case 1: return [("确定", ok), ("取消", cancel)]
        case 4: return [("是", yes), ("否", no)]
        case 5: return [("重试", retry), ("取消", cancel)]
        default: return [("确定", ok)]
        }
    }
}
