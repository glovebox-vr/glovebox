import SwiftUI
import SteamLauncherKit

/// The game's page: the art, the game's name, one big button and one line of status. The button is
/// whatever comes next (Install, then Play), so a first-time player never has to look for it.
/// Saves sync on their own (see `LauncherModel.play`), so there is nothing to manage here.
struct LibraryView: View {
    @EnvironmentObject var model: LauncherModel
    var onPlay: () -> Void
    @State private var drift = false
    /// Someone who has never installed the game sees the three steps (sign in, download, play)
    /// carried over from the sign-in card, through to Play. Returning players never see them.
    @State private var firstTime = false
    @State private var showShareAsk = false
    @State private var showLicenses = false
    @State private var showDiagnostics = false
    /// The game's language (GameLanguage): "auto" or Steam's name for one. The game takes it at the next Play.
    @AppStorage(GameLanguage.defaultsKey) private var gameLanguage = GameLanguage.automatic.id

    var body: some View {
        VStack(alignment: .leading, spacing: 36) {
            GameTitle(title: model.profile.title, size: 60)

            // A fixed height, so the title stays put when the one-line status becomes the taller
            // download progress.
            HStack(alignment: .center, spacing: 32) {
                actionButton
                    .transition(.scale(scale: 0.9).combined(with: .opacity))
                    .id(stage)
                status
                    .transition(.opacity)
                    .id(statusStage)
            }
            .frame(height: 84)
            .animation(.spring(duration: 0.5), value: stage)
        }
        .padding(.horizontal, 64)
        .padding(.bottom, 56)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
        // Art as a background so its size never pushes the controls out of the window. It drifts
        // in very slowly, so the page feels alive without anything moving for attention.
        .background {
            HeroArt(url: model.profile.heroImageURL)
                .scaleEffect(drift ? 1.06 : 1, anchor: .center)
                .animation(.linear(duration: 40).repeatForever(autoreverses: true), value: drift)
                .overlay(LinearGradient(colors: [.black.opacity(0.35), .clear, .black.opacity(0.9)], startPoint: .top, endPoint: .bottom))
        }
        .overlay(alignment: .topLeading) {
            HStack(spacing: 16) {
                GloveboxChip()
                if let version = model.gloveboxUpdate {
                    GloveboxUpdateButton(version: version).transition(.opacity)
                }
                if firstTime {
                    JourneySteps(current: journeyStep)
                        .frame(width: 400)
                        .padding(.horizontal, 18)
                        .glassBackgroundEffect()
                        .transition(.opacity)
                }
            }
            .padding(28)
        }
        .overlay(alignment: .topTrailing) { accountMenu.padding(28) }
        // A few seconds into the download, once the player has seen it start.
        .task(id: askToShare) {
            guard askToShare else { return }
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            if askToShare { showShareAsk = true }
        }
        .sheet(isPresented: $showShareAsk) {
            ShareStuttersSheet { on in
                model.setShareStutters(on)
                showShareAsk = false
            }
        }
        .sheet(isPresented: $showLicenses) { LicensesSheet() }
        .sheet(isPresented: $showDiagnostics) { DiagnosticsSheet() }
        .task {
            // The design-review screen with the sheet open (DemoState).
            guard DemoState.current == .diagnostics else { return }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            showDiagnostics = true
        }
        .clipped()
        .onAppear {
            drift = true
            firstTime = model.launcher.installed == nil && DemoState.current?.returningPlayer != true
        }
    }

    /// Asked while the game downloads, when the Mac installer didn't get an answer: the player is
    /// waiting anyway.
    private var askToShare: Bool {
        guard model.shareStutters == nil else { return false }
        switch model.install {
        case .downloading, .paused: return true
        default: return false
        }
    }

    /// Which status is showing: the button's stage, and while it reads Play, where the saves are (not every file).
    private var statusStage: String {
        guard stage == "play" || stage == "launching" else { return stage }
        switch model.saves {
        case .syncing: return stage + "-saves"
        case .failed: return stage + "-offline"
        default: return stage
        }
    }

    private var journeyStep: Int {
        if case .installed = model.install { return 2 }
        return 1
    }

