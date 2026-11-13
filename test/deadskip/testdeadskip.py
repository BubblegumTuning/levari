#!/usr/bin/env python3
"""deadskip gate: a playlist [live, DEAD, live] with shuffle OFF must play
THROUGH the dead row (state stays playing); an all-dead playlist must
still stop. Run against a build dir: python3 deadskip.py [build-dir]"""
import os, subprocess, socket, time, tempfile, shutil, sys, wave, struct, math

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
binp = os.path.join(repo, BUILD, "levari")
assert os.path.exists(binp), binp

def wav(path, secs):
    with wave.open(path, "w") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(b"".join(struct.pack("<h", int(4000*math.sin(i/8)))
                               for i in range(8000*secs)))

S = tempfile.mkdtemp(prefix="deadskip-", dir=os.path.expanduser("~/.hermes/cache/scratch"))
cfg = os.path.join(S, ".config", "levari"); os.makedirs(cfg)
sock = os.path.join(S, "lv.sock")
open(os.path.join(cfg, "config"), "w").write(
    f"audio.backend = null\ndaemon.socket = {sock}\n")
live1 = os.path.join(S, "live1.wav"); live2 = os.path.join(S, "live2.wav")
dead = os.path.join(S, "NO-SUCH-FILE.wav")
wav(live1, 1); wav(live2, 1)
env = dict(os.environ, HOME=S, XDG_CONFIG_HOME=os.path.join(S, ".config"),
           XDG_RUNTIME_DIR=S, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

def status_line():
    for _ in range(40):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(2)
            s.sendall(b"status\n"); line = s.recv(4096).decode().strip(); s.close()
            return line
        except OSError: time.sleep(0.2)
    return "<timeout>"

fails = 0
def run(files, secs, label, lines=None):
    global fails
    proc = subprocess.Popen([binp] + files, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # wait for the socket to appear before polling
    st = status_line()
    if st == "<timeout>":
        print(label, "FAIL no socket"); fails += 1; proc.kill(); return
    states = []
    t0 = time.time()
    while time.time() - t0 < secs:
        line = status_line()
        if lines is not None: lines.append(line)
        states.append(line.split()[0])
        time.sleep(0.4)
    proc.terminate()
    try: proc.wait(3)
    except subprocess.TimeoutExpired: proc.kill()
    print(label, "->", " ".join(dict.fromkeys(states)))
    return states

# case 1: dead row in the middle: playback must REACH live2 past the dead
# row (a sequential no-repeat list legitimately stops at its real end;
# the pre-fix bug stops at the DEAD row instead, never playing live2)
st1 = run([live1, dead, live2], 5, "mid-dead ", lines=(lines1 := []))
ok1 = any(l.split()[0] == "playing" and "live2.wav" in l for l in lines1)
if not ok1: print("FAIL: never played past the dead row"); fails += 1
else: print("PASS: played through the dead row")

# case 2: all-dead playlist: must stop (bounded walk, no spin)
lines2 = []
st2 = run([dead, dead + "2"], 4, "all-dead ", lines=lines2)
if not st2 or "playing" in st2: print("FAIL: never reached stop"); fails += 1
else: print("PASS: stopped with an all-dead playlist")

shutil.rmtree(S, ignore_errors=True)
sys.exit(1 if fails else 0)
