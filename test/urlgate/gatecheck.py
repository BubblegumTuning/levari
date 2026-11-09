#!/usr/bin/env python3
"""Streaming gate (internal_docs/streaming-plan.md): loopback http
e2e + security rejects against the REAL binary. Hermetic: serves the
repo's own test/ over 127.0.0.1; scratch HOME/XDG null-backend config.

usage: python3 gatecheck.py [build-dir] [--off]   (default build-dir: ../../build)

--off runs the LV_STREAMING=OFF posture against a -DLV_STREAMING=OFF tree:
every URL argv must die with state=0 + the stderr "not compiled in" line
(then the sniff/fopen fallback fails it as a plain file); the negative
shapes relax to clean-fail (they never reach the URL branch, so the
message differs); the live/ICY/urlIndex/daemon-URL blocks are ON-only
behavior and are skipped.

ON additions (P2): urlIndex banks the last WORKING url byte-exactly and a
failed open never clobbers it (D5), and the daemon refuses `add <url>`
with the house ERR while `play <url>` stays transient (D3)."""
import os, socket, subprocess, sys, tempfile, time, shutil

ARGS = [a for a in sys.argv[1:]]
OFF = "--off" in ARGS
ARGS = [a for a in ARGS if a != "--off"]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(ARGS[0]) if ARGS else os.path.join(ROOT, "build")
BIN = os.path.join(BUILD, "levari")
TESTDIR = os.path.join(ROOT, "test")
fails = []

def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        fails.append(name)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def run(scratch, args, deadline=20):
    env = dict(os.environ,
               HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, ".config"),
               XDG_RUNTIME_DIR=scratch,
               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    try:
        r = subprocess.run([BIN] + args, env=env, capture_output=True, text=True, timeout=deadline)
        return r.stdout, r.stderr
    except subprocess.TimeoutExpired as e:
        return (e.stdout or b"").decode(errors="replace"), (e.stderr or b"").decode(errors="replace")

def selftest_sig(out):
    # success signature: state=1 with pos>0 (pos may be a/b or a/b/c)
    state, pos = None, None
    for tok in out.split():
        if tok.startswith("state="):
            state = int(tok[6:])
        if tok.startswith("pos="):
            pos = tok.split("=")[1]
            first = pos.split("/")[0]
            return state, first
    return None, None

def scratch_cfg(sc):
    os.makedirs(os.path.join(sc, ".config", "levari"))
    open(os.path.join(sc, ".config", "levari", "config"), "w").write(
        "audio.backend = null\n"
        f"daemon.socket = {sc}/levari.sock\n")

def cli(scratch, args, deadline=8):
    env = dict(os.environ, HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, ".config"), XDG_RUNTIME_DIR=scratch,
               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    r = subprocess.run([BIN] + args, env=env, capture_output=True, text=True, timeout=deadline)
    return r.stdout, r.stderr

def urlindex_path(sc):
    return os.path.join(sc, ".levari", "urlIndex")

def read_urlindex(sc):
    try:
        with open(urlindex_path(sc), "r") as f:
            return f.read()
    except FileNotFoundError:
        return None

