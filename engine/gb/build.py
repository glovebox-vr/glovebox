"""The player-side build: from the player's own game files to a signed app on their headset.

Steps, each safe to re-run (finished work is kept and checked, not redone):
  kit        the prebuilt open-source runtime (engine/kit/build-kit.sh) into the Klepton tree
  runtime    Debian's arm64 C++ runtime and libraries, fetched by content hash, into <work>/sysroot
  pipelines  the pipeline archive rebuilt from the package's recipe and the game's shader files
  defaults   the settings the app starts with (<work>/alyx-demo)
  translate  every game library through klepton-ld, relinked by Apple's linker
  app        the Xcode project for the player's team, built and signed
  install    onto the paired headset
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
K = os.path.join(ROOT, "klepton")
V = os.path.join(K, "visionos")
GUEST_DIR = "build/glovebox-guest"  # relative to klepton/visionos, where gen_xcodeproj.py resolves it


class StepError(Exception):
    pass


def package_dir(package):
    return os.path.join(ROOT, "packages", package)


def glovebox_version():
    p = os.path.join(ROOT, "VERSION")
    if os.path.exists(p):
        return open(p).read().strip()
    r = subprocess.run(["git", "-C", ROOT, "describe", "--tags", "--always"], capture_output=True, text=True)
    return r.stdout.strip() or "dev"


def default_work(package):
    return os.environ.get("GLOVEBOX_WORK") or os.path.expanduser(f"~/Library/Application Support/Glovebox/{package}")


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _download(url, dest):
    tmp = dest + ".part"
    try:
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
    except Exception:  # a Python without CA certificates: curl uses the system's
        subprocess.run(["curl", "-fsSL", "--retry", "3", "-o", tmp, url], check=True)
    os.replace(tmp, dest)


def _run(cmd, emit, step, cwd=None, env=None, log=None, quiet_re=None):
    """Run a command, streaming its lines as progress events (and into log); raise StepError on failure."""
    e = dict(os.environ)
    e.update(env or {})
    lf = open(log, "w") if log else None
    p = subprocess.Popen(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail = []
    for line in p.stdout:
        if lf:
            lf.write(line)
        line = line.rstrip()
        tail = (tail + [line])[-30:]
        if line and not (quiet_re and re.search(quiet_re, line)):
            emit("progress", step=step, line=line[:300])
    rc = p.wait()
    if lf:
        lf.close()
    if rc != 0:
        raise StepError(f"{os.path.basename(cmd[0])} failed ({rc}): " + " | ".join(l for l in tail[-6:] if l))


# ---- kit ----

def find_kit():
    """The runtime kit: GLOVEBOX_KIT, else one bundled beside the engine (the Glovebox app ships it as kit/ or
    glovebox-kit.zip at the source root), else None."""
    if os.environ.get("GLOVEBOX_KIT"):
        return os.environ["GLOVEBOX_KIT"]
    for c in ("kit", "glovebox-kit", "glovebox-kit.zip"):
        p = os.path.join(ROOT, c)
        if os.path.exists(os.path.join(p, "KIT.json")) or (c.endswith(".zip") and os.path.exists(p)):
            return p
    return None


def kit(kit_path, work, emit):
    kit_path = kit_path or find_kit()
    if not kit_path:
        raise StepError("no runtime kit: set GLOVEBOX_KIT to a kit folder or .zip (engine/kit/build-kit.sh makes one)")
    src = kit_path
    if kit_path.endswith(".zip"):
        src = os.path.join(work, "kit")
        stamp = os.path.join(src, ".from")
        if not (os.path.exists(stamp) and open(stamp).read() == _sha256(kit_path)):
            shutil.rmtree(src, ignore_errors=True)
            tmp = tempfile.mkdtemp(dir=work)
            subprocess.run(["ditto", "-x", "-k", kit_path, tmp], check=True)
            inner = [d for d in os.listdir(tmp) if os.path.isdir(os.path.join(tmp, d))]
            os.replace(os.path.join(tmp, inner[0]), src)
            shutil.rmtree(tmp)
            open(stamp, "w").write(_sha256(kit_path))
    meta = json.load(open(os.path.join(src, "KIT.json")))
    shutil.copyfile(os.path.join(src, "KIT.json"), os.path.join(work, "kit.json"))
    os.makedirs(os.path.join(K, "build"), exist_ok=True)
    os.makedirs(os.path.join(V, "Frameworks"), exist_ok=True)
    places = [("bin/klepton-ld", os.path.join(K, "build", "klepton-ld")),
              ("Klepton.xcframework", os.path.join(K, "build", "Klepton.xcframework"))]
    places += [(f"Frameworks/{f}", os.path.join(V, "Frameworks", f)) for f in os.listdir(os.path.join(src, "Frameworks"))]
    for rel, dst in places:
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        elif os.path.exists(dst):
            os.unlink(dst)
        subprocess.run(["ditto", os.path.join(src, rel), dst], check=True)
    emit("step", step="kit", status="ok", detail=f"runtime kit {meta['kit']} (MoltenVK {meta['moltenvk']})")


# ---- runtime ----

def runtime(package, work, emit):
    spec_path = os.path.join(package_dir(package), "runtime-debs.json")
    spec = json.load(open(spec_path))
    sysroot = os.path.join(work, "sysroot")
    stamp = os.path.join(sysroot, ".spec")
    want = _sha256(spec_path)
    if os.path.exists(stamp) and open(stamp).read() == want and all(
            os.path.exists(os.path.join(sysroot, f)) for f in spec["files"]):
        emit("step", step="runtime", status="ok", detail=f"{len(spec['files'])} runtime libraries (cached)")
        return
    cache = os.path.join(work, "cache", "debs")
    os.makedirs(cache, exist_ok=True)
    x = tempfile.mkdtemp(dir=work)
    try:
        for i, pkg in enumerate(spec["packages"]):
            deb = os.path.join(cache, pkg["file"])
            if not (os.path.exists(deb) and _sha256(deb) == pkg["sha256"]):
                emit("progress", step="runtime", line=f"downloading {pkg['file']}", done=i, total=len(spec["packages"]))
                _download(pkg["url"], deb)
                if _sha256(deb) != pkg["sha256"]:
                    os.unlink(deb)
                    raise StepError(f"{pkg['file']}: downloaded file does not match its SHA-256")
            d = tempfile.mkdtemp(dir=x)
            subprocess.run(["ar", "-x", deb], cwd=d, check=True)
            data = [f for f in os.listdir(d) if f.startswith("data.tar")]
            subprocess.run(["tar", "-xf", data[0]], cwd=d, check=True)
        found = {}
        for dirpath, _dirs, files in os.walk(x):
            for f in files + _dirs:
                if f in spec["files"] and f not in found:
                    found[f] = os.path.realpath(os.path.join(dirpath, f))
        missing = sorted(set(spec["files"]) - set(found))
        if missing:
            raise StepError(f"runtime packages lack {', '.join(missing)}")
        os.makedirs(sysroot, exist_ok=True)
        for f, src in found.items():
            shutil.copyfile(src, os.path.join(sysroot, f))
        open(stamp, "w").write(want)
    finally:
        shutil.rmtree(x, ignore_errors=True)
    emit("step", step="runtime", status="ok", detail=f"{len(spec['files'])} runtime libraries")


# ---- game files ----

def game(work, game_src, emit):
    """<work>/linux/game: the depot's game/ folder (Steam download, or GLOVEBOX_GAME for a copy already on disk)."""
    g = os.path.join(work, "linux", "game")
    if game_src:
        os.makedirs(os.path.dirname(g), exist_ok=True)
        if os.path.islink(g) or os.path.exists(g):
            if os.path.realpath(g) != os.path.realpath(game_src):
                if os.path.islink(g):
                    os.unlink(g)
                else:
                    raise StepError(f"{g} exists and is not the game copy given")
        if not os.path.exists(g):
            os.symlink(os.path.realpath(game_src), g)
    need = ["bin/linuxsteamrtarm64", "hlvr/bin/linuxsteamrtarm64", "core/shaders_vulkan_dir.vpk",
            "hlvr/shaders_vulkan_dir.vpk"]
    missing = [n for n in need if not os.path.exists(os.path.join(g, n))]
    if missing:
        raise StepError(f"game files missing in {g}: {', '.join(missing)}")
    emit("step", step="game", status="ok", detail=g)
    return g


