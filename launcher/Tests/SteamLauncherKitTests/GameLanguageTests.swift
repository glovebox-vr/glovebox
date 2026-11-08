import XCTest
@testable import SteamLauncherKit

final class GameLanguageTests: XCTestCase {
    func testSystemTagsMapToSteamNames() {
        let cases: [(String, String?)] = [
            ("zh-Hans-CN", "schinese"), ("zh-Hans", "schinese"), ("zh-CN", "schinese"), ("zh", "schinese"),
            ("zh-Hant-TW", "tchinese"), ("zh-Hant-HK", "tchinese"), ("zh-TW", "tchinese"), ("zh-HK", "tchinese"),
            ("yue-Hant-HK", "tchinese"),
            ("en-US", "english"), ("en-GB", "english"), ("ja-JP", "japanese"), ("ko-KR", "koreana"),
            ("es-ES", "spanish"), ("es", "spanish"), ("es-419", "latam"), ("es-MX", "latam"),
            ("pt-BR", "brazilian"), ("pt-PT", "portuguese"), ("nb-NO", "norwegian"), ("uk-UA", "ukrainian"),
            ("de-DE", "german"), ("fr-CA", "french"), ("he-IL", nil),
        ]
        for (tag, want) in cases {
            XCTAssertEqual(GameLanguage.steamName(forLanguageTag: tag), want, tag)
        }
    }

    func testAutomaticTakesTheFirstSystemLanguageTheGameHas() {
        XCTAssertEqual(GameLanguage.resolve("auto", preferred: ["zh-Hans-CN", "en-US"]).id, "schinese")
        // Hebrew and Arabic have no Alyx text, so the next language on the list wins
        XCTAssertEqual(GameLanguage.resolve("auto", preferred: ["he-IL", "ar-SA", "ja-JP"]).id, "japanese")
        XCTAssertEqual(GameLanguage.resolve("auto", preferred: ["he-IL"]).id, "english")
        XCTAssertEqual(GameLanguage.resolve("auto", preferred: []).id, "english")
    }

    func testAPickedLanguageWinsOverTheSystem() {
        XCTAssertEqual(GameLanguage.resolve("english", preferred: ["zh-Hans-CN"]).id, "english")
        XCTAssertEqual(GameLanguage.resolve("tchinese", preferred: ["en-US"]).id, "tchinese")
        // a stored name the game doesn't have (an old build's, a typo) falls back to Automatic
        XCTAssertEqual(GameLanguage.resolve("klingon", preferred: ["ko-KR"]).id, "koreana")
    }

    /// Every language offered is one kl_app.c passes on (its k_steam_languages), and each is offered once.
    func testOfferedLanguagesAreSteamNames() {
        let steam: Set<String> = ["arabic", "brazilian", "bulgarian", "czech", "danish", "dutch", "english", "finnish",
                                  "french", "german", "greek", "hungarian", "indonesian", "italian", "japanese",
                                  "koreana", "latam", "norwegian", "polish", "portuguese", "romanian", "russian",
                                  "schinese", "spanish", "swedish", "tchinese", "thai", "turkish", "ukrainian",
                                  "vietnamese"]
        let ids = GameLanguage.alyx.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count)
        XCTAssertEqual(ids.count, 28)  // the store page's list
        for id in ids { XCTAssertTrue(steam.contains(id), id) }
    }
}
