#!/usr/bin/env python3
"""testseekabort — an interrupted SPC seek must not strand the readout.

Transport-level pin for the aborted-seek restamp (player CMD_SEEK +
dec_pos): start playback of a 3600s-tag SPC, post seek 3599 (absolute
seconds — an ~40 s emulated fast-forward), pause mid-skip (the abort
trigger), and read the daemon status position. The pause does NOT
redefine transport state, so before the fix pos_frames stayed at the
pre-seek value while the audio sat ~100 declared s into the skip.
Restamped, pos reports where the skip actually got to.

Run against a build dir: python3 testseekabort.py [build-dir]
"""
import os, socket, subprocess, sys, tempfile, time

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
BIN = os.path.join(repo, BUILD, "levari")
assert os.path.exists(BIN), BIN

S = tempfile.mkdtemp(prefix="seekabort-", dir=os.path.expanduser("~/.hermes/cache/scratch"))
SOCK = os.path.join(S, "levari.sock")

def mk_spc(path, secs):
    # format-floor SPC (0x10180): LE16 length at 0xA9 via the non-digit
    # fallback, silent all-zero DSP, liveness body = JMP $8000 at 0x8000
    # (SPC700 table), never at RAM 0 (the echo engine with dir=0 zeroes
    # low RAM ~170 ms in) — same shape as test/spcseek/testspcseek.c
    f = bytearray(0x10180)
    f[0:27] = b"SNES-SPC700 Sound File Data\x1a\x1a"
    f[0x23], f[0x24] = 0, 0
    f[0xA9], f[0xAA] = secs & 0xFF, (secs >> 8) & 0xFF
    f[0x25], f[0x26] = 0x00, 0x80
    o = 0x100 + 0x8000
    f[o:o+3] = b"\x5f\x00\x80"
    with open(path, "wb") as fp:
        fp.write(f)

def cmd(line, timeout=5.0):
    # the socket answers ONE command per connection and closes
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall((line + "\n").encode())
    buf = b""
    try:
        while True:
            c = s.recv(4096)
            if not c:
                break
            buf += c
    except socket.timeout:
        pass
    s.close()
    return buf.decode(errors="replace").strip()

def hhmmss(t):
    t = t.split("/")[0]      # status carries pos=CUR/TOTAL
    p = [int(x) for x in t.split(":")]
    while len(p) < 3:        # lv_fmt_time: "SS", "M:SS" and "H:MM:SS" shapes
        p = [0] + p
    return p[0]*3600 + p[1]*60 + p[2]

spc = os.path.join(S, "spc.spc")
mk_spc(spc, 3600)
cfgd = os.path.join(S, ".config", "levari")
os.makedirs(cfgd)
with open(os.path.join(cfgd, "config"), "w") as fp:
    fp.write("audio.backend = null\ndaemon.socket = %s\n" % SOCK)
os.makedirs(os.path.join(S, "home"))

env = dict(os.environ, HOME=os.path.join(S, "home"),
           XDG_CONFIG_HOME=os.path.join(S, ".config"),
           XDG_RUNTIME_DIR=S, SDL_VIDEODRIVER="dummy")
proc = subprocess.Popen([BIN, spc], env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
rc = 1
try:
    for _ in range(100):
        if os.path.exists(SOCK):
            break
        time.sleep(0.1)
    else:
        print("FAIL no daemon socket"); sys.exit(1)

    # wait for playback to start and the position readout to exist
    st = ""
    deadline = time.time() + 10
    while time.time() < deadline:
        st = cmd("status")
        if st.startswith("playing"):
            break
        time.sleep(0.2)
    print("SETTLE %r" % st)
    assert st.startswith("playing"), "playback never started: %r" % st
    pre = hhmmss(st.split()[2][4:])

    cmd("seek 3599")         # absolute seconds (daemon contract) — an
                             # ~40 s emulated fast-forward begins
    time.sleep(1.5)          # ~100+ declared seconds should be skipped
    cmd("pause")             # aborts the skip between chunks
    time.sleep(0.5)
    st = cmd("status")
    print("STATUS %r" % st)
    assert st.startswith("paused"), "pause did not take: %r" % st
    pos = hhmmss(st.split()[2][4:])

    # skip window ~1.2-1.5 s wall at ~12 ms CPU per declared s => >=~100 s;
    # the pre-fix readout stayed at the pre-seek value (<= a few s).
    # Wide margins: fixed pos >> 30 s >= any real skip point, buggy pos < 10 s.
    if pos > 30 and pos - pre > 30:
        print("PASS pos restamped to the skip point (%d s >= pre-seek %d s)" % (pos, pre))
        rc = 0
    elif pos < 10:
        print("FAIL pos stayed at the pre-seek value (%d s): readout stranded" % pos)
    else:
        print("FAIL ambiguous window (pos %d s): timing slip?" % pos)
except Exception as e:
    print("FAIL %s" % e)
finally:
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()

sys.exit(rc)
