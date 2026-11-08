import XCTest
@testable import SteamLauncherKit

/// A Steam Cloud in memory.
final class FakeCloud: CloudRemote {
    var files: [String: (data: Data, time: Date)] = [:]
    var failUploads = false
    var uploads = 0

    func list() async throws -> [CloudFile] {
        files.map { name, v in
            var f = CloudFile()
            f.filename = name
            f.timestamp = UInt64(v.time.timeIntervalSince1970)
            f.size = UInt32(v.data.count)
            f.sha1Hex = Codec.sha1(v.data).hex
            return f
        }
    }
    var downloads = 0
    /// Runs before each download, e.g. to change the cloud or cancel the sync mid-run.
    var beforeDownload: ((CloudFile) -> Void)?
    func download(_ file: CloudFile) async throws -> Data {
        beforeDownload?(file)
        downloads += 1
        return files[file.filename]!.data
    }
    func upload(_ batch: [(name: String, data: Data, time: Date)]) async throws {
        if failUploads { throw SteamError.badData("network down") }
        uploads += 1
        for f in batch { files[f.name] = (f.data, f.time) }
    }
}

final class CloudSyncTests: XCTestCase {
    var root: URL!
    var cloud: FakeCloud!
    let rules = [CloudSaveRule(root: "gameinstall", path: "game/hlvr/save", pattern: "*", recursive: true)]
    let t0 = Date(timeIntervalSince1970: 1_790_000_000)

    override func setUp() {
        root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try! FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        cloud = FakeCloud()
    }
    override func tearDown() { try? FileManager.default.removeItem(at: root) }

    /// Same mapping as the Alyx profile: depot `game/` is the install folder.
    func makeSync(uploads: Bool = true) -> CloudSync {
        var s = CloudSync(remote: cloud, rules: rules, rootFolder: { _ in self.root },
                          localPath: GameProfile.halfLifeAlyx.localPath,
                          stateURL: root.appendingPathComponent(".state.json"),
                          backupRoot: root.appendingPathComponent(".backups"))
        s.uploads = uploads
        s.neverUpload = GameProfile.halfLifeAlyx.cloudNeverUpload
        return s
    }

    func sync(uploads: Bool = true) async throws -> CloudSync.Report { try await makeSync(uploads: uploads).run() }
    /// The quick check, which never downloads anything.
    func cloudChanged(file: StaticString = #filePath, line: UInt = #line) async throws -> Bool {
        let before = cloud.downloads
        let changed = try await makeSync().cloudChangedSinceLastSync()
        XCTAssertEqual(cloud.downloads, before, "a quick check downloads nothing", file: file, line: line)
        return changed
    }

    func name(_ f: String) -> String { "%GameInstall%game/hlvr/save/\(f)" }
    func local(_ f: String) -> URL { root.appendingPathComponent("hlvr/save/\(f)") }
    func writeLocal(_ f: String, _ s: String, _ t: Date) throws {
        try FileManager.default.createDirectory(at: local(f).deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(s.utf8).write(to: local(f))
        try FileManager.default.setAttributes([.modificationDate: t], ofItemAtPath: local(f).path)
    }
    func readLocal(_ f: String) -> String? { (try? Data(contentsOf: local(f))).map { String(decoding: $0, as: UTF8.self) } }
    func backups(_ side: String = "") -> [String] {
        let e = FileManager.default.enumerator(atPath: root.appendingPathComponent(".backups").path)
        return (e?.allObjects as? [String] ?? []).filter { $0.hasSuffix(".sav") && $0.contains("/\(side)") }
    }
    func backup(_ path: String) -> String? {
        (try? Data(contentsOf: root.appendingPathComponent(".backups").appendingPathComponent(path)))
            .map { String(decoding: $0, as: UTF8.self) }
    }
    func sha(_ s: String) -> String { Codec.sha1(Data(s.utf8)).hex }
    /// The headset's container is case-sensitive; a Mac's disk usually isn't.
    func requireCaseSensitiveDisk() throws {
        let probe = root.appendingPathComponent("case-probe")
        try FileManager.default.createDirectory(at: probe, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: probe) }
        if FileManager.default.fileExists(atPath: root.appendingPathComponent("CASE-PROBE").path) {
            throw XCTSkip("this disk ignores case")
        }
    }

