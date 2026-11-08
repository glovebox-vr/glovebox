#if KL_ALYX_LAUNCHER
import SwiftUI
import UIKit
import SteamLauncherKit

// Half-Life: Alyx with its Steam launcher (the project repo's launcher/, embedded unchanged; gen_xcodeproj.py with
// KLEPTON_ALYX_LAUNCHER). The launcher's screens are the app's first window: sign in, ownership, download, saves.
// Play hands over to BootView, which starts the game exactly as an icon launch of the plain app does — configure,
// boot, the .mixed immersive space, and this window closed behind it.
//
// Straight to the game, no launcher, for a test run: a test script's Documents/alyx-env.txt with the line
// GLOVEBOX_TEST=1, pushed for this install (tools/alyx-test/morning.sh, device-batch.sh; kl_app_test_run),
// or the launch argument -klgame (KL_GAME=1); -launcher (KL_LAUNCHER=1) shows the launcher even then. Any other pushed
// file is an old test's: it never skips the launcher (one left a headset without cloud sync, 2026-10-12), and
// kl_alyx_app.c sets it aside at boot.

enum AlyxLauncherGate {
    static let skip: Bool = {
        let args = ProcessInfo.processInfo.arguments
        if args.contains("-launcher") || klEnvOn("KL_LAUNCHER", default: false) { return false }
        if args.contains("-klgame") || klEnvOn("KL_GAME", default: false) { return true }
        return kl_app_test_run(Paths.resources, Paths.container) != 0
    }()
}

/// The first window: the launcher, or straight to the game. After Play the launcher stays on screen, showing the game's
/// progress (LauncherModel.preparing: the first launch's pipeline replay, then the map loading), and BootView runs
/// hidden under it; BootView closes the window when the game shows its first frame (KleptonApp.swift), so the boot
/// report never flashes up and the immersive space is never just black. If boot fails, BootView's report shows.
struct AlyxLauncherHost: View {
    var body: some View {
        if AlyxLauncherGate.skip {
            BootView()
        } else {
            AlyxLauncherScreens()
        }
    }
}

/// Owns the launcher's model, so a run that goes straight to the game never creates one (its init signs in to
/// Steam). The launcher's own RootView picks the screen; the scene-phase hook is the one the stand-alone app has.
struct AlyxLauncherScreens: View {
    @StateObject private var model = LauncherModel()
    @Environment(\.scenePhase) private var scenePhase
    @State private var playing = false
    @State private var bootFailed = false
    @State private var prewarmStarted = false

    var body: some View {
        ZStack {
            if playing {
                BootView()
                    .opacity(bootFailed ? 1 : 0)
                    .allowsHitTesting(bootFailed)
                    .accessibilityHidden(!bootFailed)
            }
            if !bootFailed {
                RootView(onPlay: play)
                    .environmentObject(model)
            }
        }
        .onChange(of: scenePhase, initial: true) { _, phase in model.scenePhaseChanged(phase) }
        .task { StutterReports.start() }  // a stutter report, if the player said yes: one per launch
        .onChange(of: model.install, initial: true) { _, state in prewarm(state) }
    }

    /// "Preparing graphics" before Play, in the shared space: once the game is installed, the shader cache is warmed
    /// with no game running (kl_app_prewarm, on a device made as the game makes its own) while this window shows the
    /// progress and holds Play (LauncherModel.preparing), so the player can use other apps meanwhile. Play then finds
    /// the cache warm: the game's own replay takes seconds and the immersive space opens on the map loading. When it
    /// cannot run (no usable device settings), the game's own replay at Play is the fallback, in passthrough.
    private func prewarm(_ state: LauncherModel.InstallState) {
        guard !prewarmStarted, !playing, case .installed(updateAvailable: false) = state else { return }
        prewarmStarted = true
        model.preparing = LauncherModel.Preparing(fraction: 0, secondsLeft: nil, firstTimeOnly: false)
        let resources = Paths.resources, docs = Paths.container
        let run = Task.detached(priority: .userInitiated) { () -> Int32 in
            let t0 = Date()
            let rc = kl_app_prewarm(resources, docs)
            NSLog("[launcher] Preparing graphics before Play: %@ in %.1f s",
                  rc == 0 ? "done" : "not possible (\(rc)), the game's own replay is the fallback",
                  Date().timeIntervalSince(t0))
            return rc
        }
        Task { @MainActor in
            let poll = Task { @MainActor in
                var estimate = ReplayEstimate()
                while !Task.isCancelled {
                    var done: UInt32 = 0, total: UInt32 = 0, elapsed: Double = 0
                    if kl_foz_progress(&done, &total, &elapsed) == 1, total > 0, var p = model.preparing {
                        p.fraction = Double(done) / Double(total)
                        p.secondsLeft = estimate.secondsLeft(done: done, total: total, elapsed: elapsed)
                        p.firstTimeOnly = elapsed > 10
                        model.preparing = p
                    }
                    try? await Task.sleep(nanoseconds: 300_000_000)
                }
            }
            _ = await run.value
            poll.cancel()
            if !playing { model.preparing = nil }
        }
    }

