import Foundation

/// Where the port keeps the player's files on each platform.
///
/// * iPhone, iPad, Vision Pro: the game folder is copied into the app's
///   Documents (Finder file sharing, the Files app, AirDrop) or sent over
///   the local network; saves and the unpacked Chinese patch live in
///   Application Support, out of the user's way.
/// * Apple TV has no user-visible documents: everything arrives over the
///   network into Caches (which tvOS may purge when the disk runs low; the
///   launcher then asks for the files again).  Saves go to Application
///   Support all the same.
enum GamePaths {
    static let fileManager = FileManager.default

    static var documents: URL {
        fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    static var applicationSupport: URL {
        let url = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    static var caches: URL {
        fileManager.urls(for: .cachesDirectory, in: .userDomainMask)[0]
    }

    /// The folder files sent over the network are written to.
    static var transferTarget: URL {
        #if os(tvOS)
        let url = caches.appendingPathComponent("Game", isDirectory: true)
        #else
        let url = documents.appendingPathComponent("Game", isDirectory: true)
        #endif
        try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// Saves and the global database, one folder per language: the Chinese
    /// patch's saves hold GBK text and cannot be shared with the original's.
    static func saves(for language: GameLanguage) -> URL {
        let url = applicationSupport.appendingPathComponent("Saves/\(language.rawValue)", isDirectory: true)
        try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// The unpacked files of BGI-CHS.exe.
    static var chineseOverlay: URL {
        #if os(tvOS)
        caches.appendingPathComponent("CHS", isDirectory: true)
        #else
        applicationSupport.appendingPathComponent("CHS", isDirectory: true)
        #endif
    }

    /// The game folder: a folder holding sysprg.arc, searched in the places
    /// above and one level below them.
    static func findGameFolder() -> URL? {
        var roots = [transferTarget]
        #if !os(tvOS)
        roots.append(documents)
        #endif
        for root in roots {
            if isGameFolder(root) { return root }
            let children = (try? fileManager.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                                                 options: [.skipsHiddenFiles])) ?? []
            for child in children.sorted(by: { $0.lastPathComponent < $1.lastPathComponent })
            where (try? child.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                if isGameFolder(child) { return child }
            }
        }
        return nil
    }

    static func isGameFolder(_ url: URL) -> Bool {
        fileExists(named: "sysprg.arc", in: url) && fileExists(named: "system.arc", in: url)
    }

    /// The file of that name in the folder, the case of the name aside (the
    /// game was made for a case-insensitive file system).
    static func file(named name: String, in folder: URL) -> URL? {
        let direct = folder.appendingPathComponent(name)
        if fileManager.fileExists(atPath: direct.path) { return direct }
        let entries = (try? fileManager.contentsOfDirectory(atPath: folder.path)) ?? []
        if let match = entries.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) {
            return folder.appendingPathComponent(match)
        }
        return nil
    }

    static func fileExists(named name: String, in folder: URL) -> Bool {
        file(named: name, in: folder) != nil
    }
}

enum GameLanguage: String, CaseIterable, Identifiable {
    case japanese = "ja"
    case chinese = "zh"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .japanese: return "日本語（原版）"
        case .chinese: return "简体中文（汉化补丁）"
        }
    }
}
