#!/usr/bin/env python3
"""Run the firmware in Hatari with an emulated Multi-device (EmuMD), headless,
scripted and logged, one folder per run.

Usage: hatari_run.py [--machine st|ste|megast|megaste] [--memsize MB] [--sound]
                     [--expect FILE] [--parse FILE] [--name NAME]
                     [--trace FLAGS] [--realtime] [--step-time S]
                     [--firmware FILE.mdfw] [--build] [--sd DIR]

The firmware is the host build EmuMD makes of rp/src (emu/README.md):
build/md-chorensha.mdfw by default, built first with --build. Paths come from
tools/re/re.cfg (see re.cfg.example): EmuMD's Hatari (HATARI), the TOS images
(TOS_STE, 1.62, for every machine unless TOS_ST is set for the ST and Mega ST
and TOS_MSTE, 2.06, for the Mega STE, which TOS 1.62 does not boot), and
RUNS, where run folders go: outside the repository, since a run's card and
dumps may hold the game (default tools/re/logs, gitignored). Each run gets
<RUNS>/<date>-<machine>[-name]/:

  command.txt  the Hatari command line
  hatari.out   everything Hatari printed: its log, the firmware's (DPRINTF,
               unless --quiet-firmware), traces and debugger output
  shots/       a screenshot per `shot` step, on failure, and at the end
  sd/          the emulated microSD card: empty, or a copy of --sd
  result.txt   PASS or FAIL, the steps reached, the PC at the end

The expectations file has one step per line ('#' starts a comment):

  expect <name> <regex>   a line of Hatari's output matching <regex>, within
                          --step-time seconds
  quiet <name> <seconds>  no new output for <seconds>
  key <key>               press and release a key: space, return, esc, f1-f10,
                          0-9, up/down/left/right, or an ST scan code (0x39)
  hold <key> <seconds>    hold a key down
  fire [seconds]          joystick 1's fire: Hatari's right mouse button, which
                          it hands to joystick 1 while the game has the mouse
                          off (the same wire on a real ST); default 0.2 s
  mouse <dx> <dy>         move the mouse by dx, dy ST pixels
  click [seconds]         the left mouse button; default 0.2 s
  debug <command>         a Hatari debugger command; {run} is the run folder
  stage <file>            copy a debugger command file from tools/re/scripts/
                          into the run folder, {run} in it replaced by the run
                          folder's path, so `:file {run}/<file>` runs it and the
                          relative paths it saves to land in the run folder
  reset warm|cold         reset the ST: warm keeps the Multi-device powered (the
                          RP keeps its state), cold powers it off and on
  shot <name>             a screenshot, shots/<name>.png
  sleep <seconds>
  record <name>           start recording the sound to <name>.wav in the run
                          folder (needs --sound); `record` with no name stops

Key events reach the ST's keyboard (the IKBD), not the joystick: Hatari's
keyboard joystick reads host keys, which a headless run has none of. Fire
goes through the right mouse button (above).

Without --expect the run waits 30 s and takes a screenshot. --parse hands
Hatari a debugger script at start (breakpoints with :file command files);
the script and the files it names are copied into the run folder and run
from there, so relative paths they save to land in the run folder. Hatari's
own debugger is documented in its debugger.html. Standard library only.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
MACHINES = {"st": ("st", "TOS_ST"), "megast": ("megast", "TOS_ST"),
            "ste": ("ste", "TOS_STE"), "megaste": ("megaste", "TOS_MSTE")}
MDFW = REPO / "emu" / "emumd" / "tools" / "mdfw"
KEYS = {"space": 0x39, "return": 0x1C, "esc": 0x01, "backspace": 0x0E, "tab": 0x0F,
        "up": 0x48, "down": 0x50, "left": 0x4B, "right": 0x4D, "help": 0x62, "undo": 0x61}
KEYS.update({f"f{n}": 0x3A + n for n in range(1, 11)})
KEYS.update({str(n): 0x01 + n for n in range(1, 10)})
KEYS["0"] = 0x0B
DEFAULT_STEPS = [("sleep", "30"), ("shot", "screen")]
PC_LINE = re.compile(r"^Next PC: [0-9a-fA-F]+")
FILE_OPT = re.compile(r":file\s+(\S+)")


def load_cfg() -> dict[str, str]:
    path = HERE / "re.cfg"
    if not path.is_file():
        sys.exit(f"error: no {path}: copy re.cfg.example and fill it in")
    cfg = {}
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            key, _, value = line.partition("=")
            cfg[key.strip()] = os.path.expanduser(value.strip())
    return cfg


class Output:
    """Hatari's output, read from a pseudo-terminal line by line as it comes."""

    def __init__(self, fd: int, path: Path):
        self.lines: list[str] = []
        self.cursor = 0
        self.lock = threading.Lock()
        self.file = open(path, "w")
        threading.Thread(target=self._read, args=(fd,), daemon=True).start()

    def _read(self, fd: int) -> None:
        buf = b""
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("latin-1").rstrip("\r")
                with self.lock:
                    self.lines.append(text)
                self.file.write(text + "\n")
                self.file.flush()

    def wait_for(self, regex: re.Pattern, timeout: float) -> str | None:
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                while self.cursor < len(self.lines):
                    line = self.lines[self.cursor]
                    self.cursor += 1
                    if regex.search(line):
                        return line
            time.sleep(0.05)
        return None

    def wait_quiet(self, seconds: float, timeout: float) -> bool:
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                n = len(self.lines)
            time.sleep(seconds)
            with self.lock:
                if len(self.lines) == n:
                    return True
        return False


