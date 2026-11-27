#!/usr/bin/env python3
"""Build beemu.app from a CMake build directory.

    python3 tools/make_app.py [BUILD_DIR] [OUTPUT.app]      (or: cmake --build build --target app)

The bundle is self-contained:
  Contents/MacOS/beemu          the emulator
  Contents/Frameworks/          SDL3.framework, libslirp and the Homebrew libraries it needs,
                                with install names rewritten to @rpath
  Contents/Resources/firmware/  SeaBIOS + SeaVGABIOS (copied from bios/ or Homebrew's QEMU) and
                                their license (LGPLv3)
  Contents/Resources/licenses/  license files of the bundled libraries
Then everything is signed ad hoc, which Apple Silicon requires to run code at all. Not
notarized: fine on the Mac that built it; another Mac needs right-click > Open the first time.
"""
import glob, os, plistlib, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build"))
APP = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(BUILD, "beemu.app"))
BIN = os.path.join(BUILD, "beemu")
CONTENTS = os.path.join(APP, "Contents")
MACOS, FW, RES = (os.path.join(CONTENTS, d) for d in ("MacOS", "Frameworks", "Resources"))


def run(*cmd, check=True):
    return subprocess.run(cmd, check=check, capture_output=True, text=True).stdout


def install_id(path):
    out = run("otool", "-D", path).splitlines()
    return out[1].strip() if len(out) > 1 else None


def linked(path):
    own = install_id(path)
    deps = [l.split()[0] for l in run("otool", "-L", path).splitlines()[1:] if l.strip()]
    return [d for d in deps if d != own]


