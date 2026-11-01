#!/usr/bin/env python3
"""Release files and the web-flasher site.

Two steps, used by CI (.github/workflows/firmware.yml) and for local previews:

  1. dist: turn a firmware build into release files
       python3 tools/make_flasher_site.py dist [--build build] [--out dist] [--version v1.2.3]
     -> every part ESP-IDF lists in flasher_args.json (bootloader, partition table, otadata, the app as
        <app>-<version>.bin) and flash-parts.json (chip, version, build time and the flash offset of each file)

  2. site: assemble the web flasher from one or two release folders
       python3 tools/make_flasher_site.py site [--stable dist] [--beta dist-beta] [--out _site]
     -> web/flash/* + stable/ (and beta/) each with its images and an ESP Web Tools manifest.json,
        channels.json, which the page reads to show the Stable / Beta picker and the display's updater reads
        (docs/PROTOCOL.md §5), notes.json, the release notes from CHANGELOG.md, which the display shows before
        installing an update (and the page shows for the selected version), and site.json ({app, repo} from
        forge.json) for the page's title and links. fonts/ gets main/montserrat.ttf and its license when the
        project has them (optional). With --emu <dir>, try/ gets a browser build of the firmware (web/emu/build's
        files: index.html, emu.js, emu.wasm, emu-page.js, and the settings page with emu-settings.js; not its
        folders) and channels.json says so ("try"): the page links to it.

The images stay separate parts on purpose: a single merged image would also overwrite the NVS
partition (Wi-Fi credentials, settings, the TLS certificate) with 0xFF on every update.

Local preview (Web Serial works on localhost):
  python3 tools/make_flasher_site.py dist && python3 tools/make_flasher_site.py site --stable dist
  python3 -m http.server -d _site 8000
"""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import forgecfg  # noqa: E402

CFG = forgecfg.load()
ROOT = CFG["_root"]
FAMILY = {"esp32s3": "ESP32-S3", "esp32": "ESP32", "esp32c3": "ESP32-C3", "esp32c6": "ESP32-C6",
          "esp32s2": "ESP32-S2", "esp32h2": "ESP32-H2", "esp32c2": "ESP32-C2", "esp32p4": "ESP32-P4"}


def git_version():
    try:
        return subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"],
                                       cwd=ROOT, text=True).strip()
    except Exception:
        return "dev"


def cmd_dist(a):
    version = a.version or git_version()
    with open(os.path.join(a.build, "flasher_args.json")) as f:
        fa = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    parts = []
    for offset, rel in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        name = os.path.basename(rel)
        if rel == fa["app"]["file"]:                       # the app gets the version in its name
            stem, ext = os.path.splitext(name)
            name = f"{stem}-{version}{ext}"
        shutil.copy2(os.path.join(a.build, rel), os.path.join(a.out, name))
        parts.append({"file": name, "offset": int(offset, 16)})
    info = {
        "chip": fa["extra_esptool_args"]["chip"],
        "version": version,
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "flash_settings": fa["flash_settings"],
        "parts": parts,
    }
    with open(os.path.join(a.out, "flash-parts.json"), "w") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    print(f"Release files in {a.out} ({version}):")
    for p in parts:
        print(f"  0x{p['offset']:06x}  {p['file']}")


def read_parts(src):
    """flash-parts.json, or for a folder without it, the standard file names (<app>-<version>.bin at 0x10000)."""
    path = os.path.join(src, "flash-parts.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    pre = CFG["app"] + "-"
    apps = [n for n in os.listdir(src) if n.startswith(pre) and n.endswith(".bin") and not n.endswith("-full.bin")]
    if len(apps) != 1:
        sys.exit(f"{src}: no flash-parts.json and {len(apps)} app images")
    return {
        "chip": CFG["chip"],
        "version": apps[0][len(pre):-len(".bin")],
        "built": datetime.datetime.fromtimestamp(os.path.getmtime(os.path.join(src, apps[0])),
                                                 datetime.timezone.utc).strftime("%Y-%m-%d"),
        "parts": [{"file": "bootloader.bin", "offset": 0x0},
                  {"file": "partition-table.bin", "offset": 0x8000},
                  {"file": apps[0], "offset": 0x10000}],
    }


def add_channel(out, name, src):
    info = read_parts(src)
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    for p in info["parts"]:
        shutil.copy2(os.path.join(src, p["file"]), os.path.join(d, p["file"]))
    manifest = {
        "name": CFG["app"] + (" (beta)" if name == "beta" else ""),
        "version": info["version"],
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": FAMILY[info["chip"]],
            "parts": [{"path": p["file"], "offset": p["offset"]} for p in info["parts"]],  # relative to the manifest
        }],
    }
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"  {name}: {info['version']} (built {info['built']})")
    return {"version": info["version"], "built": info["built"], "manifest": f"{name}/manifest.json"}