    /// Which button is showing, for the hand-off animation between them (not every progress tick).
    private var stage: String {
        switch model.install {
        case .unknown, .preparing: return "wait"
        case .notInstalled: return "install"
        case .downloading: return "downloading"
        case .paused: return "paused"
        case .installed(true): return "update"
        case .installed(false):
            if model.preparing != nil { return "preparing" }
            if model.launching { return "launching" }
            return model.controllerIssue == nil ? "play" : "controllers"
        case .failed: return "failed"
        }
    }

    @ViewBuilder private var actionButton: some View {
        switch model.install {
        case .unknown, .preparing:
            Button {} label: { spinner }
                .bigButton()
                .disabled(true)
        case .notInstalled:
            Button { model.startInstall() } label: { actionLabel("Install", "arrow.down") }
                .bigButton()
                .disabled(!model.hasRoomForInstall)
        case .failed:
            Button { model.startInstall() } label: { actionLabel("Try Again", "arrow.clockwise") }
                .bigButton()
        case .downloading:
            // While downloading the progress is the news, so Pause steps back to a quiet button.
            Button { model.pauseInstall() } label: { actionLabel("Pause", "pause.fill") }
                .bigButton(prominent: false)
        case .paused:
            Button { model.startInstall() } label: { actionLabel("Resume", "arrow.down") }
                .bigButton()
        case .installed(let update):
            if update {
                Button { model.startInstall() } label: { actionLabel("Update", "arrow.down") }
                    .bigButton()
            } else {
                // The white pill, a soft glow and the boldest type on the page: the moment it's all for.
                // Without both controllers it waits, unlit, and lights up by itself when they're on.
                let busy = model.launching || model.preparing != nil
                let ready = model.controllerIssue == nil
                Button { model.play(onPlay) } label: {
                    if busy { spinner }
                    else { actionLabel("Play", "play.fill", weight: .bold) }
                }
                .bigButton()
                .disabled(!ready || model.preparing != nil)
                .shadow(color: .white.opacity(ready && !busy ? 0.35 : 0), radius: 22)
            }
        }
    }

    static let buttonWidth: CGFloat = 220

    /// A spinner in a button exactly the size of a labelled one, so the row doesn't jump.
    private var spinner: some View {
        actionLabel("Play", "play.fill").hidden().overlay { ProgressView() }
    }

    /// Icon and word at one size, so the glyph sits on the text's line. Every state's button is
    /// the same size, so one turns into the next without the row moving.
    private func actionLabel(_ title: String, _ symbol: String, weight: Font.Weight = .semibold) -> some View {
        Label(title, systemImage: symbol)
            .font(.title2.weight(weight))
            .frame(width: Self.buttonWidth)
    }

    @ViewBuilder private var status: some View {
        switch model.install {
        case .notInstalled:
            if model.hasRoomForInstall {
                Text("\(model.downloadBytes.bytes) download").font(.title3).foregroundStyle(.secondary)
            } else {
                Label("Needs \(model.downloadBytes.bytes) of free space", systemImage: "externaldrive.badge.exclamationmark")
                    .font(.title3).foregroundStyle(.orange)
            }
        case .downloading(let p):
            DownloadStatus(progress: p, paused: false)
        case .paused(let p):
            DownloadStatus(progress: p, paused: true)
        case .installed(updateAvailable: false):
            if let preparing = model.preparing {
                PreparingStatus(preparing: preparing)
            } else if model.launching {
                if case .syncing(let p) = model.saves {
                    SavesStatus(progress: p, launching: true)
                } else {
                    SavesStatus(progress: CloudSync.Progress(stage: .checking), launching: true)
                }
            } else if let issue = model.controllerIssue {
                ControllerStatus(issue: issue, connected: model.controllers.connected)
            } else {
                switch model.saves {
                case .syncing(let p):
                    SavesStatus(progress: p, launching: false)
                case .failed:
                    // Play tries again before the game starts.
                    Label("Couldn't reach Steam Cloud", systemImage: "exclamationmark.icloud")
                        .font(.title3).foregroundStyle(.secondary)
                case .synced:
                    // With LauncherModel.cloudUploads off, saves only come from Steam.
                    Label(LauncherModel.cloudUploads ? "Saves in sync with Steam" : "Saves from Steam",
                          systemImage: LauncherModel.cloudUploads ? "checkmark.icloud" : "icloud.and.arrow.down")
                        .font(.title3).foregroundStyle(.secondary)
                case nil:
                    EmptyView()
                }
            }
        case .installed(updateAvailable: true):
            Text("A new version is ready").font(.title3).foregroundStyle(.secondary)
        case .failed(let msg):
            Label(msg, systemImage: "exclamationmark.circle.fill").font(.title3).foregroundStyle(.orange)
        default:
            EmptyView()
        }
    }