def rpaths(path):
    return re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (.+?) \(offset", run("otool", "-l", path))


def system_lib(dep):
    return dep.startswith("/usr/lib/") or dep.startswith("/System/")


def resolve(dep, original):
    """Where a load command of `original` (a file outside the bundle) really points."""
    here = os.path.dirname(original)
    if dep.startswith("@loader_path/"):
        return os.path.normpath(os.path.join(here, dep[len("@loader_path/"):]))
    if dep.startswith("@executable_path/"):
        return os.path.normpath(os.path.join(os.path.dirname(BIN), dep[len("@executable_path/"):]))
    if dep.startswith("@rpath/"):
        rest = dep[len("@rpath/"):]
        search = [r.replace("@loader_path", here).replace("@executable_path", os.path.dirname(BIN))
                  for r in rpaths(original)] + ["/opt/homebrew/lib", "/Library/Frameworks"]
        for r in search:
            cand = os.path.join(r, rest)
            if os.path.exists(cand):
                return cand
        sys.exit(f"make_app: cannot find {dep} (needed by {original})")
    return dep


def framework_root(path):
    m = re.match(r"(.*/[^/]+\.framework)/(.*)", path)
    return (m.group(1), m.group(2)) if m else (None, None)


def main():
    if not os.path.exists(BIN):
        sys.exit(f"make_app: {BIN} not found; build first (cmake --build {BUILD})")
    shutil.rmtree(APP, ignore_errors=True)
    for d in (MACOS, FW, RES, os.path.join(RES, "firmware"), os.path.join(RES, "licenses")):
        os.makedirs(d)
    exe = os.path.join(MACOS, "beemu")
    shutil.copy2(BIN, exe)

    # ---- libraries: copy every non-system dependency, rewrite references to @rpath ----
    origin = {exe: BIN}                  # bundled file -> the file it was copied from
    todo, done, copied_from = [exe], set(), set()
    while todo:
        f = todo.pop()
        if f in done:
            continue
        done.add(f)
        for dep in linked(f):
            if system_lib(dep):
                continue
            real = resolve(dep, origin[f])
            fw, inner = framework_root(real)
            if fw:                                           # the whole framework, symlinks kept
                dest_fw = os.path.join(FW, os.path.basename(fw))
                if not os.path.exists(dest_fw):
                    shutil.copytree(fw, dest_fw, symlinks=True,
                                    ignore=shutil.ignore_patterns("Headers", "_CodeSignature"))
                new_ref = "@rpath/" + os.path.basename(fw) + "/" + inner
                dest = os.path.join(dest_fw, inner)
            else:
                name = os.path.basename(dep)
                dest = os.path.join(FW, name)
                if not os.path.exists(dest):
                    shutil.copy2(os.path.realpath(real), dest)
                    os.chmod(dest, 0o755)
                    run("install_name_tool", "-id", "@rpath/" + name, dest)
                new_ref = "@rpath/" + name
            copied_from.add(os.path.realpath(real))
            if dest not in origin:
                origin[dest] = real
                todo.append(dest)
            if dep != new_ref:
                run("install_name_tool", "-change", dep, new_ref, f)
    for r in rpaths(exe):
        run("install_name_tool", "-delete_rpath", r, exe)
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", exe)
    for f in origin:
        if f != exe and "@loader_path" not in rpaths(f):
            run("install_name_tool", "-add_rpath", "@loader_path", f, check=False)

    # ---- licenses of the bundled libraries ----
    for real in copied_from:
        m = re.match(r"(/opt/homebrew/Cellar/([^/]+)/[^/]+)/", real)
        if m:                                                # Homebrew keeps them next to the keg
            dest = os.path.join(RES, "licenses", m.group(2))
            os.makedirs(dest, exist_ok=True)
            for lic in glob.glob(os.path.join(m.group(1), "*")):
                if os.path.isfile(lic) and re.match(r"(COPYING|LICENSE|COPYRIGHT)", os.path.basename(lic), re.I):
                    shutil.copy2(lic, dest)
        fw, _ = framework_root(real)
        if fw:                                               # frameworks carry their own
            dest = os.path.join(RES, "licenses", os.path.basename(fw).replace(".framework", ""))
            os.makedirs(dest, exist_ok=True)
            for lic in glob.glob(os.path.join(fw, "Resources", "*")):
                if os.path.isfile(lic) and re.match(r"(COPYING|LICENSE)", os.path.basename(lic), re.I):
                    shutil.copy2(lic, dest)

    # ---- firmware ----
    for name in ("bios.bin", "vgabios-stdvga.bin"):
        for d in (os.path.join(ROOT, "bios"), "/opt/homebrew/share/qemu", "/usr/local/share/qemu"):
            if os.path.exists(os.path.join(d, name)):
                shutil.copy2(os.path.join(d, name), os.path.join(RES, "firmware", name))
                break
        else:
            sys.exit(f"make_app: firmware {name} not found (brew install qemu, or put it in bios/)")
    for f in glob.glob(os.path.join(ROOT, "resources", "firmware", "*")):
        shutil.copy2(f, os.path.join(RES, "firmware"))

    # ---- icon ----
    iconset = os.path.join(BUILD, "beemu.iconset")
    shutil.rmtree(iconset, ignore_errors=True)
    os.makedirs(iconset)
    src = os.path.join(ROOT, "resources", "beemu-icon.png")
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = size * scale
            out = os.path.join(iconset, f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png")
            run("sips", "-z", str(px), str(px), src, "--out", out)
    run("iconutil", "-c", "icns", iconset, "-o", os.path.join(RES, "beemu.icns"))
    shutil.rmtree(iconset)

    # ---- Info.plist ----
    minos = re.search(r"minos (\S+)", run("otool", "-l", exe))
    info = {
        "CFBundleName": "beemu",
        "CFBundleDisplayName": "beemu",
        "CFBundleIdentifier": "org.beemu.beemu",
        "CFBundleExecutable": "beemu",
        "CFBundleIconFile": "beemu.icns",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "0.1",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": minos.group(1) if minos else "13.0",
        "LSArchitecturePriority": ["arm64"],
        "NSHighResolutionCapable": True,
        "LSApplicationCategoryType": "public.app-category.utilities",
        "NSHumanReadableCopyright": "beemu: BeOS emulator for Apple Silicon",
    }
    with open(os.path.join(CONTENTS, "Info.plist"), "wb") as f:
        plistlib.dump(info, f)

    # ---- sign: inside out (libraries, frameworks, then the app) ----
    for f in sorted(origin, key=len, reverse=True):
        if f == exe:
            continue
        fw, _ = framework_root(f)
        run("codesign", "--force", "--sign", "-", fw or f)
    run("codesign", "--force", "--sign", "-", APP)
    run("codesign", "--verify", "--strict", APP)
    print(f"make_app: built {APP}")
    for f in sorted(origin):
        print("   ", os.path.relpath(f, APP))


if __name__ == "__main__":
    main()