def keycode(key: str) -> int:
    if key.lower() in KEYS:
        return KEYS[key.lower()]
    try:
        return int(key, 0)
    except ValueError:
        sys.exit(f"error: unknown key {key!r}")


def parse_steps(path: Path) -> list[tuple[str, ...]]:
    steps = []
    for n, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kind, _, rest = line.partition(" ")
        rest = rest.strip()
        if kind == "expect":
            name, _, regex = rest.partition(" ")
            re.compile(regex.strip())
            steps.append((kind, name, regex.strip()))
        elif kind == "quiet":
            name, _, secs = rest.partition(" ")
            steps.append((kind, name, str(float(secs))))
        elif kind == "hold":
            what, _, secs = rest.rpartition(" ")
            float(secs)
            keycode(what)
            steps.append((kind, what, secs))
        elif kind == "fire":
            steps.append((kind, str(float(rest or 0.2))))
        elif kind == "record":
            steps.append((kind, rest))
        elif kind == "click":
            steps.append((kind, str(float(rest or 0.2))))
        elif kind == "reset":
            if rest not in ("warm", "cold"):
                sys.exit(f"error: {path}:{n}: reset warm or reset cold")
            steps.append((kind, rest))
        elif kind == "mouse":
            dx, dy = rest.split()
            steps.append((kind, str(int(dx)), str(int(dy))))
        elif kind in ("key", "shot", "sleep", "debug", "stage"):
            if kind == "key":
                keycode(rest)
            if kind == "sleep":
                float(rest)
            steps.append((kind, rest))
        else:
            sys.exit(f"error: {path}:{n}: unknown step {kind!r}")
    return steps


