import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Unattended runs for testing on the simulators and the devices, driven by
/// the environment of the launch (xcrun simctl launch / devicectl --environment):
///
///   SUBAHIBI_AUTOSTART=ja|zh   skip the launcher and start in that language
///   SUBAHIBI_SCRIPT="..."      input once the engine runs, the words of
///                              tools/apple/hosttest.c: wait S, move X Y,
///                              click X Y, rclick X Y, key VK, wheel D, shot NAME
///
/// "shot NAME" writes the picture to Documents/Debug/NAME.png (Caches on tvOS).
/// Nothing here runs in a normal launch.
enum DebugScript {
    static var autostart: GameLanguage? {
        ProcessInfo.processInfo.environment["SUBAHIBI_AUTOSTART"].flatMap(GameLanguage.init(rawValue:))
    }

    static func runIfRequested() {
        guard let script = ProcessInfo.processInfo.environment["SUBAHIBI_SCRIPT"], !script.isEmpty else { return }
        Thread.detachNewThread {
            run(script)
        }
    }

    private static func run(_ script: String) {
        var words = script.split(separator: " ").map(String.init)[...]
        func int() -> Int32 { Int32(words.popFirst() ?? "0") ?? 0 }
        while let word = words.popFirst() {
            switch word {
            case "wait":
                Thread.sleep(forTimeInterval: Double(int()))
            case "move":
                let x = int(), y = int()
                AppleHost_PointerMove(x, y)
            case "click", "rclick":
                let x = int(), y = int()
                let b: Int32 = word == "rclick" ? 1 : 0
                AppleHost_PointerMove(x, y)
                AppleHost_PointerButton(b, 1, x, y)
                Thread.sleep(forTimeInterval: 0.06)
                AppleHost_PointerButton(b, 0, x, y)
                Thread.sleep(forTimeInterval: 0.1)
            case "key":
                let vk = int()
                AppleHost_Key(vk, 1)
                Thread.sleep(forTimeInterval: 0.06)
                AppleHost_Key(vk, 0)
            case "wheel":
                AppleHost_Wheel(int())
            case "shot":
                shot(name: words.popFirst() ?? "shot")
            default:
                NSLog("DebugScript: unknown word %@", word)
                return
            }
        }
    }

    private static func shot(name: String) {
        var frame = AppleHostFrame_t()
        guard AppleHost_LockFrame(0, &frame) == 1, let pixels = frame.pixels else { return }
        let w = Int(frame.width), h = Int(frame.height)
        var copy = [UInt32](repeating: 0, count: w * h)
        copy.withUnsafeMutableBytes { $0.copyMemory(from: UnsafeRawBufferPointer(start: pixels, count: w * h * 4)) }
        AppleHost_UnlockFrame()
        for i in copy.indices { copy[i] |= 0xff00_0000 }
        #if os(tvOS)
        let dir = GamePaths.caches.appendingPathComponent("Debug", isDirectory: true)
        #else
        let dir = GamePaths.documents.appendingPathComponent("Debug", isDirectory: true)
        #endif
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("\(name).png")
        copy.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                  let image = ctx.makeImage(),
                  let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(dest, image, nil)
            CGImageDestinationFinalize(dest)
        }
        NSLog("DebugScript: wrote %@", url.path)
    }
}