    /// LibraryView's Play button runs model.play(onPlay) itself: the latest saves first (usually a quick look at the
    /// cloud; a stalled sync is cut off after 10 s), then this.
    private func play() {
        NSLog("[launcher] Play: saves synced, starting the game")
        // The language the player chose in the account menu (or the headset's own, for Automatic), as Steam's name:
        // kl_app_boot puts it on the game's command line as -language. Set before BootView boots, which is when it's read.
        let language = GameLanguage.resolve(GameLanguage.choice)
        setenv("KL_ALYX_LANGUAGE", language.id, 1)
        NSLog("[launcher] game language: %@ (chosen: %@)", language.id, GameLanguage.choice)
        Lifecycle.gameStarted = true
        model.preparing = LauncherModel.Preparing(fraction: 0, secondsLeft: nil, firstTimeOnly: false, title: "Starting")
        playing = true
        Task { @MainActor in await followGame() }
    }

    /// The game's progress on the launcher page until its first frame: the pipeline replay (cold on a first launch,
    /// ~2.5 minutes on the headset; seconds once the cache is warm), then the map loading.
    @MainActor private func followGame() async {
        var replayEnd: Date?
        var estimate = ReplayEstimate()
        // after a prewarm the progress already reads done: the map loading starts after the game's own replay
        var sawReplay = false
        while !Lifecycle.firstPicture {
            if Lifecycle.bootFailed { bootFailed = true; model.preparing = nil; return }
            var done: UInt32 = 0, total: UInt32 = 0, elapsed: Double = 0
            let state = kl_foz_progress(&done, &total, &elapsed)
            var p = model.preparing ?? LauncherModel.Preparing(fraction: 0)
            if state == 1 && total > 0 {
                // the replay: 90% of the bar; its time left from the rate so far, plus ~12 s of loading after it
                let f = Double(done) / Double(total)
                sawReplay = true
                p.title = "Preparing graphics"
                p.fraction = 0.9 * f
                p.secondsLeft = estimate.secondsLeft(done: done, total: total, elapsed: elapsed) + 12
                p.firstTimeOnly = elapsed > 10
            } else if state == 2 && (sawReplay || !prewarmStarted) {
                // the map: the last 10%, creeping, since the game reports no progress of its own
                if replayEnd == nil { replayEnd = Date() }
                let t = Date().timeIntervalSince(replayEnd!)
                p.title = "Loading"
                p.fraction = 0.9 + 0.09 * min(1, t / 15)
                p.secondsLeft = max(1, 12 - t)
            }
            model.preparing = p
            try? await Task.sleep(nanoseconds: 300_000_000)
        }
        model.preparing = nil
    }
}

/// The system's background download events, for the launcher's chunk fetcher. The app's delegate; the launcher's
/// own AppDelegate (RootView.swift, for the stand-alone app) compiles in unused.
final class AlyxLauncherDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        LauncherModel.backgroundFetcher.activate()
        return true
    }

    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        guard identifier == LauncherModel.backgroundFetcher.identifier else { return completionHandler() }
        LauncherModel.backgroundFetcher.handleEvents(completion: completionHandler)
    }
}

/// Time left in a pipeline replay (kl_foz_progress): the pace over the last 20 s, once 30 s in; before that what a
/// first launch measured on an M5 (185.8 s for 3,038 pipelines on 6 threads). The pace since the start overstates it,
/// because the first seconds are the slowest: it read 8-9 minutes for a 3.1-minute replay.
struct ReplayEstimate {
    private var recent: [(t: Double, done: Double)] = []

    mutating func secondsLeft(done: UInt32, total: UInt32, elapsed: Double) -> Double {
        recent.append((elapsed, Double(done)))
        recent.removeAll { elapsed - $0.t > 20 }
        var perPipeline = 0.061
        if elapsed >= 30, let first = recent.first, elapsed - first.t >= 10, Double(done) > first.done {
            perPipeline = (elapsed - first.t) / (Double(done) - first.done)
        }
        return max(0, Double(total) - Double(done)) * perPipeline
    }
}
#endif