    func testFirstSyncBringsCloudSavesDown() async throws {
        cloud.files[name("quick.sav")] = (Data("pc".utf8), t0)
        let r = try await sync()
        XCTAssertEqual(readLocal("quick.sav"), "pc")
        XCTAssertEqual(r.downloaded, [name("quick.sav")])
    }

    func testNewHeadsetSaveGoesUp() async throws {
        try writeLocal("slot1.sav", "vp", t0)
        let r = try await sync()
        XCTAssertEqual(r.uploaded, [name("slot1.sav")])
        XCTAssertEqual(cloud.files[name("slot1.sav")]?.data, Data("vp".utf8))
    }

    func testOneSideChangedWins() async throws {
        cloud.files[name("a.sav")] = (Data("v1".utf8), t0)
        _ = try await sync()
        // Played on the headset: local changes, cloud doesn't. Clock skew must not matter.
        try writeLocal("a.sav", "v2-headset", t0.addingTimeInterval(-9999))
        var r = try await sync()
        XCTAssertEqual(r.uploaded, [name("a.sav")])
        XCTAssertEqual(cloud.files[name("a.sav")]?.data, Data("v2-headset".utf8))
        // Then played on the PC: cloud changes, local doesn't.
        XCTAssertEqual(backups("cloud").map(backup), ["v1"], "the cloud's copy is kept before it's replaced")
        cloud.files[name("a.sav")] = (Data("v3-pc".utf8), t0.addingTimeInterval(-99999))
        r = try await sync()
        XCTAssertEqual(r.downloaded, [name("a.sav")])
        XCTAssertEqual(readLocal("a.sav"), "v3-pc")
        XCTAssertEqual(backups("headset").map(backup), ["v2-headset"], "the replaced headset save is kept")
    }

    func testBothChangedKeepsTheCloudsAndSetsTheHeadsetsAside() async throws {
        cloud.files[name("a.sav")] = (Data("v1".utf8), t0)
        _ = try await sync()
        // Even when the headset's copy is the newer one: never upload over a version it didn't come from.
        try writeLocal("a.sav", "headset", t0.addingTimeInterval(300))
        cloud.files[name("a.sav")] = (Data("pc".utf8), t0.addingTimeInterval(200))
        let r = try await sync()
        XCTAssertEqual(readLocal("a.sav"), "pc")
        XCTAssertEqual(cloud.files[name("a.sav")]?.data, Data("pc".utf8))
        XCTAssertTrue(r.uploaded.isEmpty)
        XCTAssertEqual(backups("headset").map(backup), ["headset"])
    }

    func testLooseFileWithNoHistoryNeverOverwritesTheCloud() async throws {
        cloud.files[name("a.sav")] = (Data("pc".utf8), t0)
        try writeLocal("a.sav", "headset", t0.addingTimeInterval(500))
        let r = try await sync()
        XCTAssertEqual(readLocal("a.sav"), "pc")
        XCTAssertEqual(cloud.files[name("a.sav")]?.data, Data("pc".utf8))
        XCTAssertEqual(cloud.uploads, 0)
        XCTAssertEqual(r.backedUp.count, 1)
        XCTAssertEqual(backups("headset").map(backup), ["headset"])
    }