def main():
    if not os.path.exists(BIN):
        sys.exit(f"no binary at {BIN}")
    port = free_port()
    srv = subprocess.Popen(["python3", "-m", "http.server", str(port), "--bind", "127.0.0.1"],
                           cwd=TESTDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.7)
    try:
        base = f"http://127.0.0.1:{port}"
        # ---- accepted: loopback stream plays (null backend, headless) ----
        with tempfile.TemporaryDirectory() as sc:
            scratch_cfg(sc)
            for kind, fn in (("flac", "sample.flac"), ("mp3", "sample.mp3")):
                out, err = run(sc, ["--selftest", "3", f"{base}/{fn}"])
                st, pos = selftest_sig(out)
                if OFF:
                    check(f"{kind} over http refused OFF-build (state=0 + not-compiled-in)",
                          st == 0 and "not compiled in" in err, f"st={st} err={err.strip()[:120]}")
                else:
                    check(f"{kind} over http plays (state=1 pos>0)",
                          st == 1 and pos not in (None, "0"), out.strip()[:120])

            if not OFF:
                # ---- live (total==0) shape: an endless icy-style stream with
                # no Content-Length — ffmpeg CANNOT know a duration; CLI seek
                # must refuse politely. Long selftest window holds the socket.
                live = os.path.join(tempfile.gettempdir(), "livegate.py")
                with open(live, "w") as f:
                    f.write(f"""
import socket, threading
data = open({TESTDIR!r} + "/sample.mp3", "rb").read()
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", {port + 1})); srv.listen(8)
def h(c):
    c.recv(65536)
    c.sendall(b"HTTP/1.1 200 OK\\r\\nContent-Type: audio/mpeg\\r\\nice-name: gate\\r\\nConnection: close\\r\\n\\r\\n")
    try:
        while True: c.sendall(data)
    except Exception: pass
    c.close()
while True:
    c, _ = srv.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
""")
                lsrv = subprocess.Popen([sys.executable, live], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(0.7)
                try:
                    env = dict(os.environ, HOME=sc,
                               XDG_CONFIG_HOME=os.path.join(sc, ".config"), XDG_RUNTIME_DIR=sc,
                               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
                    proc = subprocess.Popen([BIN, "--selftest", "10", f"http://127.0.0.1:{port + 1}/live.mp3"],
                                            env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                    sockpath = os.path.join(sc, "levari.sock")
                    for _ in range(50):
                        if os.path.exists(sockpath):
                            break
                        time.sleep(0.2)
                    time.sleep(1.5)
                    r = subprocess.run([BIN, "seek", "5"], env=env, capture_output=True, text=True, timeout=8)
                    check("live total==0: CLI seek refuses politely",
                          "ERR" in r.stdout and "seek" in r.stdout.lower(), r.stdout.strip())
                    r = subprocess.run([BIN, "status"], env=env, capture_output=True, text=True, timeout=8)
                    check("live status playing with unknown total",
                          "playing" in r.stdout and "0:00" in r.stdout.split("pos=")[1].split("/")[1],
                          r.stdout.strip())
                    proc.wait(timeout=15)
                finally:
                    lsrv.terminate(); lsrv.wait()

                # ---- icy StreamTitle updates reach the status title ----
                icy = os.path.join(tempfile.gettempdir(), "icygate.py")
                with open(icy, "w") as f:
                    f.write(f"""
import socket, threading
data = open({TESTDIR!r} + "/sample.mp3", "rb").read()
METAS = [b"StreamTitle='Gate One';", b"StreamTitle='Gate Two';"]
def blk(i):
    m = METAS[i % len(METAS)] + b" "
    n = (len(m) + 15) // 16
    return bytes([n]) + m.ljust(n * 16, b" ")
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", {port + 2})); srv.listen(8)
def h(c):
    c.recv(65536)
    c.sendall(b"HTTP/1.1 200 OK\\r\\nContent-Type: audio/mpeg\\r\\nicy-metaint: 2048\\r\\nice-name: levari-gate\\r\\nConnection: close\\r\\n\\r\\n")
    pos, i = 0, 0
    try:
        while True:
            c.sendall(data[pos:pos + 2048]); pos = (pos + 2048) % len(data)
            c.sendall(blk(i)); i += 1
    except Exception:
        pass
    c.close()
while True:
    c, _ = srv.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
""")
                isrv = subprocess.Popen([sys.executable, icy], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(0.7)
                try:
                    env = dict(os.environ, HOME=sc,
                               XDG_CONFIG_HOME=os.path.join(sc, ".config"), XDG_RUNTIME_DIR=sc,
                               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
                    proc = subprocess.Popen([BIN, "--selftest", "10", f"http://127.0.0.1:{port + 2}/icy.mp3"],
                                            env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                    for _ in range(50):
                        if os.path.exists(os.path.join(sc, "levari.sock")):
                            break
                        time.sleep(0.2)
                    time.sleep(2.5)
                    r = subprocess.run([BIN, "status"], env=env, capture_output=True, text=True, timeout=8)
                    check("icy StreamTitle lands on status title",
                          "title=Gate " in r.stdout, r.stdout.strip())
                    proc.wait(timeout=15)
                finally:
                    isrv.terminate(); isrv.wait()

        # ---- rejected: clean fails, no crash, correct gate messages ----
        # OFF posture: every URL shape lands in the dec_open not-compiled-in
        # branch (state=0 + that stderr line); concat:/CR are NOT URL shapes
        # and relax to a plain no-open fail (different message by design).
        for name, arg, want in [
            ("file:// scheme rejected", "file:///etc/passwd", "not allowed"),
            ("pipe:// scheme rejected", "pipe://x", "not allowed"),
            ("concat: not a URL, no-open", "concat:http://x/a.mp3", ""),
            ("gopher:// scheme rejected", "gopher://x/a.mp3", "not allowed"),
        ]:
            with tempfile.TemporaryDirectory() as sc:
                scratch_cfg(sc)
                out, err = run(sc, ["--selftest", "1", arg])
                st, _ = selftest_sig(out)
                if OFF:
                    want = "not compiled in" if arg != "concat:http://x/a.mp3" else ""
                check(name, st == 0 and (want in err), f"st={st} err={err.strip()[:100]}")

        # hygiene: CR injection never reaches a request line -> plain reject
        with tempfile.TemporaryDirectory() as sc:
            scratch_cfg(sc)
            evil = "http://127.0.0.1:%d/x\r\nX-Evil: 1" % port
            out, err = run(sc, ["--selftest", "1", evil])
            st, _ = selftest_sig(out)
            check("CR-injection argv clean-rejected", st == 0, f"st={st}")
            # unreachable-but-allowed scheme: clean fail, no crash
            out, err = run(sc, ["--selftest", "1", "http://127.0.0.1:%d/nope.flac" % port])
            st, _ = selftest_sig(out)
            want_ok = (not OFF) or "not compiled in" in err
            check("404 http stream clean fail", st == 0 and want_ok, f"st={st} err={err.strip()[:100]}")

        if not OFF:
            # ---- urlIndex (D5): banks the last WORKING url, byte-exact;
            # failed opens NEVER clobber. Fresh scratch: 404 -> absent;
            # working play -> exact row; 404 again -> survives untouched.
            with tempfile.TemporaryDirectory() as sc:
                scratch_cfg(sc)
                out, _ = run(sc, ["--selftest", "1", f"{base}/nope.flac"])
                st, _ = selftest_sig(out)
                check("404 preflight (state=0)", st == 0, f"st={st}")
                check("urlIndex absent after 404-only", read_urlindex(sc) is None,
                      repr(read_urlindex(sc)))
                good = f"{base}/sample.flac"
                out, _ = run(sc, ["--selftest", "3", good])
                st, pos = selftest_sig(out)
                check("working play preflight (state=1 pos>0)", st == 1 and pos not in (None, "0"), out.strip()[:120])
                check("urlIndex banks the working url byte-exact",
                      read_urlindex(sc) == good + "\n", repr(read_urlindex(sc)))
                out, _ = run(sc, ["--selftest", "1", f"{base}/nope2.flac"])
                st, _ = selftest_sig(out)
                check("failed open does not clobber urlIndex",
                      st == 0 and read_urlindex(sc) == good + "\n",
                      f"st={st} got={read_urlindex(sc)!r}")

            # ---- daemon D3: `add <url>` is the house ERR, `play <url>` is
            # transient (socket held open by a long --selftest window) ----
            with tempfile.TemporaryDirectory() as sc:
                scratch_cfg(sc)
                proc = subprocess.Popen([BIN, "--selftest", "15", os.path.join(TESTDIR, "sample.flac")],
                                        env=dict(os.environ, HOME=sc,
                                                 XDG_CONFIG_HOME=os.path.join(sc, ".config"),
                                                 XDG_RUNTIME_DIR=sc, SDL_VIDEODRIVER="dummy",
                                                 SDL_AUDIODRIVER="dummy"),
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    sockpath = os.path.join(sc, "levari.sock")
                    for _ in range(50):
                        if os.path.exists(sockpath):
                            break
                        time.sleep(0.2)
                    url = f"{base}/sample.mp3"
                    out, _ = cli(sc, ["pl"])
                    n0 = len([l for l in out.splitlines() if l])
                    out, _ = cli(sc, ["add", url])
                    check("daemon add <url> refused with the D3 ERR",
                          "ERR" in out and "url rows are not allowed" in out, out.strip())
                    out, _ = cli(sc, ["pl"])
                    check("ERR added no row (count + hygiene)",
                          len([l for l in out.splitlines() if l]) == n0 and "http" not in out, out.strip())
                    out, _ = cli(sc, ["add", os.path.join(TESTDIR, "a.wav")])
                    check("daemon add <file> still OK", out.strip().startswith("OK"), out.strip())
                    out, _ = cli(sc, ["pl"])
                    rows = [l for l in out.splitlines() if l]
                    check("add appended the local row",
                          len(rows) == n0 + 1 and any("a.wav" in r for r in rows) and
                          not any("http" in r for r in rows), out.strip())
                    out, _ = cli(sc, ["play", url])
                    check("daemon play <url> accepted (OK)", out.strip().startswith("OK"), out.strip())
                    time.sleep(1.2)
                    out, _ = cli(sc, ["pl"])
                    rows = [l for l in out.splitlines() if l]
                    check("play <url> swapped to ONE transient row",
                          len(rows) == 1 and url in rows[0], out.strip())
                    out, _ = cli(sc, ["status"])
                    # status carries no path — 1/1 asserts the single-row swap is live
                    check("daemon playing the URL stream (1/1)", "playing" in out and " 1/1 " in out, out.strip())
                finally:
                    proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill()

        # ---- unit pins ----
        cc = os.path.join(tempfile.gettempdir(), "testurlgate.exe")
        subprocess.run(["gcc", "-std=gnu11", "-I", os.path.join(ROOT, "src"),
                        "-o", cc, os.path.join(ROOT, "test", "urlgate", "testurlgate.c")], check=True)
        r = subprocess.run([cc], capture_output=True, text=True)
        check("urlgate unit pins", r.returncode == 0, r.stdout.strip())
    finally:
        srv.terminate()
        srv.wait()
    print(("GATECHECK%s: %d FAIL\n" % ("(OFF)" if OFF else "", len(fails))) if fails
          else "GATECHECK%s: ALL PASS" % ("(OFF)" if OFF else ""))
    return 1 if fails else 0

sys.exit(main())