def read_changelog(path, keep=15, max_bytes=None):
    """CHANGELOG.md -> [{"version", "date", "notes": [str]}], newest first: the `keep` newest stable releases, and the
    release candidates newer than the newest stable one (older rc sections are repeated by their release's section).
    Counting rc sections in the 15 made a display a few releases behind miss stable notes. At most `max_bytes` of
    JSON (forge.json notes_max_bytes): the display reads notes.json into a fixed buffer (forge_ota).

    Sections start with "## vX.Y.Z" (optionally followed by " - YYYY-MM-DD" or " — YYYY-MM-DD"), items with
    "- ". Markdown emphasis and code marks are dropped: the display shows plain text."""
    max_bytes = max_bytes or CFG["notes_max_bytes"]
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8"):
        line = line.rstrip()
        m = re.match(r"^##\s+(v\d+\.\d+\.\d+\S*)(?:\s+[-\u2014\u2013]\s+(\d{4}-\d{2}-\d{2}))?", line)
        if m:
            out.append({"version": m.group(1), "date": m.group(2) or "", "notes": []})
        elif out and re.match(r"^\s*[-*]\s+", line):
            out[-1]["notes"].append(re.sub(r"^\s*[-*]\s+", "", line))
        elif out and out[-1]["notes"] and line.startswith("  ") and line.strip():
            out[-1]["notes"][-1] += " " + line.strip()          # continuation of the previous item
    for r in out:
        r["notes"] = [re.sub(r"[*_`#]", "", n).strip() for n in r["notes"]]
    kept, stable = [], 0
    for r in out:
        pre = "-" in r["version"]
        if pre and stable:                                   # an rc of an already released version
            continue
        if not pre:
            stable += 1
            if stable > keep:
                break
        if len(json.dumps({"releases": kept + [r]})) > max_bytes:
            break
        kept.append(r)
    return kept


def cmd_site(a):
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(os.path.join(ROOT, "web", "flash"), a.out)
    fonts = [n for n in ("montserrat.ttf", "montserrat-OFL.txt") if os.path.exists(os.path.join(ROOT, "main", n))]
    if fonts:                                                      # the display's font (SIL OFL), optional
        os.makedirs(os.path.join(a.out, "fonts"), exist_ok=True)
        for name in fonts:
            shutil.copy2(os.path.join(ROOT, "main", name), os.path.join(a.out, "fonts", name))
    with open(os.path.join(a.out, "site.json"), "w") as f:
        json.dump({"app": CFG["app"], "repo": CFG.get("repo", ""), "fonts": bool(fonts)}, f, indent=2)
        f.write("\n")
    print(f"Site in {a.out}:")
    if not a.stable and not a.beta:
        sys.exit("site: --stable and/or --beta")
    # A new project's first releases are candidates: Beta only, "stable": null until vX.Y.Z is tagged
    channels = {"stable": add_channel(a.out, "stable", a.stable) if a.stable else None, "beta": None}
    if a.beta:
        channels["beta"] = add_channel(a.out, "beta", a.beta)
    if a.emu:                                                      # the firmware in the browser (optional)
        os.makedirs(os.path.join(a.out, "try"), exist_ok=True)
        for name in ("index.html", "emu.js", "emu.wasm"):                # (required)
            if not os.path.isfile(os.path.join(a.emu, name)):
                sys.exit(f"site: --emu {a.emu} has no {name}")
        for name in sorted(os.listdir(a.emu)):                           # its files, not the object folders
            if os.path.isfile(os.path.join(a.emu, name)):
                shutil.copy2(os.path.join(a.emu, name), os.path.join(a.out, "try", name))
        channels["try"] = "try/"
        print(f"  try/: the firmware in the browser ({os.path.getsize(os.path.join(a.emu, 'emu.wasm')) // 1024} KB)")
    with open(os.path.join(a.out, "channels.json"), "w") as f:
        json.dump(channels, f, indent=2)
        f.write("\n")
    notes = read_changelog(a.changelog)
    with open(os.path.join(a.out, "notes.json"), "w", encoding="utf-8") as f:
        json.dump({"releases": notes}, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"  notes.json: {len(notes)} releases from {os.path.relpath(a.changelog, ROOT)}")


ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = ap.add_subparsers(dest="cmd", required=True)
d = sub.add_parser("dist", help="release files from a build")
d.add_argument("--build", default=forgecfg.path(CFG, CFG["build_dir"]), help="default: forge.json build_dir")
d.add_argument("--out", default=os.path.join(ROOT, "dist"))
d.add_argument("--version", default=None, help="default: git describe")
s = sub.add_parser("site", help="web flasher from release folders")
s.add_argument("--stable", default=None, help="folder with flash-parts.json (a dist folder or a downloaded release); none before the first stable release")
s.add_argument("--beta", default=None, help="same, for the beta channel (optional)")
s.add_argument("--out", default=os.path.join(ROOT, "_site"))
s.add_argument("--emu", default=None, help="web/emu/build: a browser build of the firmware, published as try/ (optional)")
s.add_argument("--changelog", default=os.path.join(ROOT, "CHANGELOG.md"), help="release notes source")
a = ap.parse_args()
{"dist": cmd_dist, "site": cmd_site}[a.cmd](a)