    private var accountMenu: some View {
        Menu {
            // Steam's per-game language setting, which the headset has no Steam client for: text and subtitles (the
            // voice is English in every language). Automatic is the headset's own language when the game has it.
            Picker(selection: $gameLanguage) {
                Text("Automatic (\(GameLanguage.resolve(GameLanguage.automatic.id).name))")
                    .tag(GameLanguage.automatic.id)
                ForEach(GameLanguage.alyx) { Text($0.name).tag($0.id) }
            } label: {
                Label("Game Language", systemImage: "globe")
            }
            .pickerStyle(.menu)
            Toggle("Help Improve Glovebox", isOn: Binding(get: { model.shareStutters ?? false },
                                                          set: { model.setShareStutters($0) }))
            Button("Share Diagnostics") { showDiagnostics = true }
            Button("Licenses") { showLicenses = true }
            Button("Sign out", role: .destructive) { model.signOut() }
        } label: {
            Label(model.accountName, systemImage: "person.crop.circle.fill")
                .font(.callout.weight(.medium))
        }
        .buttonStyle(.plain)
        .padding(.horizontal, 16).padding(.vertical, 10)
        .glassBackgroundEffect()
    }
}

/// The one question about reports, as a visionOS sheet, with the same words as the Mac's: a big
/// ask, why it helps, and at a glance what we get and what never leaves the headset. The lists
/// match what a report holds (glovebox-vr/glovebox diagnostics/README.md).
private struct ShareStuttersSheet: View {
    var answer: (Bool) -> Void

    var body: some View {
        VStack(spacing: 28) {
            VStack(spacing: 12) {
                Text("Help make Glovebox better").font(.largeTitle.weight(.bold))
                Text("Glovebox is an open-source fan project. Let us see where it stutters, and we'll fix it for everyone.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack(alignment: .top, spacing: 16) {
                ShareList(title: "We get", shared: true, items: [
                    "Where the game stutters",
                    "Your Vision Pro model and visionOS version",
                ])
                ShareList(title: "Never shared", shared: false, items: [
                    "Your name, email or accounts",
                    "Eye, hand and face tracking",
                    "Camera and room data",
                ])
            }
            VStack(spacing: 12) {
                Button { answer(true) } label: { Text("Count Me In").frame(maxWidth: .infinity) }
                    .bigButton()
                Button { answer(false) } label: { Text("No Thanks").frame(maxWidth: .infinity) }
                    .bigButton(prominent: false)
            }
        }
        .padding(40)
        .frame(width: 640)
        .presentationSizing(.fitted)
        .interactiveDismissDisabled()
    }
}

/// What's in a report, or what never is: a heading and a few lines, each with an orange tick or a
/// red cross.
private struct ShareList: View {
    var title: String
    var shared: Bool
    var items: [String]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.callout.weight(.semibold))
                .textCase(.uppercase)
                .foregroundStyle(.secondary)
            ForEach(items, id: \.self) { item in
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: shared ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .foregroundStyle(.white, shared ? Color.green : Color(red: 1, green: 0.36, blue: 0.36))
                    Text(item).fixedSize(horizontal: false, vertical: true)
                }
                .font(.body)
            }
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }
}

/// Time left first (what people want to know), the bar, and the one thing a first-timer
/// wonders: whether they have to keep the window open.
private struct DownloadStatus: View {
    let progress: DownloadProgress
    let paused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(headline).font(.title3.weight(.semibold))
                Spacer()
                Text("\(Int(progress.fraction * 100))%")
                    .font(.title3.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
            ProgressView(value: progress.fraction)
            Text(paused ? "\(progress.doneBytes.bytes) of \(progress.totalBytes.bytes)"
                        : "Keeps downloading if you close this window")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .frame(width: 420)
    }

