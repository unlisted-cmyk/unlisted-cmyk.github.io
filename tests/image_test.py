#!/usr/bin/env python3
"""Tests the installer scripts of image/ against a FAKE release (no network, nothing installed on the machine). Linux only.
Usage: python3 tests/image_test.py ./logl"""
import configparser, hashlib, os, shutil, stat, subprocess, sys, tempfile
binary = os.path.abspath(sys.argv[1]); root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
setup = os.path.join(root, "image", "pi-os", "setup-log-os.sh"); fetch = os.path.join(root, "image", "buildroot", "fetch-logl.sh"); post = os.path.join(root, "image", "buildroot", "post-build.sh")
ok = True
def check(name, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name); ok = ok and bool(cond)
def sha(p): return hashlib.sha256(open(p, "rb").read()).hexdigest()
tmp = tempfile.mkdtemp()
try:
    rel = os.path.join(tmp, "rel"); os.makedirs(rel); asset = os.path.join(rel, "logl-linux-arm64"); shutil.copy(binary, asset)
    open(os.path.join(rel, "SHA256SUMS.txt"), "w").write("%s  logl-linux-arm64\n" % sha(asset))
    dest = os.path.join(tmp, "root"); boot = os.path.join(dest, "boot", "firmware"); os.makedirs(boot)
    open(os.path.join(boot, "cmdline.txt"), "w").write("console=serial0,115200 console=tty1 root=PARTUUID=abc rootwait\n"); open(os.path.join(boot, "config.txt"), "w").write("dtparam=audio=on\n")
    env = dict(os.environ, DRY="1", DESTDIR=dest, LOGL_BASE_URL="file://" + rel, LOGL_ALLOW_ANY_ARCH="1")
    def run_setup(extra_env=None, args=()):
        e = dict(env); e.update(extra_env or {}); return subprocess.run(["bash", setup, *args], capture_output=True, text=True, env=e, timeout=60)
    r = run_setup()
    check("installer succeeds with a valid release", r.returncode == 0 and "SHA-256 verified" in r.stdout)
    for f in ("usr/local/bin/logl", "usr/local/bin/logl-session", "etc/systemd/system/logl-console.service", "etc/logl.conf"):
        check("installed " + f, os.path.exists(os.path.join(dest, f)))
    check("logl and logl-session are executable", all(os.stat(os.path.join(dest, "usr/local/bin", f)).st_mode & stat.S_IXUSR for f in ("logl", "logl-session")))
    check("installed logl is the verified file", sha(os.path.join(dest, "usr/local/bin/logl")) == sha(asset))
    check("system commands are only printed in a dry run", "+ systemctl enable logl-console.service" in r.stdout and "+ raspi-config nonint do_i2c 0" in r.stdout)
    run_setup(); run_setup()
    c = open(os.path.join(boot, "cmdline.txt")).read(); cf = open(os.path.join(boot, "config.txt")).read()
    check("running it three times does not duplicate boot settings", c.count("logo.nologo") == 1 and cf.count("disable_splash=1") == 1 and c.count("\n") == 1)
    cp = configparser.ConfigParser(strict=False); cp.read(os.path.join(dest, "etc/systemd/system/logl-console.service"))
    check("service file is valid and points at the installed session script", cp["Service"]["ExecStart"] == "/usr/local/bin/logl-session" and cp["Service"]["Restart"] == "always")
    # a modified download must be refused and nothing installed
    shutil.rmtree(os.path.join(dest, "usr")); open(asset, "ab").write(b"tampered")
    r = run_setup()
    check("a modified binary is REFUSED (exit code != 0)", r.returncode != 0 and "DOES NOT MATCH" in r.stderr)
    check("and nothing was installed", not os.path.exists(os.path.join(dest, "usr/local/bin/logl")))
    open(os.path.join(rel, "SHA256SUMS.txt"), "w").write("")
    r = run_setup(); check("an asset missing from SHA256SUMS.txt is refused", r.returncode != 0 and "not listed" in r.stderr)
    # DRY=1 without DESTDIR must never write to the real system
    shutil.copy(binary, asset); open(os.path.join(rel, "SHA256SUMS.txt"), "w").write("%s  logl-linux-arm64\n" % sha(asset))
    before = os.path.exists("/usr/local/bin/logl-session"); e2 = dict(env); e2.pop("DESTDIR")
    r = subprocess.run(["bash", setup], capture_output=True, text=True, env=e2, timeout=60)
    check("DRY=1 without DESTDIR installs under a temporary folder only", r.returncode == 0 and "dry run: files go under" in r.stdout and os.path.exists("/usr/local/bin/logl-session") == before)
    r = subprocess.run(["bash", setup, "/no/such/file"], capture_output=True, text=True, env=env, timeout=60); check("a missing local binary is an error", r.returncode != 0)
    # Buildroot helper scripts
    out = os.path.join(root, "image", "buildroot", "overlay", "usr", "bin", "logl"); fenv = dict(os.environ, LOGL_BASE_URL="file://" + rel)
    r = subprocess.run(["bash", fetch], capture_output=True, text=True, env=fenv, timeout=60)
    check("buildroot fetch-logl.sh verifies and writes the binary", r.returncode == 0 and os.path.exists(out) and sha(out) == sha(asset))
    os.remove(out); open(asset, "ab").write(b"x")
    r = subprocess.run(["bash", fetch], capture_output=True, text=True, env=fenv, timeout=60); check("buildroot fetch-logl.sh refuses a modified binary and writes nothing", r.returncode != 0 and not os.path.exists(out))
    T = os.path.join(tmp, "T"); B = os.path.join(tmp, "B"); os.makedirs(os.path.join(T, "usr", "bin")); os.makedirs(os.path.join(B, "rpi-firmware")); open(os.path.join(B, "rpi-firmware", "config.txt"), "w").write("kernel=Image\n")
    for _ in range(2): subprocess.run(["sh", post, T], env=dict(os.environ, BINARIES_DIR=B), check=True)
    cfg = open(os.path.join(B, "rpi-firmware", "config.txt")).read()
    check("buildroot post-build.sh adds the bus settings once and installs logl-session", cfg.count("dtparam=i2c_arm=on") == 1 and cfg.count("enable_uart=1") == 1 and os.path.exists(os.path.join(T, "usr/bin/logl-session")))
    for s in (setup, fetch, post, os.path.join(root, "image", "buildroot", "build.sh")):
        check("bash -n " + os.path.relpath(s, root), subprocess.run(["bash", "-n", s]).returncode == 0)
    check("sh -n image/common/logl-session", subprocess.run(["sh", "-n", os.path.join(root, "image", "common", "logl-session")]).returncode == 0)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
    p = os.path.join(root, "image", "buildroot", "overlay", "usr", "bin", "logl")
    if os.path.exists(p): os.remove(p)
print("image test:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