# ---- pipelines ----

def pipelines(package, work, game_dir, emit):
    recipes = sorted(f for f in os.listdir(os.path.join(package_dir(package), "pipelines")) if f.endswith(".recipe"))
    if len(recipes) != 1:
        raise StepError(f"expected one recipe in packages/{package}/pipelines, found {recipes}")
    out = os.path.join(work, "pipelines.foz")
    _run([sys.executable, os.path.join(ROOT, "engine", "pipelines.py"), "build",
          os.path.join(package_dir(package), "pipelines", recipes[0]), game_dir, out], emit, "pipelines")
    emit("step", step="pipelines", status="ok", detail=f"{os.path.getsize(out) >> 20} MB pipeline list")
    return out


# ---- defaults ----

def defaults(package, work, foz_path, emit):
    """<work>/alyx-demo, what the app falls back on when Documents has no copy (Klepton's kl_alyx_app.c)."""
    pd = package_dir(package)
    man = json.load(open(os.path.join(pd, "package.json")))["defaults"]
    tex = str(man["texture_max"])
    # the game's texture streaming pool, fixed (MB), optional: without it the pool is gameinfo's 2816 MB (Steam Frame),
    # which cap 1024 never fills. Above 1024 the pool stays full in chapter 1, which is fine as long as the streamer
    # only drops what is least needed (the floor below) and what is in view fits: cap 2048 does in 2560 MB, cap 4096
    # needs ~3.3 GB, more than the headset has (Mac, 2026-10-12). 1536 MB since 2026-11-08: Chapters 6 to 8 reach the
    # app's 8192 MB limit, where the headset ends the game without a report. The footprint climbs about 1 GB over a
    # level's first 15-20 minutes and tops out near 6 GB plus the pool: runs at 2560 ended with 128 and 108 MB left, and one
    # at 2048 reached 8.0-8.1 GB twice and ended with 80 MB left, a jump of 325 MB in 2 s the minute before. The [hb] line's
    # gfx= says how much of that is the GPU's
    pool = int(man["texture_pool_mb"]) if man.get("texture_pool_mb") else 0
    # the lowest the streamer may scale every texture request when the pool is full: the game's own 0.8 (the Frame's,
    # and PC's Low/Medium) softens every texture and sharpens it again each time the pool fills, which reads as
    # pop-in; 1.0 (PC High/Ultra) leaves it to drop the least needed mips (Mac, chapter 1 crawl at cap 4096 and
    # 3328 MB: 16.4 pop-ins a minute against 22.9 at 0.8; cap 1024: 15.7)
    bias_min = float(man["texture_bias_min"]) if man.get("texture_bias_min") else 0.0
    demo = os.path.join(work, "alyx-demo")
    shutil.rmtree(demo, ignore_errors=True)
    os.makedirs(demo)
    args_extra = f" +r_texture_stream_max_resolution {tex}"
    if pool:
        args_extra += f" +r_texture_pool_size {pool} +r_texture_budget_dynamic 0"
    if bias_min:
        args_extra += f" +r_texture_stream_resolution_bias_min {bias_min:.1f}"
    with open(os.path.join(demo, "alyx-env.txt"), "w") as f:
        f.write("# The app's bundled defaults (Glovebox). @demo/ = this folder, @docs/ = Documents.\n")
        for line in open(os.path.join(pd, man["env"])):
            if line.startswith("#"):
                continue
            if line.startswith("KL_ALYX_PROFILE_ARGS="):
                line = line.rstrip("\n") + args_extra + "\n"
            f.write(line)
        # hand tracking is not compiled in (KLEPTON_NO_HANDS); the Sense controllers are the input
        f.write("KL_HANDS=0\n")
        # sysinfo's total RAM: the headset's 16 GB, so the game's minimum-memory notice stays away
        f.write("KL_MEM_TOTAL_MB=16384\n")
        # a new player's movement: continuous, hand-oriented
        f.write("KL_ALYX_NEWPLAYER_ARGS=+hlvr_movetype_default 3\n")
        f.write("KL_VK_FOZ_REPLAY=@demo/demo.foz:@docs/klfz\nKL_VK_FOZ_REPLAY_BLOCK=1\nKL_VK_FOZ_THREADS=6\n"
                "KL_VK_FOZ_RECORD=auto\n"
                # the game's own Vulkan device settings (Documents/klfz/device-info.json), for warming the shader cache
                # before the game starts
                "KL_VK_DEVICE_INFO=auto\n")
        # the player's answer to "share stutter reports?" from the Mac app (unset: the headset launcher asks)
        share = os.environ.get("GLOVEBOX_SHARE_STUTTERS", "")
        if share in ("0", "1"):
            # when it was answered (the Mac app passes it; else the build time): a newer answer given on the headset
            # wins over it, and this one wins over an older headset answer
            at = os.environ.get("GLOVEBOX_SHARE_STUTTERS_AT", "")
            at = at if re.fullmatch(r"\d{1,12}(\.\d+)?", at) else str(int(time.time()))
            f.write(f"GLOVEBOX_SHARE_STUTTERS={share}\nGLOVEBOX_SHARE_STUTTERS_AT={at}\n")
    with open(os.path.join(demo, "alyx-convars.lua"), "w") as f:
        f.write(open(os.path.join(pd, man["convars"])).read())
        f.write(f'''
-- Texture cap ({tex}): set at the first spawn if the game's saved video settings changed it.
ListenToGameEvent("player_activate", function()
  if tostring(Convars:GetStr("r_texture_stream_max_resolution")) ~= "{tex}" then
    SendToConsole("r_texture_stream_max_resolution {tex}"); Msg("KLTEXMAX set {tex}\\n")
  end
end, nil)
''')
        if bias_min:
            f.write(f'''
-- Texture request floor ({bias_min:.1f}): the menu's Textures Low/Medium set 0.5/0.8, which brings back the pop-in.
ListenToGameEvent("player_activate", function()
  if (tonumber(Convars:GetStr("r_texture_stream_resolution_bias_min")) or 0) < {bias_min:.1f} then
    SendToConsole("r_texture_stream_resolution_bias_min {bias_min:.1f}"); Msg("KLTEXBIAS set {bias_min:.1f}\\n")
  end
end, nil)
''')
    shutil.copyfile(os.path.join(pd, man["fidelity"]), os.path.join(demo, "vr_fidelity_levels_override.csv"))
    video = open(os.path.join(pd, man["video"])).read()
    video = re.sub(r'("setting.r_texture_stream_max_resolution"\s+")\d+"', r'\g<1>' + tex + '"', video)
    if bias_min:
        video = re.sub(r'("setting.r_texture_stream_resolution_bias_min"\s+")[\d.]+"', r'\g<1>' + f"{bias_min:.6f}" + '"',
                       video)
    open(os.path.join(demo, "video2.txt"), "w").write(video)
    shutil.copyfile(foz_path, os.path.join(demo, "demo.foz"))
    # how the game makes its Vulkan device, for "Preparing graphics" before Play (kl_app_prewarm) on a first launch;
    # the headset's own record (Documents/klfz/device-info.json) wins from the second
    if man.get("device_info"):
        shutil.copyfile(os.path.join(pd, man["device_info"]), os.path.join(demo, "device-info.json"))
    # what the headset app's stutter reports need (diagnostics/): versions and endpoint. The player's answer travels in
    # alyx-env.txt (GLOVEBOX_SHARE_STUTTERS, above)
    kit_meta = os.path.join(work, "kit.json")
    info = {"glovebox": glovebox_version(),
            "kit": json.load(open(kit_meta))["kit"] if os.path.exists(kit_meta) else "dev",
            "endpoint": "https://glovebox-reports.vercel.app/api/report"}
    json.dump(info, open(os.path.join(demo, "glovebox.json"), "w"), indent=1)
    share = {"1": "yes", "0": "no"}.get(os.environ.get("GLOVEBOX_SHARE_STUTTERS", ""), "ask on the headset")
    tex_txt = (f"texture cap {tex}" + (f", texture pool {pool} MB" if pool else "")
               + (f", floor {bias_min:.1f}" if bias_min else ""))
    emit("step", step="defaults", status="ok", detail=f"settings ({tex_txt}, stutter reports: {share})")
    return demo


