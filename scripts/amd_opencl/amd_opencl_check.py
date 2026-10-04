#!/usr/bin/env python3
"""Build Alcedo's OpenCL programs with AMD's Windows driver compiler, without an AMD GPU.

AMD's OpenCL runtime (amdocl64.dll, which calls amd_comgr_*.dll) can create offline devices, so
an Adrenalin driver package is enough to compile for gfx1030/gfx1100/gfx1201 on any Windows
machine. This catches AMD compiler crashes and build errors that NVIDIA and Intel OpenCL never
show. The program list is read from the OpenClProgramDescriptor registrations in the source
tree, so it follows the code.

Everything downloaded or built lives in <repo>/.amd_opencl/ (gitignored):
  installers/   Adrenalin packages        drivers/<version>[-legacy]/   extracted amdocl folders
  bin/          ocl_probe(.exe, _gbk.exe) runs/                per-program manifests and logs

Typical use:
  python scripts/amd_opencl/amd_opencl_check.py fetch 26.6.2
  python scripts/amd_opencl/amd_opencl_check.py build
  python scripts/amd_opencl/amd_opencl_check.py check
  python scripts/amd_opencl/amd_opencl_check.py check --program edit_geometry_lens_calib --gbk
  python scripts/amd_opencl/amd_opencl_check.py check --source my_kernel.cl --options "-cl-std=CL1.2"
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "alcedo_studio" / "src"
INCLUDE = SRC / "include"
HERE = Path(__file__).resolve().parent
WORK = REPO / ".amd_opencl"
DRIVERS = WORK / "drivers"
INSTALLERS = WORK / "installers"
BIN = WORK / "bin"
RUNS = WORK / "runs"

# Adrenalin ships two OpenCL runtimes. Display/ (drivers/<version>) serves RDNA2 and newer;
# Display2/ (drivers/<version>-legacy) serves Polaris, Vega, the Vega iGPU of Ryzen APUs and RDNA1.
DRIVER_BRANCHES = {"": "Display", "-legacy": "Display2"}
# RDNA2, RDNA3, RDNA3 iGPU (780M), RDNA4.
DEFAULT_DEVICES = ["gfx1030", "gfx1100", "gfx1103", "gfx1201"]
# RX 5700 (RDNA1). The legacy runtime also lists gfx8/gfx9 (Polaris, Vega, Ryzen APU iGPUs), but
# it compiles those through HSAIL, and offline HSAIL builds fail for every kernel ("The instruction
# set architecture name is invalid"), so they cannot be checked this way.
DEFAULT_LEGACY_DEVICES = ["gfx1010:xnack-"]
CRASH_CODE = 0xC0000005
VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
BROWSER_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/130 Safari/537.36"


@dataclass
class Program:
    name: str
    options: str
    sources: list[Path]
    required_at_startup: bool


# ---------------------------------------------------------------------------
# Program list from the source tree
# ---------------------------------------------------------------------------


def _string_constants() -> dict[str, str]:
    """`inline constexpr const char* kName = "a" "b";` in headers -> {kName: "ab"}."""
    pattern = re.compile(r'inline\s+constexpr\s+const\s+char\*\s+(\w+)\s*=\s*((?:"[^"]*"\s*)+);')
    constants: dict[str, str] = {}
    for header in INCLUDE.rglob("*.hpp"):
        text = header.read_text(encoding="utf-8", errors="replace")
        for name, literals in pattern.findall(text):
            constants[name] = "".join(re.findall(r'"([^"]*)"', literals))
    return constants


def _source_macros() -> dict[str, Path]:
    """ALCEDO_OPENCL_* compile definitions of OpenClProgramLibrary -> file paths."""
    cmake = (SRC / "opencl" / "CMakeLists.txt").read_text(encoding="utf-8")
    roots = {"ALCEDO_SRC_ROOT": str(SRC), "ALCEDO_INCLUDE_ROOT": str(INCLUDE)}
    variables = {name: value for name, value in re.findall(r'set\((\w+)\s+"([^"]+)"\)', cmake)}

    def expand(value: str) -> str:
        for _ in range(4):
            value = re.sub(r"\$\{(\w+)\}", lambda m: roots.get(m.group(1), variables.get(m.group(1), m.group(0))), value)
        return value

    macros = {}
    for name, value in re.findall(r'(ALCEDO_OPENCL_\w+)="([^"]+)"', cmake):
        path = Path(expand(value))
        if path.suffix in (".cl", ".h"):
            macros[name] = path
    return macros


def _resolve(token: str, constants: dict[str, str]) -> str:
    token = token.strip()
    if token.startswith('"'):
        return "".join(re.findall(r'"([^"]*)"', token))
    key = token.split("::")[-1]
    if key not in constants:
        raise SystemExit(f"cannot resolve {token} in program registrations")
    return constants[key]


def load_programs() -> list[Program]:
    constants = _string_constants()
    macros = _source_macros()
    programs: list[Program] = []
    for cpp in SRC.rglob("*.cpp"):
        if "third_party" in cpp.parts:
            continue
        text = cpp.read_text(encoding="utf-8", errors="replace")
        if "OpenClProgramDescriptor{" not in text or "RegisterManifest" not in text:
            continue
        for block in re.findall(r"OpenClProgramDescriptor\{(.*?)\n\s*\}", text, re.S):
            fields = dict(re.findall(r"\.(\w+)\s*=\s*(\{[^}]*\}|[^,\n]+(?:\n\s*\"[^\n]*)*)", block))
            sources = []
            for macro in re.findall(r"ALCEDO_OPENCL_\w+", fields.get("source_paths", "")):
                if macro not in macros:
                    raise SystemExit(f"{cpp.name}: {macro} is not an OpenClProgramLibrary compile definition")
                sources.append(macros[macro])
            programs.append(
                Program(
                    name=_resolve(fields["name"], constants),
                    options=_resolve(fields["build_options"], constants) if "build_options" in fields else "",
                    sources=sources,
                    required_at_startup=fields.get("required_at_startup", "false").strip() == "true",
                )
            )
    if not programs:
        raise SystemExit("no OpenClProgramDescriptor registrations found")
    return sorted(programs, key=lambda p: (not p.required_at_startup, p.name))


# ---------------------------------------------------------------------------
# fetch / build
# ---------------------------------------------------------------------------


def _seven_zip() -> str:
    for candidate in (shutil.which("7z"), r"C:\Program Files\7-Zip\7z.exe"):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit("7-Zip (7z) is required to extract the driver package")


def fetch(version: str) -> None:
    targets = {DRIVERS / f"{version}{suffix}": folder for suffix, folder in DRIVER_BRANCHES.items()}
    if all((target / "amdocl64.dll").exists() for target in targets):
        print(f"{', '.join(map(str, targets))} already present")
        return
    INSTALLERS.mkdir(parents=True, exist_ok=True)
    installer = INSTALLERS / f"adrenalin-{version}.exe"
    if not installer.exists():
        notes = f"https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-{version.replace('.', '-')}.html"
        request = urllib.request.Request(notes, headers={"User-Agent": BROWSER_UA})
        page = urllib.request.urlopen(request, timeout=120).read().decode("utf-8", "replace")
        links = sorted(set(re.findall(r"https://drivers\.amd\.com/[^\"' <>]+\.exe", page)))
        if not links:
            raise SystemExit(f"no driver download link on {notes}")
        print(f"downloading {links[0]} (about 1.6 GB)")
        request = urllib.request.Request(links[0], headers={"User-Agent": BROWSER_UA, "Referer": "https://www.amd.com/"})
        with urllib.request.urlopen(request, timeout=3600) as response, open(installer, "wb") as out:
            shutil.copyfileobj(response, out, length=8 << 20)
    for target, folder in targets.items():
        if (target / "amdocl64.dll").exists():
            continue
        extract = WORK / "extract_tmp"
        subprocess.run([_seven_zip(), "x", "-y", f"-o{extract}", str(installer),
                        f"Packages/Drivers/{folder}/WT6A_INF/amdocl/*"], check=True, stdout=subprocess.DEVNULL)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(extract / f"Packages/Drivers/{folder}/WT6A_INF/amdocl"), str(target))
        shutil.rmtree(extract, ignore_errors=True)
        print(f"extracted {target}")


def _vs_dev_cmd() -> Path:
    install = subprocess.run([str(VSWHERE), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], capture_output=True, text=True, check=True).stdout.strip()
    return Path(install) / "Common7" / "Tools" / "VsDevCmd.bat"


def _opencl_include() -> Path:
    for candidate in (REPO / "vcpkg" / "installed" / "x64-windows" / "include",
                      REPO / "build" / "debug" / "vcpkg_installed" / "x64-windows" / "include"):
        if (candidate / "CL" / "cl_icd.h").exists():
            return candidate
    raise SystemExit("OpenCL headers (CL/cl_icd.h) not found; bootstrap vcpkg first")


def build() -> None:
    BIN.mkdir(parents=True, exist_ok=True)
    source = HERE / "ocl_probe.cpp"
    probe = BIN / "ocl_probe.exe"
    gbk = BIN / "ocl_probe_gbk.exe"
    # A .cmd file, because Python escapes the quotes of a command line passed to cmd /c.
    script = BIN / "build_probe.cmd"
    script.write_text(
        "@echo off\r\n"
        f'call "{_vs_dev_cmd()}" -arch=x64 -host_arch=x64 >nul || exit /b 1\r\n'
        f'cl /nologo /std:c++20 /EHsc /O2 /I"{_opencl_include()}" /Fo"{BIN}\\\\" "{source}" /Fe"{probe}" >nul || exit /b 1\r\n'
        f'copy /y "{probe}" "{gbk}" >nul || exit /b 1\r\n'
        f'mt -nologo -manifest "{HERE / "gbk.manifest"}" -outputresource:"{gbk}";#1 || exit /b 1\r\n',
        encoding="utf-8")
    env = dict(os.environ)
    env["PATH"] = str(VSWHERE.parent) + os.pathsep + env.get("PATH", "")  # VsDevCmd calls vswhere
    subprocess.run(["cmd", "/d", "/c", str(script)], check=True, env=env)
    print(f"built {probe} and {gbk}")


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------


def _run(exe: Path, driver: Path, device: str, threads: int, manifest: Path, log: Path) -> tuple[str, str]:
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        code = subprocess.run([str(exe), str(driver), device, str(threads), str(manifest)],
                              stdout=out, stderr=subprocess.STDOUT).returncode & 0xFFFFFFFF
    text = log.read_text(encoding="utf-8", errors="replace")
    if code == CRASH_CODE:
        line = next((l for l in text.splitlines() if l.startswith("CRASH")), "CRASH")
        module = re.search(r"module=.*\\([^\\]+) offset=(\S+)", line)
        return "CRASH", f"{module.group(1)}+{module.group(2)}" if module else line
    if code == 0:
        return "OK", ""
    first = next((l.strip() for l in text.splitlines() if "error" in l.lower() or "failed" in l), "")
    return f"FAIL({code:#x})", first[:160]


def check(args: argparse.Namespace) -> int:
    exe = BIN / ("ocl_probe_gbk.exe" if args.gbk else "ocl_probe.exe")
    if not exe.exists():
        build()
    drivers = sorted(p for p in DRIVERS.iterdir() if (p / "amdocl64.dll").exists()) if DRIVERS.exists() else []
    if args.driver:
        drivers = [d for d in drivers if d.name in args.driver]
    if not drivers:
        raise SystemExit("no driver under .amd_opencl/drivers; run: amd_opencl_check.py fetch <version>")

    if args.source:
        programs = [Program(Path(args.source).stem, args.options, [Path(args.source).resolve()], False)]
    else:
        programs = load_programs()
        if args.program:
            programs = [p for p in programs if p.name in args.program]
            if not programs:
                raise SystemExit("no matching program; see: amd_opencl_check.py list")

    RUNS.mkdir(parents=True, exist_ok=True)
    failures = 0
    for driver in drivers:
        legacy = driver.name.endswith("-legacy")
        for device in args.device or (DEFAULT_LEGACY_DEVICES if legacy else DEFAULT_DEVICES):
            if args.threads > 1:
                manifest = RUNS / f"{driver.name}-{device.replace(':', '_')}-all.txt"
                manifest.write_text("".join(f"{p.name}\t{p.options}\t{';'.join(map(str, p.sources))}\n"
                                            for p in programs), encoding="utf-8")
                status, detail = _run(exe, driver, device, args.threads, manifest, manifest.with_suffix(".log"))
                print(f"{driver.name:15s} {device:15s} {args.threads} threads, {len(programs)} programs: {status} {detail}")
                failures += status != "OK"
                continue
            for program in programs:
                # One process per program: a compiler crash ends the process.
                manifest = RUNS / f"{driver.name}-{device.replace(':', '_')}-{program.name}.txt"
                manifest.write_text(f"{program.name}\t{program.options}\t{';'.join(map(str, program.sources))}\n",
                                    encoding="utf-8")
                status, detail = _run(exe, driver, device, 1, manifest, manifest.with_suffix(".log"))
                if status != "OK" or args.verbose:
                    print(f"{driver.name:15s} {device:15s} {program.name:32s} {status} {detail}")
                failures += status != "OK"
            if not args.verbose:
                print(f"{driver.name:15s} {device:15s} {len(programs)} programs checked")
    print("all programs built" if failures == 0 else f"{failures} failed build(s)")
    return 0 if failures == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    fetch_parser = sub.add_parser("fetch", help="download an Adrenalin package and extract its OpenCL runtime")
    fetch_parser.add_argument("version", help="Adrenalin version, for example 26.6.2")
    sub.add_parser("build", help="build ocl_probe.exe and ocl_probe_gbk.exe")
    sub.add_parser("list", help="print the OpenCL programs registered in the source tree")
    check_parser = sub.add_parser("check", help="build programs with every fetched driver")
    check_parser.add_argument("--driver", nargs="*", help="driver versions (default: all fetched)")
    check_parser.add_argument("--device", nargs="*", help=f"offline devices (default: {' '.join(DEFAULT_DEVICES)}; "
                              f"{' '.join(DEFAULT_LEGACY_DEVICES)} for -legacy drivers)")
    check_parser.add_argument("--program", nargs="*", help="registered program names (default: all)")
    check_parser.add_argument("--source", help="build this .cl file instead of the registered programs")
    check_parser.add_argument("--options", default="-cl-std=CL1.2", help="build options for --source")
    check_parser.add_argument("--threads", type=int, default=1, help="build all programs at once on N threads")
    check_parser.add_argument("--gbk", action="store_true", help="run the probe with code page 936 (GBK)")
    check_parser.add_argument("--verbose", action="store_true", help="print every program, not only failures")
    args = parser.parse_args()

    if args.command == "fetch":
        fetch(args.version)
    elif args.command == "build":
        build()
    elif args.command == "list":
        for p in load_programs():
            print(f"{p.name:32s} startup={str(p.required_at_startup).lower():5s} options='{p.options}'")
            for s in p.sources:
                print(f"    {s.relative_to(REPO)}")
    else:
        return check(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
