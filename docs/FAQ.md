# FAQ

### Do I need a paid developer account?

No. A free Apple Account works: sign in to Xcode with it and Glovebox builds with its Personal Team. The game needs about 6 to 8 GB of memory, and Apple gives free teams the Increased Memory Limit capability that allows it. The difference is how long an install lasts: 7 days with a free account, a year with the paid Developer Program ($99 a year). See "Does the app expire?".

### Why not TestFlight or the App Store?

visionOS only runs code that is signed into the app. A downloadable Glovebox would have to contain Valve's game code, and we won't distribute that without Valve's permission. Building it yourself means the game's code goes only from your Steam account to your own devices.

### Is this piracy?

No. You need Half-Life: Alyx on your own Steam account. Steam itself refuses to hand out the game's download keys to an account that doesn't own it. Glovebox never hosts or shares game files.

### Do I need a gaming PC, or a PC streaming app?

No. The game runs on the headset's own chip. The Mac is only needed to build and install the app.

### Which controllers work?

PS VR2 Sense controllers. Half-Life: Alyx needs buttons, sticks and triggers, so hand tracking alone isn't supported.

### How long does installing take?

On the Mac: about 2 minutes to build, plus downloading about 1.7 GB of game files, and installing Xcode if you don't have it. On the headset: the game download, about 67 GB, which depends on your internet connection.

### How much space does it need?

About 75 GB on the headset. On the Mac: Xcode with the visionOS platform, plus a few GB of working space.

### Will my saves carry over?

Yes. Glovebox syncs your Steam Cloud saves when you sign in and before you play, the same way the Steam client does. Before replacing anything, it keeps a backup. A save you make on the headset goes up to Steam the next time you open the game there, because quitting the game closes the app.

### Does the app expire?

Yes. An install stops opening after 7 days with a free Apple Account, or after a year with a paid developer account. Glovebox on your Mac shows **Renew** two days before a free install runs out (a month before a paid one). Renewing builds and installs it again and keeps your game files and saves.

### The headset says the developer isn't trusted

With a free Apple Account, trust it once after the first install: on the headset, open Settings › General › VPN & Device Management, tap your Apple Account, then Trust. A renewal keeps it trusted.

### What happens when Valve updates the game?

Glovebox installs the game version it was tested with, so a Steam update can't break your install. When we've tested a new version, Glovebox offers it as an update.

### Why does macOS ask for my Mac's password?

Once, before the first build, if your Xcode came from the App Store: Xcode made your signing key when you signed in, and macOS asks before another tool may sign with it. Type your Mac's password and click **Always Allow**; it won't ask again. The password goes to macOS, never to Glovebox.

### Is my Steam password safe?

You type it into Glovebox's own sign-in screen. It's sent only to Steam, encrypted with Steam's key, and never stored. Glovebox keeps only Steam's sign-in token, in your Keychain. All of the code is in this repository.

### How do I play in another language?

Open Glovebox: HLA on the headset, tap your name at the top right and choose **Game Language**. **Automatic**, the
default, uses your Vision Pro's language when the game has it, the way Steam uses its own. The game takes the new
language the next time you press Play. Half-Life: Alyx has its text and subtitles in 28 languages and its voice in
English only, so the voices stay English. Turn on subtitles in the game's options to read them.

### Can Glovebox run other games or mods?

Not yet. It's built so that more can be added later.

### Something went wrong. How do I report it?

In the Mac app, press **Copy diagnostics**, then [open an issue](https://github.com/glovebox-vr/glovebox/issues/new) and paste it in. The diagnostics contain logs and your setup, but no passwords or game files.

If the problem is in the game, open Glovebox: HLA on the headset, tap your name at the top right and choose **Share Diagnostics**. Send the file to yourself (AirDrop or Mail) and attach it to the issue. It holds the game's logs and your setup, but not your Steam sign-in or your saves.