# ---- translate ----

def translate(work, emit):
    env = {"ALYX_DEPOT": work, "KLEPTON_LDWRAP": "1", "KLEPTON_GUEST_OUT": GUEST_DIR, "KLEPTON_GUEST_ONLY": "1",
           "XROS_PLATFORMS": "xros"}
    _run(["bash", "mkguest_alyx.sh"], emit, "translate", cwd=V, env=env, log=os.path.join(work, "translate.log"))
    n = len(open(os.path.join(V, GUEST_DIR, "libs.txt")).read().split())
    emit("step", step="translate", status="ok", detail=f"{n} game libraries translated")


# ---- app ----

def bundle_id_for(team):
    return f"com.glovebox.alyx.t{team.lower()}"


def signing_identities(team, find_certs=None, valid_ids=None):
    """The SHA-1s of the "Apple Development" identities for <team> in the keychain, newest first. Reads public
    certificates only (no private key, so no prompt). find_certs/valid_ids: the two `security` outputs, for tests."""
    if find_certs is None:
        find_certs = subprocess.run(["security", "find-certificate", "-a", "-c", "Apple Development", "-Z", "-p"],
                                    capture_output=True, text=True).stdout
    if valid_ids is None:
        valid_ids = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"],
                                   capture_output=True, text=True).stdout
    valid = set(re.findall(r"\b([0-9A-F]{40})\b", valid_ids))
    found = []
    for sha, pem in re.findall(r"SHA-1 hash: ([0-9A-F]{40}).*?(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)",
                               find_certs, re.S):
        if sha not in valid:
            continue  # no private key here, or expired
        r = subprocess.run(["openssl", "x509", "-noout", "-subject", "-startdate", "-nameopt", "RFC2253"],
                           input=pem, capture_output=True, text=True).stdout
        ou = re.search(r"OU=([A-Z0-9]{10})", r)
        nb = re.search(r"notBefore=(.*)", r)
        if ou and ou.group(1) == team:
            found.append((time.mktime(time.strptime(nb.group(1).strip(), "%b %d %H:%M:%S %Y %Z")) if nb else 0, sha))
    return [sha for _, sha in sorted(found, reverse=True)]


