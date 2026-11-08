import Foundation

/// The language the game runs in. On a PC that's Steam's setting for the game (its Properties › Language, or Steam's
/// own language when nothing was picked), which the game reads from the Steam client. The headset has no Steam client,
/// so the launcher keeps the player's choice and hands the game Steam's name for it at Play (Alyx takes it as
/// `-language`: KL_ALYX_LANGUAGE, Klepton's kl_app.c). Automatic follows the headset's languages, the way Steam
/// follows its own.
public struct GameLanguage: Identifiable, Hashable, Sendable {
    /// Steam's API name for the language ("schinese"), which is what the game is given; "auto" for `automatic`.
    public let id: String
    /// The name players know it by: Steam's own, written in that language.
    public let name: String

    public init(id: String, name: String) {
        self.id = id
        self.name = name
    }

    public static let automatic = GameLanguage(id: "auto", name: "Automatic")
    public static let english = GameLanguage(id: "english", name: "English")

    /// Half-Life: Alyx's languages, from its Steam store page (2026-11), in Steam's order: text and subtitles in all of
    /// them, voice only in English.
    public static let alyx: [GameLanguage] = [
        english,
        GameLanguage(id: "schinese", name: "简体中文"),
        GameLanguage(id: "tchinese", name: "繁體中文"),
        GameLanguage(id: "japanese", name: "日本語"),
        GameLanguage(id: "koreana", name: "한국어"),
        GameLanguage(id: "thai", name: "ไทย"),
        GameLanguage(id: "bulgarian", name: "Български"),
        GameLanguage(id: "czech", name: "Čeština"),
        GameLanguage(id: "danish", name: "Dansk"),
        GameLanguage(id: "german", name: "Deutsch"),
        GameLanguage(id: "spanish", name: "Español - España"),
        GameLanguage(id: "latam", name: "Español - Latinoamérica"),
        GameLanguage(id: "greek", name: "Ελληνικά"),
        GameLanguage(id: "french", name: "Français"),
        GameLanguage(id: "italian", name: "Italiano"),
        GameLanguage(id: "hungarian", name: "Magyar"),
        GameLanguage(id: "dutch", name: "Nederlands"),
        GameLanguage(id: "norwegian", name: "Norsk"),
        GameLanguage(id: "polish", name: "Polski"),
        GameLanguage(id: "portuguese", name: "Português - Portugal"),
        GameLanguage(id: "brazilian", name: "Português - Brasil"),
        GameLanguage(id: "romanian", name: "Română"),
        GameLanguage(id: "russian", name: "Русский"),
        GameLanguage(id: "finnish", name: "Suomi"),
        GameLanguage(id: "swedish", name: "Svenska"),
        GameLanguage(id: "turkish", name: "Türkçe"),
        GameLanguage(id: "vietnamese", name: "Tiếng Việt"),
        GameLanguage(id: "ukrainian", name: "Українська"),
    ]

    /// Steam's name for a language tag as the system lists them (`Locale.preferredLanguages`: "zh-Hans-CN", "pt-BR",
    /// "es-419"), or nil when Steam has no language for it.
    public static func steamName(forLanguageTag tag: String) -> String? {
        let locale = Locale(identifier: tag)
        guard let code = locale.language.languageCode?.identifier.lowercased() else { return nil }
        let script = locale.language.script?.identifier
        let region = locale.region?.identifier.uppercased()
        switch code {
        case "zh":
            if script == "Hant" { return "tchinese" }
            if script == "Hans" { return "schinese" }
            return ["TW", "HK", "MO"].contains(region ?? "") ? "tchinese" : "schinese"
        case "yue": return "tchinese"
        case "en": return "english"
        case "ja": return "japanese"
        case "ko": return "koreana"
        case "th": return "thai"
        case "bg": return "bulgarian"
        case "cs": return "czech"
        case "da": return "danish"
        case "de": return "german"
        // Steam's Spanish is Spain's; every other region's is Latin America's
        case "es": return region == nil || region == "ES" ? "spanish" : "latam"
        case "el": return "greek"
        case "fr": return "french"
        case "it": return "italian"
        case "hu": return "hungarian"
        case "nl": return "dutch"
        case "nb", "nn", "no": return "norwegian"
        case "pl": return "polish"
        case "pt": return region == "BR" ? "brazilian" : "portuguese"
        case "ro": return "romanian"
        case "ru": return "russian"
        case "fi": return "finnish"
        case "sv": return "swedish"
        case "tr": return "turkish"
        case "vi": return "vietnamese"
        case "uk": return "ukrainian"
        case "ar": return "arabic"
        case "id": return "indonesian"
        default: return nil
        }
    }

    /// The language a choice runs the game in: the one picked, or for Automatic (and anything not in `available`) the
    /// first of the system's languages the game has, else English.
    public static func resolve(_ choice: String, preferred: [String] = Locale.preferredLanguages,
                               available: [GameLanguage] = alyx) -> GameLanguage {
        if choice != automatic.id, let picked = available.first(where: { $0.id == choice }) { return picked }
        for tag in preferred {
            if let n = steamName(forLanguageTag: tag), let l = available.first(where: { $0.id == n }) { return l }
        }
        return english
    }

    /// Where the player's choice is kept (UserDefaults): a Steam name, or "auto", the default.
    public static let defaultsKey = "gameLanguage"

    public static var choice: String {
        get { UserDefaults.standard.string(forKey: defaultsKey) ?? automatic.id }
        set { UserDefaults.standard.set(newValue, forKey: defaultsKey) }
    }
}