    private var headline: String {
        if paused { return "Paused" }
        if let left = progress.secondsLeft { return timeLeftText(left) }
        return "Downloading"
    }
}

/// After Play, while the game gets ready: what's happening, how far along, and the one thing to know.
/// The game app drives it through `LauncherModel.preparing`.
private struct PreparingStatus: View {
    let preparing: LauncherModel.Preparing

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(preparing.title).font(.title3.weight(.semibold))
                Spacer()
                Text(howLong)
                    .font(.title3.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
            ProgressView(value: min(max(preparing.fraction, 0), 1))
            Text(preparing.firstTimeOnly ? "First time only · Keep this window open" : "Keep this window open")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .frame(width: 420)
    }

    /// Time left when the game can tell, else how far along.
    private var howLong: String {
        guard let left = preparing.secondsLeft else { return "\(Int(preparing.fraction * 100))%" }
        return left < 60 ? "Almost done" : timeLeftText(left)
    }
}

/// Saves on their way to or from Steam Cloud: what's happening, how far along, and always moving, so it's plain the
/// launcher is at work. After Play it's what the game waits for, which is usually only the quick look at the cloud:
/// the sync that started with the launcher has brought the saves by then.
private struct SavesStatus: View {
    let progress: CloudSync.Progress
    let launching: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Image(systemName: symbol)
                    .foregroundStyle(Brand.orange)
                    .symbolEffect(.pulse, options: .repeating)
                    .accessibilityHidden(true)
                Text(headline).font(.title3.weight(.semibold))
                Spacer()
                if progress.total > 0 {
                    Text("\(progress.done) of \(progress.total)")
                        .font(.title3.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .contentTransition(.numericText())
                }
            }
            if progress.total > 0 {
                ProgressView(value: Double(progress.done), total: Double(progress.total))
            } else {
                SweepingBar()
            }
            Text(caption)
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .frame(width: 420)
        .animation(.easeOut(duration: 0.35), value: progress)
        .accessibilityElement(children: .combine)
    }

    private var headline: String {
        switch progress.stage {
        case .checking: return launching ? "Checking Steam Cloud" : "Syncing saves"
        case .downloading: return "Downloading saves"
        case .uploading: return "Uploading saves"
        }
    }

    private var symbol: String {
        switch progress.stage {
        case .checking: return "arrow.triangle.2.circlepath.icloud"
        case .downloading: return "icloud.and.arrow.down"
        case .uploading: return "icloud.and.arrow.up"
        }
    }

    private var caption: String {
        launching ? "Your latest saves, then the game" : "Steam Cloud · Play anytime"
    }
}

/// A progress bar for work with no count yet: a glow sweeping along the track, so it never sits still.
private struct SweepingBar: View {
    @State private var sweep = false

    var body: some View {
        ProgressView(value: 0)
            .overlay {
                GeometryReader { g in
                    Capsule()
                        .fill(.tint)
                        .frame(width: g.size.width * 0.3)
                        .offset(x: sweep ? g.size.width : -g.size.width * 0.3)
                }
                .clipShape(.capsule)
            }
            .onAppear {
                withAnimation(.easeInOut(duration: 1.1).repeatForever(autoreverses: false)) { sweep = true }
            }
            .accessibilityLabel("In progress")
    }
}

private extension Button {
    /// The page's one big button: Glovebox's white pill, or glass when it isn't the point.
    func bigButton(prominent: Bool = true) -> some View {
        buttonStyle(PillButtonStyle(prominent: prominent))
    }
}

/// The account menu's "Licenses": the license texts of everything the app carries (tools/licenses.py).
private struct LicensesSheet: View {
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            ScrollView {
                Text(LicensesText.all)
                    .font(.system(.footnote, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(24)
            }
            .navigationTitle("Licenses")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button { dismiss() } label: { Image(systemName: "xmark") }
                }
            }
        }
        .frame(minWidth: 720, minHeight: 560)
    }
}