    /// The cloud spells some names in capitals (`SAVE/S0`, from a Windows PC); the game only ever
    /// looks in lowercase `save/s0` on the headset's case-sensitive disk.
    func testCloudSpellingLandsInTheGamesLowercaseFolder() async throws {
        let upper = "%GameInstall%game/hlvr/SAVE/S0/autosave.sav"
        cloud.files[upper] = (Data("pc".utf8), t0)
        var r = try await sync()
        XCTAssertEqual(readLocal("s0/autosave.sav"), "pc")
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: root.appendingPathComponent("hlvr").path), ["save"])
        r = try await sync()
        XCTAssertEqual(r.unchanged, 1)
        // Played on the headset: it goes up under the cloud's own name, not as a second file.
        try writeLocal("s0/autosave.sav", "headset", t0.addingTimeInterval(100))
        r = try await sync()
        XCTAssertEqual(r.uploaded, [upper])
        XCTAssertEqual(Array(cloud.files.keys), [upper])
        XCTAssertEqual(cloud.files[upper]?.data, Data("headset".utf8))
    }

    /// The app links hlvr/SAVE to hlvr/save for the game's autosave commit (kl_app.c). The link is not a second folder:
    /// nothing is folded, moved or backed up, each save goes up once under its one name, and the link stays.
    func testTheGamesSaveLinkIsNotASecondSpelling() async throws {
        try requireCaseSensitiveDisk()
        try writeLocal("s0/autosave.sav", "headset", t0)
        let link = root.appendingPathComponent("hlvr/SAVE").path
        try FileManager.default.createSymbolicLink(atPath: link, withDestinationPath: "save")

        var r = try await sync()
        XCTAssertEqual(r.uploaded, [name("s0/autosave.sav")])
        XCTAssertEqual(Array(cloud.files.keys), [name("s0/autosave.sav")])
        XCTAssertEqual(readLocal("s0/autosave.sav"), "headset")
        XCTAssertEqual(backups(), [])
        XCTAssertEqual(try FileManager.default.destinationOfSymbolicLink(atPath: link), "save")

        r = try await sync()
        XCTAssertEqual(r.unchanged, 1)
        XCTAssertEqual(r.uploaded, [])
        XCTAssertEqual(try FileManager.default.destinationOfSymbolicLink(atPath: link), "save")

        // The game saves through the link: one file changed, one upload.
        try Data("commit".utf8).write(to: URL(fileURLWithPath: link).appendingPathComponent("s0/autosave.sav"))
        try FileManager.default.setAttributes([.modificationDate: t0.addingTimeInterval(100)],
                                              ofItemAtPath: local("s0/autosave.sav").path)
        r = try await sync()
        XCTAssertEqual(r.uploaded, [name("s0/autosave.sav")])
        XCTAssertEqual(cloud.files[name("s0/autosave.sav")]?.data, Data("commit".utf8))

        // A cloud save in capitals still lands in the lowercase folder, beside the link.
        cloud.files["%GameInstall%game/hlvr/SAVE/S1/autosave.sav"] = (Data("pc".utf8), t0)
        r = try await sync()
        XCTAssertEqual(readLocal("s1/autosave.sav"), "pc")
        XCTAssertEqual(try FileManager.default.destinationOfSymbolicLink(atPath: link), "save")
    }

    /// A new game started on the headset in slot s0, while the cloud already has a different s0:
    /// both games are kept, the headset's in the next free slot.
    func testNewHeadsetSlotNeverReplacesACloudSlot() async throws {
        let cloudS0 = "%GameInstall%game/hlvr/SAVE/S0/autosave.sav"
        cloud.files[cloudS0] = (Data("pc-2020".utf8), t0)
        cloud.files[name("s3/autosave.sav")] = (Data("pc-s3".utf8), t0)
        try writeLocal("s0/autosave.sav", "today", t0.addingTimeInterval(9999))
        try writeLocal("s0/personal.cfg", "cfg", t0.addingTimeInterval(9999))
        let r = try await sync()
        XCTAssertEqual(r.relocated, ["s0 → s1"], "the lowest slot nobody uses")
        XCTAssertEqual(readLocal("s0/autosave.sav"), "pc-2020")
        XCTAssertNil(readLocal("s0/personal.cfg"))
        XCTAssertEqual(readLocal("s1/autosave.sav"), "today")
        XCTAssertEqual(readLocal("s1/personal.cfg"), "cfg")
        XCTAssertEqual(cloud.files[cloudS0]?.data, Data("pc-2020".utf8))
        XCTAssertEqual(r.uploaded, [name("s1/autosave.sav"), name("s1/personal.cfg")], "with its settings, as a PC's slots")
        XCTAssertEqual(cloud.files[name("s1/personal.cfg")]?.data, Data("cfg".utf8))
        XCTAssertNil(cloud.files[name("s0/autosave.sav")])
    }

    /// The game makes empty folders for its unused slots on first launch (s5…s9 next to the cloud's
    /// s0…s4). They count as free: the headset's game goes to s5, not s10, which the game may not list.
    func testEmptySlotFoldersCountAsFree() async throws {
        for n in 0...4 { cloud.files[name("s\(n)/autosave.sav")] = (Data("pc\(n)".utf8), t0) }
        for n in 1...9 {
            try FileManager.default.createDirectory(at: local("s\(n)"), withIntermediateDirectories: true)
        }
        try writeLocal("s0/autosave.sav", "today", t0.addingTimeInterval(9999))
        let r = try await sync()
        XCTAssertEqual(r.relocated, ["s0 → s5"])
        XCTAssertEqual(readLocal("s5/autosave.sav"), "today")
        XCTAssertEqual(readLocal("s0/autosave.sav"), "pc0")
        XCTAssertEqual(readLocal("s4/autosave.sav"), "pc4")
        XCTAssertNil(readLocal("s10/autosave.sav"))
        XCTAssertEqual(cloud.files[name("s0/autosave.sav")]?.data, Data("pc0".utf8))
    }

    /// What build 17 left on a test headset: the cloud's S0–S1 written under hlvr/SAVE (invisible to
    /// the game), s3 under hlvr/save, and today's new game in save/s0 that the sync history would
    /// have uploaded over the cloud's S0.
    func testMigratesTheUppercaseTreeAnEarlierLauncherWrote() async throws {
        try requireCaseSensitiveDisk()
        let fm = FileManager.default
        func put(_ rel: String, _ s: String, _ t: Date) throws {
            let u = root.appendingPathComponent(rel)
            try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(s.utf8).write(to: u)
            try fm.setAttributes([.modificationDate: t], ofItemAtPath: u.path)
        }
        let s0 = "%GameInstall%game/hlvr/SAVE/S0/autosave.sav", s1 = "%GameInstall%game/hlvr/SAVE/S1/a.sav"
        let s3 = name("s3/a.sav")
        cloud.files[s0] = (Data("pc0".utf8), t0)
        cloud.files[s1] = (Data("pc1".utf8), t0)
        cloud.files[s3] = (Data("pc3".utf8), t0)
        try put("hlvr/SAVE/S0/autosave.sav", "pc0", t0)
        try put("hlvr/SAVE/S1/a.sav", "pc1", t0)
        try put("hlvr/save/s3/a.sav", "pc3", t0)
        let history = [s0.lowercased(): sha("pc0"), s1.lowercased(): sha("pc1"), s3.lowercased(): sha("pc3")]
        try JSONEncoder().encode(history).write(to: root.appendingPathComponent(".state.json"))
        try put("hlvr/save/s0/autosave.sav", "today", t0.addingTimeInterval(9999))
        try put("hlvr/save/s0/personal.cfg", "cfg", t0.addingTimeInterval(9999))

        let r = try await sync()
        XCTAssertEqual(r.relocated, ["s0 → s2"])
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: root.appendingPathComponent("hlvr").path), ["save"])
        XCTAssertEqual(readLocal("s0/autosave.sav"), "pc0")
        XCTAssertEqual(readLocal("s1/a.sav"), "pc1")
        XCTAssertEqual(readLocal("s3/a.sav"), "pc3")
        XCTAssertEqual(readLocal("s2/autosave.sav"), "today")
        XCTAssertEqual(readLocal("s2/personal.cfg"), "cfg")
        XCTAssertEqual(cloud.files[s0]?.data, Data("pc0".utf8), "the 2020 save is untouched")
        XCTAssertEqual(r.uploaded, [name("s2/autosave.sav"), name("s2/personal.cfg")])
        XCTAssertEqual(r.unchanged, 3)
        XCTAssertTrue(r.downloaded.isEmpty)
        let again = try await sync()
        XCTAssertEqual(again.unchanged, 5)
        XCTAssertTrue(again.uploaded.isEmpty && again.downloaded.isEmpty && again.relocated.isEmpty)
    }

    /// With uploads off, saves come down, nothing goes up, and the history isn't advanced for what
    /// would have, so it goes up once uploads are on.
    func testUploadsOffIsDownloadOnly() async throws {
        cloud.files[name("a.sav")] = (Data("v1".utf8), t0)
        cloud.files[name("pc.sav")] = (Data("pc".utf8), t0)
        _ = try await sync(uploads: false)
        try writeLocal("a.sav", "headset", t0.addingTimeInterval(100))
        try writeLocal("new.sav", "new", t0.addingTimeInterval(100))
        var r = try await sync(uploads: false)
        XCTAssertEqual(cloud.uploads, 0)
        XCTAssertEqual(Set(r.wouldUpload), [name("a.sav"), name("new.sav")])
        XCTAssertEqual(cloud.files[name("a.sav")]?.data, Data("v1".utf8))
        XCTAssertEqual(readLocal("pc.sav"), "pc")
        r = try await sync(uploads: true)
        XCTAssertEqual(Set(r.uploaded), [name("a.sav"), name("new.sav")])
        XCTAssertEqual(cloud.files[name("a.sav")]?.data, Data("headset".utf8))
    }

    /// The game's settings sync like any save, as on a PC; the level files it leaves behind never go up.
    func testSettingsSyncAndLevelFilesStayOnTheHeadset() async throws {
        cloud.files[name("s0/personal.cfg")] = (Data("pc".utf8), t0)
        _ = try await sync()
        XCTAssertEqual(readLocal("s0/personal.cfg"), "pc")
        try writeLocal("s0/personal.cfg", "headset", t0.addingTimeInterval(100))
        try writeLocal("a1_intro_world.hl1", "level", t0)
        try writeLocal("s5/autosave.sav", "save", t0)
        try writeLocal("s5/personal.cfg", "headset", t0)
        var r = try await sync(uploads: false)
        XCTAssertEqual(r.wouldUpload, [name("s0/personal.cfg"), name("s5/autosave.sav"), name("s5/personal.cfg")])
        r = try await sync()
        XCTAssertEqual(r.uploaded, [name("s0/personal.cfg"), name("s5/autosave.sav"), name("s5/personal.cfg")])
        XCTAssertEqual(Set(cloud.files.keys), [name("s0/personal.cfg"), name("s5/autosave.sav"), name("s5/personal.cfg")])
        XCTAssertEqual(cloud.files[name("s0/personal.cfg")]?.data, Data("headset".utf8))
        XCTAssertEqual(r.backedUp.filter { $0.hasSuffix("/cloud/game/hlvr/save/s0/personal.cfg") }.map(backup), ["pc"],
                       "the cloud's copy is kept before it's replaced")
        XCTAssertEqual(readLocal("s0/personal.cfg"), "headset")
        XCTAssertEqual(readLocal("a1_intro_world.hl1"), "level")
    }

    /// A reinstall starts with nothing on the headset: a slot started here comes back with its own settings,
    /// so the game never falls back to the global ones (a PC's, from 2020, set to teleport).
    func testReinstallBringsBackAHeadsetSlotWithItsSettings() async throws {
        cloud.files[name("personal.cfg")] = (Data("teleport, 2020".utf8), t0)
        try writeLocal("s5/autosave.sav", "van", t0.addingTimeInterval(100))
        try writeLocal("s5/personal.cfg", "continuous", t0.addingTimeInterval(100))
        var r = try await sync()
        XCTAssertEqual(r.uploaded, [name("s5/autosave.sav"), name("s5/personal.cfg")])

        try FileManager.default.removeItem(at: root)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        r = try await sync()
        XCTAssertEqual(Set(r.downloaded), [name("personal.cfg"), name("s5/autosave.sav"), name("s5/personal.cfg")])
        XCTAssertEqual(readLocal("s5/personal.cfg"), "continuous")
        XCTAssertEqual(readLocal("s5/autosave.sav"), "van")
        XCTAssertEqual(readLocal("personal.cfg"), "teleport, 2020")
        XCTAssertTrue(r.uploaded.isEmpty && r.relocated.isEmpty)
    }

    /// A slot started here goes up with its settings. When a PC carries on in it, the PC's copies
    /// come down into the same slot.
    func testSlotContinuedOnThePCStaysInPlace() async throws {
        try writeLocal("s5/autosave.sav", "headset", t0)
        try writeLocal("s5/personal.cfg", "headset-cfg", t0)
        _ = try await sync()
        XCTAssertEqual(cloud.files[name("s5/personal.cfg")]?.data, Data("headset-cfg".utf8))
        cloud.files[name("s5/autosave.sav")] = (Data("pc".utf8), t0.addingTimeInterval(600))
        cloud.files[name("s5/personal.cfg")] = (Data("pc-cfg".utf8), t0.addingTimeInterval(600))
        let r = try await sync()
        XCTAssertTrue(r.relocated.isEmpty)
        XCTAssertTrue(r.uploaded.isEmpty)
        XCTAssertEqual(readLocal("s5/autosave.sav"), "pc")
        XCTAssertEqual(readLocal("s5/personal.cfg"), "pc-cfg")
        XCTAssertEqual(backups("headset").map(backup), ["headset"], "the headset's copy is kept")
    }

    func testNothingToDoWhenInSync() async throws {
        cloud.files[name("a.sav")] = (Data("same".utf8), t0)
        try writeLocal("a.sav", "same", t0)
        let r = try await sync()
        XCTAssertEqual(r.unchanged, 1)
        XCTAssertTrue(r.downloaded.isEmpty && r.uploaded.isEmpty)
        XCTAssertEqual(cloud.uploads, 0)
    }

    func testDeletionsAreNotPropagatedOrUndone() async throws {
        cloud.files[name("old.sav")] = (Data("x".utf8), t0)
        try writeLocal("mine.sav", "y", t0)
        _ = try await sync()
        try FileManager.default.removeItem(at: local("old.sav"))   // the game deleted it here
        cloud.files[name("mine.sav")] = nil                         // deleted on the PC
        let r = try await sync()
        XCTAssertNil(readLocal("old.sav"), "not brought back")
        XCTAssertNotNil(cloud.files[name("old.sav")], "not deleted from the cloud")
        XCTAssertNil(cloud.files[name("mine.sav")], "not re-uploaded")
        XCTAssertEqual(readLocal("mine.sav"), "y", "not deleted here")
        XCTAssertTrue(r.uploaded.isEmpty && r.downloaded.isEmpty)
    }

    func testFailedUploadIsRetriedNextTime() async throws {
        try writeLocal("a.sav", "vp", t0)
        cloud.failUploads = true
        do { _ = try await sync(); XCTFail("should throw") } catch {}
        XCTAssertEqual(readLocal("a.sav"), "vp")
        cloud.failUploads = false
        let r = try await sync()
        XCTAssertEqual(r.uploaded, [name("a.sav")])
    }

    func testSubfoldersAndNoTempFilesLeft() async throws {
        cloud.files[name("profiles/p1/a.sav")] = (Data("deep".utf8), t0)
        _ = try await sync()
        XCTAssertEqual(readLocal("profiles/p1/a.sav"), "deep")
        let all = FileManager.default.enumerator(atPath: root.path)?.allObjects as? [String] ?? []
        XCTAssertFalse(all.contains { $0.contains("cloud-tmp") })
    }

    // MARK: Play right after a sync

    func testCloudSavesAreInPlaceBeforeAnythingGoesUp() async throws {
        cloud.files[name("s1/a.sav")] = (Data("pc-a".utf8), t0)
        cloud.files[name("s1/b.sav")] = (Data("pc-b".utf8), t0)
        try writeLocal("s0/mine.sav", "vp", t0)
        final class Seen: @unchecked Sendable { var steps: [CloudSync.Progress] = []; var inPlace: [Bool] = [] }
        let seen = Seen()
        let r = try await makeSync().run { p in
            seen.steps.append(p)
            if p.stage == .uploading && p.done == 0 { seen.inPlace.append(self.readLocal("s1/a.sav") != nil && self.readLocal("s1/b.sav") != nil) }
        }
        XCTAssertEqual(seen.steps, [
            .init(stage: .checking),
            .init(stage: .downloading, done: 0, total: 2), .init(stage: .downloading, done: 1, total: 2),
            .init(stage: .downloading, done: 2, total: 2),
            .init(stage: .uploading, done: 0, total: 1), .init(stage: .uploading, done: 1, total: 1),
        ])
        XCTAssertEqual(seen.inPlace, [true], "every download is in place once uploading starts")
        XCTAssertEqual(r.downloaded.count, 2)
        XCTAssertEqual(r.uploaded, [name("s0/mine.sav")])
    }

    func testQuickCheckSeesOnlyWhatTheCloudChanged() async throws {
        cloud.files[name("s0/a.sav")] = (Data("pc".utf8), t0)
        cloud.files[name("gone.sav")] = (Data("old".utf8), t0)
        cloud.files["%GameInstall%game/hlvr/cfg/other.cfg"] = (Data("not a save".utf8), t0)
        try writeLocal("s1/mine.sav", "vp", t0)
        let first = try await cloudChanged()
        XCTAssertTrue(first, "nothing synced yet")
        _ = try await sync()
        let afterSync = try await cloudChanged()
        XCTAssertFalse(afterSync, "the sync just ran, its upload included")
        try FileManager.default.removeItem(at: local("gone.sav"))   // the game deleted it
        try writeLocal("s1/mine.sav", "vp2", t0)                     // and saved again
        let headsetOnly = try await cloudChanged()
        XCTAssertFalse(headsetOnly, "the headset's own changes wait for the next sync")
        cloud.files[name("s0/a.sav")] = (Data("pc2".utf8), t0.addingTimeInterval(60))
        let changed = try await cloudChanged()
        XCTAssertTrue(changed, "a newer PC save")
        _ = try await sync()
        cloud.files[name("s2/new.sav")] = (Data("pc".utf8), t0)
        let added = try await cloudChanged()
        XCTAssertTrue(added, "a new PC save")
    }

    func testCancelledSyncStopsBetweenFilesAndFinishesNextTime() async throws {
        for f in ["s0/a.sav", "s0/b.sav", "s0/c.sav"] { cloud.files[name(f)] = (Data(f.utf8), t0) }
        try writeLocal("s1/mine.sav", "vp", t0)
        cloud.beforeDownload = { f in if f.filename == self.name("s0/b.sav") { withUnsafeCurrentTask { $0?.cancel() } } }
        // Its own task, as the launcher's: cancelling it leaves the test's own task running.
        let cancelled = Task { try await self.sync() }
        do { _ = try await cancelled.value; XCTFail("should stop") } catch is CancellationError {}
        XCTAssertEqual(readLocal("s0/a.sav"), "s0/a.sav")
        XCTAssertNil(readLocal("s0/c.sav"), "stopped before the next file")
        XCTAssertNil(cloud.files[name("s1/mine.sav")], "nothing went up")
        let all = FileManager.default.enumerator(atPath: root.path)?.allObjects as? [String] ?? []
        XCTAssertFalse(all.contains { $0.contains("cloud-tmp") })
        cloud.beforeDownload = nil
        let r = try await sync()
        XCTAssertEqual(r.downloaded, [name("s0/b.sav"), name("s0/c.sav")], "a.sav was recorded")
        XCTAssertEqual(r.uploaded, [name("s1/mine.sav")])
    }
}
