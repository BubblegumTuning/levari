#!/usr/bin/env python3
"""Whole-line deadline guard for the daemon's serial accept loop.

A raw client dribbles one byte every 100 ms without ever sending a
newline. Per-read socket timeouts (SO_RCVTIMEO on unix, the 1 s
WaitForSingleObject on windows) reset on every successful read, so the
old code served this one client for minutes while every other client
waited. The fix bounds the WHOLE line read (5 s): the dribbler must be
dropped within ~6 s and a second client must get its reply while the
dribbler is still trying.

Run:  python3 test/dribble/daemon_dribble.py [path-to-levari]
Exit 0 = guard holds, 1 = regression. Takes ~12 s (daemon runs a
selftest under the null backend so no audio device is needed).
"""
import os, socket, subprocess, sys, tempfile, time, shutil

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "build", "levari")
timeout_b = 8.0   # second client must be served well inside the 5 s budget
deadline_a = 8.0  # dribbler must be disconnected shortly after the 5 s cap

scratch = tempfile.mkdtemp(prefix="dribble-")
cfgdir = os.path.join(scratch, "config", "levari")
os.makedirs(cfgdir)
sock = os.path.join(scratch, "levari.sock")
with open(os.path.join(cfgdir, "config"), "w") as f:
    f.write("audio.backend = null\ndaemon.socket = %s\n" % sock)

env = dict(os.environ, HOME=scratch, XDG_CONFIG_HOME=os.path.join(scratch, "config"),
           XDG_RUNTIME_DIR=scratch, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
proc = subprocess.Popen([binary, "--selftest", "15", os.path.join(root, "test", "example.ogg")],
                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def wait_sock(secs=10):
    end = time.time() + secs
    while time.time() < end:
        if os.path.exists(sock):
            try:
                socket.socket(socket.AF_UNIX, socket.SOCK_STREAM).connect(sock)
                return True
            except OSError:
                pass
        time.sleep(0.1)
    return False

fails = []
try:
    if not wait_sock():
        print("FAIL: daemon socket never came up")
        sys.exit(1)

    # client A: dribble forever, never a newline
    a = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    a.connect(sock)
    t0 = time.time()

    def dribble():
        while time.time() - t0 < 20:
            try:
                a.sendall(b"x")
            except OSError:
                return
            time.sleep(0.1)
    import threading
    ta = threading.Thread(target=dribble, daemon=True)
    ta.start()

    # client B: while A is dribbling (well before the 5 s cap), demand a reply
    time.sleep(1.0)
    b = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    b.settimeout(timeout_b)
    b.connect(sock)
    b.sendall(b"status\n")
    tb = time.time()
    try:
        resp = b.recv(4096)
        dt = time.time() - tb
        if not resp:
            fails.append("second client got an empty reply")
        elif dt > timeout_b:
            fails.append("second client took %.1f s" % dt)
        else:
            print("OK: second client served in %.1f s while dribbler active" % dt)
    except socket.timeout:
        fails.append("second client timed out at %.0f s: dribble parked the accept loop" % timeout_b)
    b.close()

    # the dribbler itself must be disconnected shortly after the 5 s cap
    a.settimeout(deadline_a)
    dropped = False
    try:
        while True:
            if a.recv(4096) == b"":
                dropped = True
                break
    except (socket.timeout, OSError):
        pass
    tdrop = time.time() - t0
    if dropped:
        print("OK: dribbler disconnected after %.1f s" % tdrop)
        if tdrop > deadline_a:
            fails.append("dribbler drop took %.1f s (> %.0f s cap+margin)" % (tdrop, deadline_a))
    else:
        fails.append("dribbler never disconnected within %.0f s" % deadline_a)
    a.close()

    # the same deadline must NOT break a legitimate long command split
    # across several reads with gaps: build a real ~1.5 KB path, send it
    # in 300-byte chunks (well inside the 5 s budget) and expect it in `pl`
    deep = scratch
    for _ in range(5):
        deep = os.path.join(deep, "d" * 240)
    os.makedirs(deep)
    song = os.path.join(deep, "song.ogg")
    shutil.copy(os.path.join(root, "test", "example.ogg"), song)
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(5)
    c.connect(sock)
    payload = ('add "%s"\n' % song).encode()
    for i in range(0, len(payload), 300):
        c.sendall(payload[i:i + 300])
        time.sleep(0.15)
    resp = c.recv(4096)
    c.close()
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(5)
    c.connect(sock)
    c.sendall(b"pl\n")
    pl = c.recv(65536)
    c.close()
    if not resp.startswith(b"OK") or song.encode() not in pl:
        fails.append("chunked long add lost (resp=%r)" % resp[:12])
    else:
        print("OK: chunked %d-byte add across %d reads preserved" % (len(payload), (len(payload) + 299) // 300))
finally:
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    shutil.rmtree(scratch, ignore_errors=True)

if fails:
    print("FAIL: " + "; ".join(fails))
    sys.exit(1)
print("PASS: whole-line deadline holds")
sys.exit(0)
