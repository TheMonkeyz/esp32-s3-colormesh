#!/bin/bash
# Sets up a Mac for this project: the tools from Homebrew, ESP-IDF at forge.json's version (the firmware; the same as
# CI) and the settings-page tests (Node + Playwright). Safe to run again: it skips what is already there. Ends with
# tools/mac/doctor.sh. Details and troubleshooting: docs/MACOS.md.
#
#   bash tools/mac/setup.sh             # everything (~30-60 min the first time, ~3 GB)
#   bash tools/mac/setup.sh --idf-dir   # only print where ESP-IDF goes (for ". <dir>/export.sh" in scripts)
#
# Needs: Xcode command line tools and Homebrew (the script says how if they're missing; both ask for your password,
# so it doesn't install them itself).
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
# (sed, not python3: on a fresh Mac python3 is a stub that asks for the command line tools)
cfg() { sed -n "s/^ *\"$1\": *\"\([^\"]*\)\".*/\1/p" "$REPO/forge.json" | head -1; }
IDF_VERSION=$(cfg idf); IDF_VERSION=${IDF_VERSION:-v5.5.4}
CHIP=$(cfg chip); CHIP=${CHIP:-esp32s3}
IDF_DIR=${IDF_DIR:-$HOME/esp/esp-idf-$IDF_VERSION}
[ "$1" = --idf-dir ] && { echo "$IDF_DIR"; exit 0; }
step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
stop() { printf '\n\033[1;31m%s\033[0m\n' "$*"; exit 1; }

[ "$(uname -s)" = Darwin ] || echo "(not macOS: carrying on, but this script is written for a Mac)"

step "Xcode command line tools"
if ! xcode-select -p >/dev/null 2>&1; then
  xcode-select --install || true
  stop "A window asks to install the command line tools: accept, wait for it to finish, then run this script again."
fi
echo "ok: $(xcode-select -p)"

step "Homebrew"
if ! command -v brew >/dev/null; then
  for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x $b ] && eval "$($b shellenv)" && break; done
fi
if ! command -v brew >/dev/null; then
  stop 'Homebrew is missing. Install it (it asks for your password), then run this script again:
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
and follow its "Next steps" (two lines that add brew to your shell).'
fi
echo "ok: $(brew --version | head -1)"

step "Tools from Homebrew (git cmake ninja dfu-util ccache python node gh)"
# ESP-IDF's macOS prerequisites (docs/en/get-started/linux-macos-setup.rst: cmake ninja dfu-util, ccache recommended,
# Python 3) + the project's. One at a time: a package already installed another way (a link conflict) must not stop
# the rest; doctor.sh says at the end if a tool is really missing.
for pkg in git cmake ninja dfu-util ccache python node gh; do
  brew install "$pkg" || echo "(brew had a problem with $pkg: see above; carrying on, doctor.sh checks it at the end)"
done

step "ESP-IDF $IDF_VERSION in $IDF_DIR"
if [ ! -f "$IDF_DIR/export.sh" ]; then
  mkdir -p "$(dirname "$IDF_DIR")"
  git clone --depth 1 --branch "$IDF_VERSION" --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
else
  echo "already there ($(git -C "$IDF_DIR" describe --tags 2>/dev/null))"
fi
# Uses the first python3 on the PATH: Homebrew's (the tested one)
PATH="$(brew --prefix)/bin:$PATH" "$IDF_DIR/install.sh" "$CHIP"

step "The 'get_idf' command (loads ESP-IDF in a terminal)"
RC="$HOME/.zshrc"; [ "${SHELL##*/}" = bash ] && RC="$HOME/.bash_profile"
if ! grep -q 'alias get_idf=' "$RC" 2>/dev/null; then
  printf '\n# ESP-IDF %s (tools/mac/setup.sh)\nalias get_idf=". %s/export.sh"\n' "$IDF_VERSION" "$IDF_DIR" >> "$RC"
  echo "added to $RC: alias get_idf (open a new terminal for it)"
else
  echo "already in $RC"
fi

step "Settings page tests (tools/webtest: Playwright + Chromium)"
(cd "$REPO/tools/webtest" && npm ci && npx playwright install chromium)

step "Git settings for this checkout"
git -C "$REPO" config core.autocrlf false        # the repository's LF stays LF (.gitattributes)
echo ok

step "Check (tools/mac/doctor.sh)"
bash "$REPO/tools/mac/doctor.sh" || true
printf '\nNext: open a new terminal, then follow docs/MACOS.md from "Build".\n'
