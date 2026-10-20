import Foundation
import CryptoKit

@main
enum AfterstoryPatchTest {
    static func main() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? fm.removeItem(at: root) }
        let game = root.appendingPathComponent("Game")
        let overlay = root.appendingPathComponent("CHS")
        try fm.createDirectory(at: game, withIntermediateDirectories: true)
        try fm.createDirectory(at: overlay, withIntermediateDirectories: true)
        let font = game.appendingPathComponent("font.ttf")
        let script = overlay.appendingPathComponent("8_af")
        try Data("old font".utf8).write(to: font)
        try Data("old script".utf8).write(to: script)
        let absent = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay)
        precondition(absent == nil)
        let folder = game.appendingPathComponent(AfterstoryPatch.directoryName)
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        var hashes: [String: String] = [:]
        for name in AfterstoryPatch.payloadNames {
            let data = Data(name.utf8)
            try data.write(to: folder.appendingPathComponent(name))
            hashes[name] = digest(data)
        }
        var manifest: [String: Any] = ["format": "subahibi-afterstory-chs", "version": 1,
            "baseFontSHA256": digest(try Data(contentsOf: font)),
            "baseScriptSHA256": digest(try Data(contentsOf: script)), "files": hashes]
        func writeManifest() throws {
            try JSONSerialization.data(withJSONObject: manifest).write(to: folder.appendingPathComponent("patch.json"))
        }
        func rejects(_ message: String) {
            do {
                _ = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay)
                fatalError("accepted \(message)")
            } catch {}
        }
        try writeManifest()
        let patch = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay)
        precondition(patch?.directory.path == folder.path)
        let bundled = root.appendingPathComponent("BundledAfterstory")
        try fm.moveItem(at: folder, to: bundled)
        let fallback = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay, bundledDirectory: bundled)
        precondition(fallback?.directory.path == bundled.path)
        try fm.copyItem(at: bundled, to: folder)
        let external = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay, bundledDirectory: bundled)
        precondition(external?.directory.path == folder.path)
        try Data("corrupt".utf8).write(to: folder.appendingPathComponent("extra"))
        rejects("corrupt scene")
        try Data("extra".utf8).write(to: folder.appendingPathComponent("extra"))
        try Data("wrong base".utf8).write(to: font)
        rejects("wrong base font")
        try Data("old font".utf8).write(to: font)
        hashes["Main"] = digest(Data())
        manifest["files"] = hashes
        try writeManifest()
        rejects("main-story override")
        do {
            _ = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay, bundledDirectory: bundled)
            fatalError("a corrupt external patch silently fell back to the bundle")
        } catch {}
        let disabled = try AfterstoryPatch.load(gameFolder: game, baseOverlay: overlay,
                                               bundledDirectory: bundled, enabled: false)
        precondition(disabled == nil)
        precondition(AfterstoryPatch.isTransferPath("AfterstoryCHS/extra"))
        precondition(!AfterstoryPatch.isTransferPath("AfterstoryCHS/../font.ttf"))
        precondition(!AfterstoryPatch.isTransferPath("AfterstoryCHS/Main"))
        precondition(!AfterstoryPatch.isTransferPath("AfterstoryCHS\\extra"))
        print("afterstory manifest: passed")
    }

    static func digest(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}
