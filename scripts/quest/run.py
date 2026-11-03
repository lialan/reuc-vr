#!/usr/bin/env python3
"""Run the Quest app on the headset for a diagnostic session (gse-recomp's quest/run_vulkan_xr.py for this game).

    python3 -I scripts/quest/run.py [--apk PATH] [--seconds N | --interactive] [--arg=--opt ...] [--set NAME=VALUE ...] [--out DIR]

Installs the APK, stages the game into the app's private folder files/.codex-xr-<id> (once: 3.8 GB; kept and reused while its main.dol
matches), writes env.txt/args.txt there, starts the app with the private-game extras (the app refuses any other folder for a diagnostic
run), streams logcat, then pulls uc.log, logs/ and a screen capture into --out. Run it under quest3-lock (the headset is shared):
the caller's wrapper retries while the lock is busy (exit 75).
"""
import argparse, hashlib, json, pathlib, shlex, subprocess, sys, time, uuid

SERIAL = "2G97C5ZJ0G02DQ"
PACKAGE = "org.uc.umbrellachronicles.vr"
ACTIVITY = "org.uc.umbrellachronicles.UmbrellaChroniclesVR"
ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--apk", default=str(ROOT / "quest/reuc/build/outputs/apk/debug/reuc-debug.apk"))
    p.add_argument("--game", default=str(pathlib.Path.home() / "workspace/reuc-install/game"))
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--interactive", action="store_true", help="run until <out>/STOP exists")
    p.add_argument("--arg", action="append", default=[], help="a command-line option for the game (repeatable)")
    p.add_argument("--set", action="append", default=[], help="NAME=VALUE environment option (repeatable)")
    p.add_argument("--out", default=None)
    p.add_argument("--file", action="append", default=[], help="LOCAL=NAME: copy a file into the private folder (e.g. a demo)")
    p.add_argument("--saves", default=None, help="a saves directory to copy into the run's saves/ (replacing it) before the start")
    p.add_argument("--no-install", action="store_true")
    p.add_argument("--guardian-pause", action="store_true", help="pause Guardian for scripted runs (restored after)")
    args = p.parse_args()
    out = pathlib.Path(args.out or ROOT / "build-android/quest-runs" / time.strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)
    report = {"args": vars(args)}

    def command(*argv, check=True, timeout=60, **kw):
        result = subprocess.run(["adb", "-s", SERIAL, *map(str, argv)], capture_output=True, timeout=timeout, **kw)
        if check and result.returncode:
            raise RuntimeError(f"adb {' '.join(map(str, argv))}: {result.stderr.decode(errors='replace')}")
        return result

    def shell(*argv, **kw):
        return command("shell", shlex.join(map(str, argv)), **kw)

    def save(name, result):
        (out / name).write_bytes(result.stdout + result.stderr)
        return result

    stream = logfile = None
    launched = proximity = guardian = False
    try:
        if not args.no_install:
            save("install.txt", command("install", "-r", "-g", args.apk, timeout=300))
        home = shell("run-as", PACKAGE, "pwd").stdout.decode().strip()
        idfile = ROOT / "build-android/quest-private-id"
        if not idfile.exists():
            idfile.write_text(uuid.uuid4().hex)
        private = f"{home}/files/.codex-xr-{idfile.read_text().strip()}"
        dol = pathlib.Path(args.game) / "DATA/sys/main.dol"
        want = hashlib.sha256(dol.read_bytes()).hexdigest()
        have = shell("run-as", PACKAGE, "sha256sum", private + "/game/DATA/sys/main.dol", check=False).stdout.decode().split()
        if not have or have[0] != want or shell("run-as", PACKAGE, "test", "-f", private + "/game/.complete", check=False).returncode:
            print("staging the game into the app's private folder (once)...", flush=True)
            shell("run-as", PACKAGE, "rm", "-rf", private)
            shell("run-as", PACKAGE, "mkdir", "-p", private)
            game = pathlib.Path(args.game)
            transfer = subprocess.Popen(["tar", "-C", str(game.parent), "-cf", "-", game.name], stdout=subprocess.PIPE)
            subprocess.run(["adb", "-s", SERIAL, "shell", f"run-as {PACKAGE} tar -C {shlex.quote(private)} -xf -"],
                           stdin=transfer.stdout, check=True, timeout=1800)
            transfer.wait()
            if game.name != "game":
                shell("run-as", PACKAGE, "mv", f"{private}/{game.name}", f"{private}/game")
            shell("run-as", PACKAGE, "touch", private + "/game/.complete")
        for name, lines in (("env.txt", args.set), ("args.txt", args.arg)):
            subprocess.run(["adb", "-s", SERIAL, "shell", f"run-as {PACKAGE} sh -c " + shlex.quote(f"cat > {shlex.quote(private + '/' + name)}")],
                           input=("\n".join(lines) + "\n").encode(), check=True, capture_output=True, timeout=30)
        for spec in args.file:   # LOCAL=NAME: a file the run reads (a recorded demo), into the private folder
            local, name = spec.split("=", 1)
            subprocess.run(["adb", "-s", SERIAL, "shell", f"run-as {PACKAGE} sh -c " + shlex.quote(f"cat > {shlex.quote(private + '/' + name)}")],
                           input=pathlib.Path(local).read_bytes(), check=True, capture_output=True, timeout=120)
        if args.saves:   # a save snapshot (the harness's pinned saves): the run's saves/ starts as an exact copy of it every time
            shell("run-as", PACKAGE, "rm", "-rf", private + "/saves")
            shell("run-as", PACKAGE, "mkdir", "-p", private + "/saves")
            transfer = subprocess.Popen(["tar", "-C", args.saves, "-cf", "-", "."], stdout=subprocess.PIPE)
            subprocess.run(["adb", "-s", SERIAL, "shell", f"run-as {PACKAGE} tar -C {shlex.quote(private + '/saves')} -xf -"],
                           stdin=transfer.stdout, check=True, timeout=120)
            transfer.wait()
        shell("run-as", PACKAGE, "rm", "-f", private + "/uc.log")
        shell("run-as", PACKAGE, "rm", "-rf", private + "/captures")   # this run's captures only (they piled up to GBs and the pull failed)
        if args.guardian_pause:
            guardian = True; shell("setprop", "debug.oculus.guardian_pause", "1")
        token = uuid.uuid4().hex
        logfile = (out / "logcat-all.txt").open("wb")
        stream = subprocess.Popen(["adb", "-s", SERIAL, "logcat", "-b", "all", "-v", "threadtime", "-T", "1"], stdout=logfile, stderr=subprocess.STDOUT)
        for _ in range(20):
            shell("log", "-t", "UcXRSession", "START " + token)
            time.sleep(.2)
            if "START " + token in (out / "logcat-all.txt").read_text(errors="replace"):
                break
        else:
            raise RuntimeError("fresh logcat marker was not observed; refusing untracked launch")
        proximity = True; save("proximity-close.txt", shell("am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close"))
        shell("input", "keyevent", "KEYCODE_WAKEUP")
        launched = True
        save("launch.txt", shell("am", "start", "-n", f"{PACKAGE}/{ACTIVITY}", "--ez", "uc.require_private_game", "true",
                                 "--es", "uc.private_game", private))
        start = time.monotonic(); pid = ""
        stop = out / "STOP"
        if args.interactive:
            print("interactive session running; create", stop, "to end it", flush=True)
        while args.interactive or time.monotonic() - start < args.seconds:
            if args.interactive and stop.exists():
                break
            current = shell("pidof", PACKAGE, check=False).stdout.decode().strip()
            if current and not pid:
                pid = current.split()[0]; report["pid"] = pid
            if pid and not current:
                report["early_process_exit"] = True
                break
            time.sleep(1)
        report["seconds"] = round(time.monotonic() - start, 1)
        with (out / "screen.png").open("wb") as picture:
            subprocess.run(["adb", "-s", SERIAL, "exec-out", "screencap", "-p"], stdout=picture, timeout=20)
        save("exit-info.txt", shell("dumpsys", "activity", "exit-info", PACKAGE, check=False))
    finally:
        if launched:
            command("shell", "am force-stop " + PACKAGE, check=False)
        if proximity:
            command("shell", "am broadcast -a com.oculus.vrpowermanager.automation_disable", check=False)
        if guardian:
            command("shell", "setprop debug.oculus.guardian_pause 0", check=False)
        if stream:
            stream.terminate()
            try:
                stream.wait(timeout=10)
            except subprocess.TimeoutExpired:
                stream.kill()
            logfile.close()
        try:
            result = command("exec-out", f"run-as {PACKAGE} cat {private}/uc.log", check=False)
            (out / "uc.log").write_bytes(result.stdout)
            tar = command("exec-out", f"run-as {PACKAGE} tar -C {private} -cf - logs", check=False, timeout=120)
            (out / "logs.tar").write_bytes(tar.stdout)
            caps = command("exec-out", f"run-as {PACKAGE} sh -c 'cd {private} && [ -d captures ] && tar -cf - captures'", check=False, timeout=300)
            if caps.stdout:   # frame captures the run asked for (--capture-dir captures)
                (out / "captures.tar").write_bytes(caps.stdout)
        except Exception as error:   # noqa: BLE001 - best effort after a failed run
            report["pull_error"] = repr(error)
        (out / "report.json").write_text(json.dumps(report, indent=2))
        print("run folder:", out, flush=True)


if __name__ == "__main__":
    sys.exit(main())
