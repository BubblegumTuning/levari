#!/usr/bin/env python3
"""verify gate: `levari library verify` over a hand-built v2 index.

Pins: live row kept; gone row (ENOENT) dropped AND banked verbatim in
index.dropped; EACCES row (unreadable dir -> "cannot ask") kept and
counted unreachable; non-regular row (path is a directory) dropped;
escaped-backslash row resolved via field-1 unescape and kept; header
line byte-identical after rewrite; second run on an all-live index
leaves the file byte-identical; missing index reports clean; rows
longer than the fixed readers' buffer survive a purge-triggered
rewrite verbatim (over-long trailing fields keep their field-1 verdict;
an over-long FIELD-1 cannot be stat'd and joins the unreachable class
instead of being silently deleted or prefix-ENOENT'd).

Usage: python3 test/verify/testverify.py [build-dir]
"""
import os, subprocess, tempfile, shutil, sys, stat

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
build = sys.argv[1] if len(sys.argv) > 1 else "build"
BIN = os.path.join(repo, build, "levari")
assert os.path.exists(BIN), BIN

S = tempfile.mkdtemp(prefix="verifygate-")
fails = 0
def check(name, ok):
    global fails
    print(("PASS " if ok else "FAIL ") + name)
    if not ok: fails += 1

def verify(home):
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/.config",
               XDG_RUNTIME_DIR=home)
    p = subprocess.run([BIN, "library", "verify"], env=env,
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr

HDR = "#levari-index v2 0.0.0 700101\n"

def build_index(home, rows):
    d = os.path.join(home, ".levari")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "index"), "w") as f:
        f.write(HDR)
        f.writelines(rows)
    return os.path.join(d, "index")

def mkhome():
    h = tempfile.mkdtemp(prefix="verifyhome-", dir=S)
    os.makedirs(os.path.join(h, ".config"))
    return h

# --- case A: live + gone + eacces + dir-row + escaped-backslash row ----
h = mkhome()
live = os.path.join(h, "live.wav")
open(live, "w").write("RIFF")
esc_file = os.path.join(h, "a\\b.wav")          # literal backslash name
open(esc_file, "w").write("RIFF")
gone = os.path.join(h, "gone.wav")              # never created
nodir = os.path.join(h, "locked")               # 000 dir, path inside
os.makedirs(nodir)
os.chmod(nodir, 0)
eacces = os.path.join(nodir, "x.wav")
notfile = os.path.join(h, "adir")               # a directory where a row points
os.makedirs(notfile)
rows = [f"{live}\tA\tT\tAl\t100\t1\n",
        f"{gone}\tA\tT\tAl\t100\t1\n",
        f"{eacces}\tA\tT\tAl\t100\t1\n",
        f"{notfile}\tA\tT\tAl\t100\t1\n",
        f"{esc_file.replace(chr(92), chr(92)*2)}\tA\tT\tAl\t100\t1\t1\n"]
idxp = build_index(h, rows)
rc, out = verify(h)
check("case A rc==0", rc == 0)
check("case A counts (kept 3 dropped 2 unreachable 1): " + out.strip().replace("\n", " | "),
      "kept 3, dropped 2, unreachable kept 1" in out)
after = open(idxp).read()
check("case A header byte-identical", after.startswith(HDR))
check("case A live row verbatim", f"{live}\tA\tT\tAl\t100\t1\n" in after)
check("case A eacces row kept", f"{eacces}\tA\tT\tAl\t100\t1\n" in after)
check("case A escaped row verbatim",
      f"{esc_file.replace(chr(92), chr(92)*2)}\tA\tT\tAl\t100\t1\t1\n" in after)
check("case A gone row gone", f"{gone}\t" not in after)
check("case A dir row gone", f"{notfile}\t" not in after)
bank = open(idxp + ".dropped").read()
check("case A bank holds both dropped rows verbatim",
      f"{gone}\tA\tT\tAl\t100\t1\n" in bank and f"{notfile}\tA\tT\tAl\t100\t1\n" in bank)

# --- case B: second run, nothing to drop -> index byte-identical -------
before = open(idxp, "rb").read()
rc, out = verify(h)
check("case B reports clean: " + out.strip().replace("\n", " | "),
      rc == 0 and "dropped 0" in out)
check("case B index untouched", open(idxp, "rb").read() == before)

# --- case C: missing index reports clean --------------------------------
h = mkhome()
rc, out = verify(h)
check("case C missing index clean: " + out.strip().replace("\n", " | "),
      rc == 0 and "kept 0, dropped 0" in out)

# --- case D: oversize rows survive a purge-triggered rewrite ------------
# (the fixed readers skipped them while counting them kept: once any row
# triggered the rewrite they vanished with NO bank entry, and a long
# FIELD-1 could even prefix-ENOENT a live path into deletion)
h = mkhome()
live = os.path.join(h, "live.wav")
open(live, "w").write("RIFF")
gone = os.path.join(h, "gone.wav")
big_fields = f"{live}\t" + "x" * 20000 + "\t100\t1\n"     # live, long fields
big_path = "y" * 20000 + "\tA\tT\tAl\t100\t1\n"           # unstatable field-1
idxp = build_index(h, [f"{live}\tA\tT\tAl\t100\t1\n",
                       f"{gone}\tA\tT\tAl\t100\t1\n",      # forces rewrite
                       big_fields, big_path])
rc, out = verify(h)
check("case D rc==0", rc == 0)
check("case D counts (kept 3 dropped 1 unreachable 1): " + out.strip().replace("\n", " | "),
      "kept 3, dropped 1, unreachable kept 1" in out)
after = open(idxp).read()
check("case D oversize-fields row verbatim", big_fields in after)
check("case D oversize-path row verbatim", big_path in after)
check("case D gone row still dropped", f"{gone}\t" not in after)
check("case D bank only holds the gone row",
      open(idxp + ".dropped").read() == f"{gone}\tA\tT\tAl\t100\t1\n")

shutil.rmtree(S, ignore_errors=True)
sys.exit(1 if fails else 0)