def stage_debugger_script(script: Path, run_dir: Path) -> str:
    """Copy a debugger script and the :file command files it names (in turn)
    into the run folder, each pointing at the copies."""
    text = script.read_text()
    for name in set(FILE_OPT.findall(text)):
        src = Path(name) if Path(name).is_absolute() else script.parent / name
        if not src.is_file():
            sys.exit(f"error: {script}: no such :file {name}")
        text = text.replace(f":file {name}", f":file {stage_debugger_script(src, run_dir)}")
    staged = run_dir / script.name
    staged.write_text(text)
    return str(staged)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--machine", default="ste", choices=sorted(MACHINES))
    ap.add_argument("--memsize", type=int, default=1, help="ST RAM in MB; 0 is 512 KB")
    ap.add_argument("--sound", action="store_true", help="emulate sound output (to a dummy audio driver)")
    ap.add_argument("--expect", type=Path, help="expectations file")
    ap.add_argument("--parse", type=Path, help="Hatari debugger script, run at start")
    ap.add_argument("--name", default="", help="suffix for the run folder")
    ap.add_argument("--trace", default="", help="Hatari trace flags (see hatari --trace help)")
    ap.add_argument("--realtime", action="store_true", help="no fast-forward")
    ap.add_argument("--step-time", type=float, default=60, help="seconds a step may take")
    ap.add_argument("--key-hold", type=float, default=0.1, help="host seconds after a key step")
    ap.add_argument("--firmware", type=Path, default=REPO / "build" / "md-chorensha.mdfw",
                    help="the .mdfw to run (default build/md-chorensha.mdfw)")
    ap.add_argument("--build", action="store_true", help="build the .mdfw first (mdfw build)")
    ap.add_argument("--sd", type=Path, help="a folder copied as the run's microSD card")
    ap.add_argument("--quiet-firmware", action="store_true", help="leave out the firmware's log")
    ap.add_argument("--log-root", type=Path, help="where run folders go (default RUNS of re.cfg)")
    args = ap.parse_args()

    cfg = load_cfg()
    machine, tos_key = MACHINES[args.machine]
    hatari = cfg.get("HATARI", "hatari")
    # TOS 1.62 everywhere unless the ST's own is given.
    tos = cfg.get(tos_key) or cfg.get("TOS_STE", "")
    if args.build:
        # The bench scene's synthetic assets, which the RP's CMake build makes
        # itself; EmuMD includes build/generated/scene (mdfw.ini).
        subprocess.run([sys.executable, str(REPO / "tools" / "gen_scene.py"), "--out",
                        str(REPO / "build" / "generated" / "scene" / "bench_scene_assets.h")],
                       check=True)
        subprocess.run([str(MDFW), "build"], cwd=REPO, check=True)
    for p in (hatari, tos, args.firmware):
        if not Path(p).is_file():
            sys.exit(f"error: not found: {p} (tools/re/re.cfg or the command line)")
    steps = parse_steps(args.expect) if args.expect else DEFAULT_STEPS

    stamp = time.strftime("%Y%m%d-%H%M%S")
    label = args.machine + (f"-{args.name}" if args.name else "")
    log_root = args.log_root or Path(cfg.get("RUNS") or HERE / "logs")
    run_dir = (log_root / f"{stamp}-{label}").resolve()
    for sub in ("shots", "home"):
        (run_dir / sub).mkdir(parents=True)
    sd = run_dir / "sd"
    if args.sd:
        shutil.copytree(args.sd, sd)
    else:
        sd.mkdir()
    # No GEMDOS drive, so the firmware's cartridge boots (emu/README.md); the
    # borders shown, as the app opens them.
    cmd = [hatari, "--machine", machine, "--tos", tos, "--memsize", str(args.memsize),
           "--monitor", "rgb", "--sound", "44100" if args.sound else "off", "--confirm-quit", "false",
           "--fast-forward", "no" if args.realtime else "yes", "--fast-boot", "on",
           "--statusbar", "false", "--drive-led", "false", "--borders", "on",
           "--harddrive", "none", "--md-sd", str(sd),
           "--md-verbose", "off" if args.quiet_firmware else "on",
           "--screenshot-dir", str(run_dir / "shots")]
    if args.trace:
        cmd += ["--trace", args.trace]
    if args.parse:
        cmd += ["--parse", stage_debugger_script(args.parse, run_dir)]
    cmd += ["--md-firmware", str(Path(args.firmware).resolve())]

    # macOS caps a Unix socket path at 104 bytes; keep it short.
    sock_dir = tempfile.mkdtemp(prefix="crh", dir="/tmp")
    sock_path = os.path.join(sock_dir, "ctl.sock")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(1)
    server.settimeout(20)
    cmd += ["--control-socket", sock_path]

    # A private HOME, or Hatari loads the user's saved configuration; headless.
    env = dict(os.environ, HOME=str(run_dir / "home"), SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    (run_dir / "command.txt").write_text(" ".join(f"'{c}'" if " " in c else c for c in cmd) + "\n")
    master, slave = os.openpty()
    start = time.time()
    proc = subprocess.Popen(cmd, env=env, cwd=run_dir, stdin=subprocess.DEVNULL, stdout=slave, stderr=slave)
    os.close(slave)
    out = Output(master, run_dir / "hatari.out")
    print(f"{label}: {run_dir}", flush=True)

    reached: list[str] = []
    failure = None
    conn = None
    pc_line = None

    def say(text: str) -> None:
        print(f"  [{time.time() - start:6.1f}s] {text}", flush=True)

    def command(text: str) -> None:
        conn.sendall((text + "\n").encode())

    def shot(name: str) -> Path | None:
        shots = run_dir / "shots"
        before = set(os.listdir(shots))
        time.sleep(0.3)  # fast-forward skips frames: let one render
        command("hatari-shortcut screenshot")
        end = time.time() + 5
        while time.time() < end:
            new = [f for f in os.listdir(shots) if f not in before]
            if new:
                time.sleep(0.2)
                path = shots / f"{name}.png"
                os.replace(shots / new[0], path)
                return path
            time.sleep(0.05)
        return None

    try:
        conn, _ = server.accept()
        for step in steps:
            kind = step[0]
            if proc.poll() is not None:
                failure = f"Hatari exited ({proc.returncode}) before step {' '.join(step)}"
                break
            if kind == "expect":
                _, name, regex = step
                line = out.wait_for(re.compile(regex), args.step_time)
                if line is None:
                    failure = f"step {name}: no line matching {regex!r} within {args.step_time:g} s"
                    break
                reached.append(name)
                say(f"{name}: {line.strip()[:110]}")
            elif kind == "quiet":
                _, name, secs = step
                if not out.wait_quiet(float(secs), args.step_time):
                    failure = f"step {name}: output never stopped for {secs} s"
                    break
                reached.append(name)
                say(f"{name}: quiet for {float(secs):g} s")
            elif kind == "key":
                # Pressed and released together: in fast-forward a host-timed
                # hold is seconds of the ST's time, and the firmware's ESC exit
                # wants the release within 200 ms.
                code = f"0x{keycode(step[1]):02x}"  # always hex: "1" would type a 1
                command(f"hatari-event keypress {code}")
                time.sleep(args.key_hold)
                say(f"key {step[1]}")
            elif kind == "hold":
                code = f"0x{keycode(step[1]):02x}"
                command(f"hatari-event keydown {code}")
                time.sleep(float(step[2]))
                command(f"hatari-event keyup {code}")
                say(f"hold {step[1]} {step[2]} s")
            elif kind == "fire":
                command("hatari-event rightdown")
                time.sleep(float(step[1]))
                command("hatari-event rightup")
                say(f"fire {step[1]} s")
            elif kind == "click":
                command("hatari-event leftdown")
                time.sleep(float(step[1]))
                command("hatari-event leftup")
                say(f"click {step[1]} s")
            elif kind == "reset":
                command(f"hatari-shortcut {step[1]}reset")
                say(f"{step[1]} reset")
            elif kind == "mouse":
                command(f"hatari-event mousemove {step[1]} {step[2]}")
                say(f"mouse {step[1]} {step[2]}")
            elif kind == "debug":
                text = step[1].replace("{run}", str(run_dir))
                command(f"hatari-debug {text}")
                say(f"debug {text}")
            elif kind == "stage":
                src = HERE / "scripts" / step[1]
                (run_dir / src.name).write_text(src.read_text().replace("{run}", str(run_dir)))
                say(f"staged {src.name}")
            elif kind == "shot":
                path = shot(step[1])
                say(f"screenshot {path.name if path else '(none)'}")
            elif kind == "sleep":
                time.sleep(float(step[1]))
            elif kind == "record":
                if step[1]:
                    wav = run_dir / f"{step[1]}.wav"
                    command(f"hatari-path soundout {wav}")
                    command("hatari-shortcut recsound")
                    say(f"recording {wav.name}")
                else:
                    command("hatari-shortcut recsound")
                    say("recording stopped")
    except (OSError, socket.timeout) as err:
        failure = f"control socket: {err}"
    finally:
        if conn is not None:
            try:
                if failure:
                    shot("fail")
                shot("end")
                out.cursor = len(out.lines)
                command("hatari-debug r")
                pc_line = out.wait_for(PC_LINE, 5)
                command("hatari-shortcut quit")
            except OSError:
                pass
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(5)
        server.close()
        shutil.rmtree(sock_dir, ignore_errors=True)
        time.sleep(0.3)

    if proc.poll() is None:
        failure = (failure + "; " if failure else "") + "Hatari still running"
    wall = time.time() - start
    expected = [s[1] for s in steps if s[0] in ("expect", "quiet")]
    verdict = "FAIL" if failure else "PASS"
    summary = (f"{verdict} {label}: {len(reached)}/{len(expected)} steps"
               + (f", last {reached[-1]}" if reached else "") + f", {wall:.1f} s")
    (run_dir / "result.txt").write_text(
        summary + "\n" + (f"failure: {failure}\n" if failure else "")
        + "".join(f"reached: {r}\n" for r in reached)
        + (f"at the end: {pc_line.strip()}\n" if pc_line else ""))
    print(summary)
    if failure:
        print(f"  {failure}")
    return 1 if failure else 0


if __name__ == "__main__":
    sys.exit(main())
