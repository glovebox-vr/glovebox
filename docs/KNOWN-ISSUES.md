# Known issues

| What you see | When | What to do |
|---|---|---|
| Close objects look doubled at the edges when you move your head | Moving your head side to side, most on things within arm's reach | Nothing yet. The game draws 45 frames a second and the headset shows each one twice, turned to follow your head's rotation but not its movement. |
| A few busy scenes run below full speed | Some areas with lots of glass and effects | Nothing yet. |
| The game uses your PC's settings, such as teleport movement | After your first sync | Your settings come down from Steam Cloud. Change them in the game's options on the headset. |
| Left-handed or one-controller mode doesn't work | Choosing it in the game's options | Use the default setup: two controllers, right-handed. |
| A glass window won't break: a shot leaves no hole, and things you throw go straight through it | Chapter 6 (Arms Race), the storeroom window | Throw or push any object, such as a chair, through the glass, then walk through right away: for a second or two after, it lets you through. Glovebox also checks the window 3 seconds after something has gone through it, and removes it if it is solid again. Then press **Share Diagnostics** and attach the file to an issue: it records what the window did. |
| A save from the headset isn't on your PC yet | Right after you quit on the headset | Saves go up the next time you open Glovebox: HLA, because quitting the game closes the app. Open it once more. |
| Steam's QR code doesn't sign you in | Your account has no Steam Guard Mobile Authenticator, or you scanned with the Camera app | Scan from inside the Steam app, or choose **Sign In with Password**. |
| Glovebox asks you to sign in to Xcode again | Apple ended Xcode's saved sign-in | In Xcode › Settings › Apple Accounts, sign in, then press **Try Again**. |
| The install stops at Apple's new terms | Apple updated its developer agreement | Accept it at [developer.apple.com/account](https://developer.apple.com/account) (on a company team, only the account holder can), then press **Try Again**. |
| macOS asks for your Mac's password | Before the first build, if Xcode came from the App Store | Type it and click **Always Allow**. It asks once. |
| The game stops opening | 7 days after installing with a free Apple Account | Open Glovebox on your Mac and press **Renew**. It shows two days before. |
| The game downloads again | After switching to another Apple Account or team | Each team gets its own copy of the app on the headset, so stay with one. |
| The download slows down | With the headset off | It carries on in the background, more slowly. Open Glovebox: HLA again to finish. |

Something not listed? Press **Copy diagnostics** in the Mac app (or, for a problem in the game, **Share Diagnostics** in the headset app's menu under your name) and [open an issue](https://github.com/glovebox-vr/glovebox/issues/new).
