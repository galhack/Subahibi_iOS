import AVFoundation
import SwiftUI

@main
struct SubahibiApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var scenePhase

    init() {
        Self.configureAudio()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
        }
        .onChange(of: scenePhase) { _, phase in
            AppleHost_SetActive(phase == .active ? 1 : 0)
            if phase == .active {
                model.refreshIfIdle()
            }
        }
        #if os(visionOS)
        .defaultSize(width: 1280, height: 960)
        #endif
    }

    /// Game sound plays even with the silent switch on, and comes back after a call.
    private static func configureAudio() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [])
        try? session.setActive(true)
        NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil,
                                               queue: .main) { note in
            guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return }
            try? AVAudioSession.sharedInstance().setActive(true)
            AppleHost_AudioResume()
        }
    }
}

struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        switch model.phase {
        case .playing:
            GameView()
                .ignoresSafeArea()
                .persistentSystemOverlays(.hidden)
        case .preparing(let message):
            ProgressScreen(message: message, progress: model.progress)
        case .ended:
            EndedScreen()
        case .launcher:
            LauncherView()
        }
    }
}

struct GameView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> GameViewController { GameViewController() }
    func updateUIViewController(_ controller: GameViewController, context: Context) {}
}

struct ProgressScreen: View {
    let message: String
    let progress: Double

    var body: some View {
        VStack(spacing: 24) {
            Text(message).font(.title2)
            ProgressView(value: progress).frame(maxWidth: 480)
            Text("\(Int(progress * 100))%").monospacedDigit().foregroundStyle(.secondary)
        }
        .padding(48)
    }
}

struct EndedScreen: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(spacing: 24) {
            Text("游戏已结束").font(.largeTitle)
            Text("再次游玩请重新打开本应用。").foregroundStyle(.secondary)
            Button("退出") { model.quitApp() }
                .buttonStyle(.borderedProminent)
        }
        .padding(48)
    }
}
