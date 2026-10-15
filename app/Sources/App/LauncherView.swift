import SwiftUI

/// The first screen: whether the game files are there, the language, and how
/// to bring the files onto this device.
struct LauncherView: View {
    @EnvironmentObject private var model: AppModel
    @ObservedObject private var transfer: TransferServerObserver = .shared

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                header
                statusSection
                if model.gameFolder != nil {
                    languageSection
                    startButton
                }
                importSection
                if let error = model.errorMessage {
                    Text(error).foregroundStyle(.red)
                }
                footer
            }
            .padding(.horizontal, 48)
            .padding(.vertical, 40)
            .frame(maxWidth: 900, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .onAppear {
            TransferServerObserver.shared.bind(model.transfer)
            model.refresh()
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("美好的每一天 ～不连续存在～").font(.largeTitle.bold())
            Text("素晴らしき日々 ～不連続存在～ · Apple 平台移植").foregroundStyle(.secondary)
        }
    }

    private var statusSection: some View {
        Card {
            VStack(alignment: .leading, spacing: 8) {
                if let folder = model.gameFolder {
                    Label("已找到游戏文件", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Text(folder.path).font(.footnote).foregroundStyle(.secondary).lineLimit(2)
                    if model.hasChinesePatch {
                        Label(model.overlayReady ? "汉化补丁已就绪" : "检测到汉化补丁（首次启动时自动解包）",
                              systemImage: "character.book.closed")
                    }
                } else {
                    Label("尚未找到游戏文件", systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    Text("请把你自己购买的游戏目录（含 sysprg.arc、data*.arc 等文件）放到本应用中。")
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            Text("游戏数据")
        }
    }

    private var languageSection: some View {
        Picker("语言", selection: Binding(get: { model.language }, set: { model.language = $0 })) {
            ForEach(GameLanguage.allCases) { lang in
                Text(lang.title).tag(lang)
            }
        }
        .pickerStyle(.segmented)
        .disabled(!model.hasChinesePatch)
    }

    private var startButton: some View {
        Button {
            model.start()
        } label: {
            Text("开始游戏").font(.title2.bold()).frame(maxWidth: .infinity).padding(.vertical, 8)
        }
        .buttonStyle(.borderedProminent)
    }

    private var importSection: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                #if os(tvOS)
                Text("Apple TV 没有文件管理，请通过局域网从 Mac 发送游戏文件。")
                #elseif os(visionOS)
                Text("方式一：在 Mac 上用隔空投送把游戏目录发送到 Vision Pro，存入「文件 › 我的 Apple Vision Pro › 美好的每一天」。")
                Text("方式二：开启下方的局域网接收，在 Mac 上运行传输脚本。")
                #else
                Text("方式一：用数据线连接 Mac，在访达中选中本设备 › 文件 › 美好的每一天，把整个游戏目录拖进去。")
                Text("方式二：开启下方的局域网接收，在 Mac 上运行传输脚本。")
                #endif
                Toggle("局域网接收游戏文件", isOn: Binding(
                    get: { transfer.running },
                    set: { $0 ? model.transfer.start() : model.transfer.stop() }))
                if transfer.running {
                    Text("配对码：\(transfer.code)").font(.title3.monospacedDigit().bold())
                    ForEach(TransferServer.addresses(), id: \.self) { ip in
                        Text("python3 tools/push_data.py --host \(ip) --code \(transfer.code) <游戏目录>")
                            .font(.system(.footnote, design: .monospaced))
                    }
                    if transfer.bytesReceived > 0 {
                        Text("已接收 \(ByteCountFormatter.string(fromByteCount: transfer.bytesReceived, countStyle: .file))")
                            .foregroundStyle(.secondary)
                    }
                    if !transfer.lastEvent.isEmpty {
                        Text(transfer.lastEvent).font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Button("重新检查") { model.refresh() }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            Text("导入游戏文件")
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("操作：轻点 = 左键 · 双指轻点 = 右键（菜单）· 上下滑动 = 滚轮（回想）· 长按 = 快进")
            Text("本应用不包含任何游戏内容，游戏数据需由玩家自行购买。")
        }
        .font(.footnote)
        .foregroundStyle(.secondary)
    }
}

/// A titled panel (GroupBox is not available on tvOS).
struct Card<Content: View, Label: View>: View {
    @ViewBuilder var content: Content
    @ViewBuilder var label: Label

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            label.font(.headline)
            content
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color.white.opacity(0.08)))
    }
}

/// Republishes the transfer server's state for SwiftUI (the server itself
/// lives in the model and outlives the view).
final class TransferServerObserver: ObservableObject {
    static let shared = TransferServerObserver()

    @Published var running = false
    @Published var lastEvent = ""
    @Published var bytesReceived: Int64 = 0
    @Published var code = ""
    private var bound = false

    func bind(_ server: TransferServer) {
        guard !bound else { return }
        bound = true
        server.$running.receive(on: DispatchQueue.main).assign(to: &$running)
        server.$lastEvent.receive(on: DispatchQueue.main).assign(to: &$lastEvent)
        server.$bytesReceived.receive(on: DispatchQueue.main).assign(to: &$bytesReceived)
        server.$code.receive(on: DispatchQueue.main).assign(to: &$code)
    }
}