def warm_up_signing(team, work, emit):
    """One signature, alone, before xcodebuild's parallel ones. A signing key made by the App Store copy of the Xcode
    app (signed by its App Store team, so the key's partition list lacks "apple:") makes macOS ask before codesign may
    use it, and xcodebuild signs ~94 things at once: each waiting codesign shows its own password window (36 at once
    on 2026-10-07). Signing one scratch file first turns that into at most one window, whose Always Allow covers the
    build. A key that allows codesign already signs here in well under a second. No key yet: xcodebuild makes one in
    this build, and a key made by xcodebuild ("anchor apple", so "apple:") never asks. See docs/SIGNING-PROMPTS.md."""
    ids = signing_identities(team)
    if not ids:
        emit("progress", step="app", line=f"no signing key for team {team} on this Mac yet; Xcode makes one now")
        return
    d = os.path.join(work, "warmup")
    os.makedirs(d, exist_ok=True)
    # a plain file, not a program: a universal program gets one signature per architecture, and a plain Allow
    # (not Always Allow) covers only one of them, so the second would ask again
    f = os.path.join(d, "signing-check")
    with open(f, "w") as out:
        out.write("Glovebox\n")
    emit("progress", step="app", line="checking that this Mac may sign with your key (macOS may ask once: "
                                      "type your Mac password and click Always Allow)")
    try:
        r = subprocess.run(["codesign", "--force", "--sign", ids[0], "--timestamp=none", f],
                           capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        raise StepError("macOS is waiting for permission to use your signing key: answer its window with your Mac "
                        "password and Always Allow, then try again")
    if r.returncode != 0:
        raise StepError("macOS did not let this Mac sign with your key (" + (r.stderr.strip().splitlines() or ["?"])[-1]
                        + "): try again and click Always Allow")


def app(package, work, demo, team, emit, provisioning=True, unsigned=False, device=None, free_team=False):
    pd = package_dir(package)
    man = json.load(open(os.path.join(pd, "package.json")))
    env = {"KLEPTON_TEAM": team, "KLEPTON_TARGET": "alyx", "KLEPTON_ALYX_DEMO": demo,
           # a free Personal Team can have the memory limit but not Extended Virtual Addressing (paid only)
           "KLEPTON_EVA": "0" if free_team else "1",
           "KLEPTON_ALYX_LAUNCHER": os.path.join(ROOT, "launcher"), "KLEPTON_DISPLAY_NAME": man.get("app_name") or man["game"],
           "KLEPTON_INFOPLIST_SET": os.path.join(pd, man["defaults"]["purpose_strings"]), "KLEPTON_NO_HANDS": "1",
           "KLEPTON_GUEST_DIR": GUEST_DIR, "KLEPTON_BUNDLE_ID": bundle_id_for(team),
           "KLEPTON_APPICON_DIR": os.environ.get("GLOVEBOX_ICON") or (os.path.join(pd, man["icon"]) if man.get("icon") else "")}
    _run([sys.executable, "gen_xcodeproj.py"], emit, "app", cwd=V, env=env, log=os.path.join(work, "project.log"))
    dd = os.path.join(work, "dd")
    dest = f"platform=visionOS,id={device}" if device else "generic/platform=visionOS"
    cmd = ["xcodebuild", "-project", "KleptonAlyx.xcodeproj", "-scheme", "KleptonAlyx", "-configuration", "Release",
           "-destination", dest, "-derivedDataPath", dd]
    if unsigned:
        cmd += ["CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "CODE_SIGN_IDENTITY="]
    elif provisioning:
        warm_up_signing(team, work, emit)
        cmd.append("-allowProvisioningUpdates")
        if device:
            cmd.append("-allowProvisioningDeviceRegistration")  # a tester's headset joins their team's devices
    cmd.append("build")
    quiet = (r"^\s|^(Ld|CompileC|Copy|Write|Process|Sign|Create|Touch|Validate|Gather|Register|Generate|Compile|"
             r"SwiftDriver|Emit|Link|Run|Ext)")
    try:
        try:
            _run(cmd, emit, "app", cwd=V, log=os.path.join(work, "xcodebuild.log"), quiet_re=quiet)
        except StepError:
            log = open(os.path.join(work, "xcodebuild.log")).read()
            if not (device and re.search(r"Unable to find a destination|is not available|Unable to find a device", log)):
                raise
            # the headset is asleep or out of reach: sign generically (installs where the team already knows it)
            emit("progress", step="app", line="headset not reachable; building without registering it")
            cmd = [("generic/platform=visionOS" if c == dest else c) for c in cmd if c != "-allowProvisioningDeviceRegistration"]
            _run(cmd, emit, "app", cwd=V, log=os.path.join(work, "xcodebuild.log"), quiet_re=quiet)
    except StepError:
        log = open(os.path.join(work, "xcodebuild.log")).read()
        errs = sorted(set(re.findall(r"error: (.{0,200})", log)))[:5]
        raise StepError("the app did not build: " + (" | ".join(errs) or "see xcodebuild.log"))
    built = os.path.join(dd, "Build", "Products", "Release-xros", "KleptonAlyx.app")
    out = os.path.join(work, "KleptonAlyx.app")
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run(["ditto", built, out], check=True)
    emit("step", step="app", status="ok",
         detail="built, unsigned" if unsigned else f"signed for team {team} as {bundle_id_for(team)}")
    return out


def install(app_path, device_id, emit, pause=5):
    # A headset that has just gone to sleep (or just woken) can drop the connection mid-install ("Connection with the
    # remote side was unexpectedly closed"); a try or two later it goes through. Anything else fails right away.
    for attempt in range(3):
        try:
            _run(["xcrun", "devicectl", "device", "install", "app", "--device", device_id, app_path], emit, "install")
            break
        except StepError as e:
            if attempt == 2 or "unexpectedly closed" not in str(e).lower():
                raise
            emit("progress", step="install", line="The Vision Pro dropped the connection; trying again")
            time.sleep(pause)
    emit("step", step="install", status="ok", detail="installed on the headset")


# ---- collect ----

# The runtime's log of this run and of the one before (a freeze's [stall] dump survives the relaunch there), and
# its crash report, beside the game's own console.
COLLECT = ["Documents/klfz/recorded.foz", "Documents/klfz/slow.txt", "Documents/alyx/assets/hlvr/console.log",
           "Documents/klepton-boot.log", "Documents/klepton-boot.prev.log", "Documents/klepton-crash.log"]


def collect(package, work, device_id, team, game_dir, emit):
    """Copy the headset app's pipeline recording, hitch log, game console and runtime logs to <work>/feedback/<time>/, and turn the
    recording into a recipe (no shader code) that can be shared."""
    import time
    out = os.path.join(work, "feedback", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    got = []
    for src in COLLECT:
        dst = os.path.join(out, os.path.basename(src))
        r = subprocess.run(["xcrun", "devicectl", "device", "copy", "from", "--device", device_id,
                            "--domain-type", "appDataContainer", "--domain-identifier", bundle_id_for(team),
                            "--source", src, "--destination", dst], capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(dst):
            got.append(os.path.basename(src))
    if "recorded.foz" in got and game_dir:
        _run([sys.executable, os.path.join(ROOT, "engine", "pipelines.py"), "make", os.path.join(out, "recorded.recipe"),
              game_dir, os.path.join(out, "recorded.foz"), "--allow-missing"], emit, "collect")
        got.append("recorded.recipe")
    emit("step", step="collect", status="ok" if got else "missing", detail=f"{', '.join(got) or 'nothing'} in {out}")
    return out
