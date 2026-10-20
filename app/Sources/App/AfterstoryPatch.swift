import Foundation
import CryptoKit

/// An optional delta next to the purchased game's archives. Only these four
/// afterstory resources may override the existing Chinese translation.
struct AfterstoryPatch {
    static let directoryName = "AfterstoryCHS"
    static let resourceNames: Set<String> = ["extra", "8_af", "8-1_extra_01-1", "8-1_extra_01-2"]
    static let fontName = "afterstory-font.ttf"
    static let payloadNames = resourceNames.union([fontName])
    static let transferNames = payloadNames.union(["patch.json", "Readme.txt"])

    let directory: URL
    var font: URL { directory.appendingPathComponent(Self.fontName) }

    private struct Manifest: Decodable {
        let format: String
        let version: Int
        let baseFontSHA256: String
        let baseScriptSHA256: String
        let files: [String: String]
    }

    private struct Invalid: LocalizedError {
        let detail: String
        var errorDescription: String? { "后日谈增量补丁无法启用：\(detail)" }
    }

    static func isTransferPath(_ path: String) -> Bool {
        let parts = path.split(separator: "/", omittingEmptySubsequences: false)
        return parts.count == 2 && parts[0] == directoryName && transferNames.contains(String(parts[1]))
    }

    /// Absence leaves the base game alone; an incomplete or incompatible
    /// patch is reported before the engine starts, never partially enabled.
    static func load(gameFolder: URL, baseOverlay: URL) throws -> AfterstoryPatch? {
        let fm = FileManager.default
        let folder = gameFolder.appendingPathComponent(directoryName, isDirectory: true)
        guard fm.fileExists(atPath: folder.path) else { return nil }
        do {
            let data = try Data(contentsOf: folder.appendingPathComponent("patch.json"))
            let manifest = try JSONDecoder().decode(Manifest.self, from: data)
            guard manifest.format == "subahibi-afterstory-chs", manifest.version == 1,
                  Set(manifest.files.keys) == payloadNames else {
                throw Invalid(detail: "补丁格式或资源清单不受支持。")
            }
            func check(_ url: URL, _ expected: String) throws {
                let attrs = try fm.attributesOfItem(atPath: url.path)
                guard attrs[.type] as? FileAttributeType == .typeRegular,
                      let size = attrs[.size] as? NSNumber, size.intValue <= 16 * 1024 * 1024 else {
                    throw Invalid(detail: "\(url.lastPathComponent) 不是有效资源文件。")
                }
                let actual = SHA256.hash(data: try Data(contentsOf: url)).map { String(format: "%02x", $0) }.joined()
                guard actual == expected else {
                    throw Invalid(detail: "\(url.lastPathComponent) 校验不符，请重新导入匹配的完整补丁。")
                }
            }
            func baseFile(_ name: String, in directory: URL) throws -> URL {
                let names = try fm.contentsOfDirectory(atPath: directory.path)
                guard let actual = names.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) else {
                    throw Invalid(detail: "未找到原汉化文件 \(name)。")
                }
                return directory.appendingPathComponent(actual)
            }
            try check(baseFile("font.ttf", in: gameFolder), manifest.baseFontSHA256)
            try check(baseFile("8_af", in: baseOverlay), manifest.baseScriptSHA256)
            for name in payloadNames.sorted() {
                try check(folder.appendingPathComponent(name), manifest.files[name]!)
            }
            return AfterstoryPatch(directory: folder)
        } catch let error as Invalid {
            throw error
        } catch {
            throw Invalid(detail: "文件缺失或无法读取。\(error.localizedDescription)")
        }
    }
}
