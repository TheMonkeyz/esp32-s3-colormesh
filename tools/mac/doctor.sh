#!/bin/bash
# What this Mac has and lacks for this project: one PASS / FAIL / INFO line per check, exit 1 on any FAIL. Changes
# nothing. Run it first when something doesn't work, and paste its output when asking for help.
#
#   bash tools/mac/doctor.sh            # everything
#   bash tools/mac/doctor.sh --board    # a missing board is a FAIL (not INFO)
#
# Versions come from forge.json (ESP-IDF, as CI) and tools/mac/setup.sh (where it installs them).
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cfg() { sed -n "s/^ *\"$1\": *\"\([^\"]*\)\".*/\1/p" "$REPO/forge.json" | head -1; }
IDF_VERSION=$(cfg idf)
IDF_DIR=$(bash "$REPO/tools/mac/setup.sh" --idf-dir)
BUILD_DIR=$(cfg build_dir); BUILD_DIR=${BUILD_DIR:-build}
APP=$(cfg app)
GH_REPO=$(cfg repo)
NEED_BOARD=0; [ "$1" = "--board" ] && NEED_BOARD=1
fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
info() { echo "INFO  $*"; }
have() { command -v "$1" >/dev/null 2>&1; }

echo "== Computer"
info "macOS $(sw_vers -productVersion 2>/dev/null || uname -sr), $(uname -m), shell $SHELL"
[ "$(uname -s)" = Darwin ] || info "not macOS: these checks are written for a Mac"
xcode-select -p >/dev/null 2>&1 && pass "Xcode command line tools ($(xcode-select -p))" \
  || fail "Xcode command line tools missing: xcode-select --install"
if have brew; then pass "Homebrew $(brew --version 2>/dev/null | head -1 | awk '{print $2}') at $(brew --prefix)"
else fail "Homebrew missing: see docs/MACOS.md, step 1"; fi
for t in git cmake ninja ccache dfu-util python3 node npm gh; do
  if have $t; then pass "$t: $($t --version 2>&1 | head -1)"
  else case $t in npm) pkg=node ;; python3) pkg=python ;; *) pkg=$t ;; esac; fail "$t missing: brew install $pkg"; fi
done
if have node; then
  major=$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null)
  [ "${major:-0}" -ge 20 ] && pass "Node.js $major is new enough (Playwright)" || fail "Node.js ${major:-?} is too old: brew upgrade node"
fi

echo "== ESP-IDF $IDF_VERSION"
if [ -f "$IDF_DIR/export.sh" ]; then
  v=$(git -C "$IDF_DIR" describe --tags 2>/dev/null)
  [ "$v" = "$IDF_VERSION" ] && pass "ESP-IDF $v at $IDF_DIR" || fail "ESP-IDF at $IDF_DIR is $v, expected $IDF_VERSION (forge.json, CI's)"
  # In a subshell: does export.sh work, and does its Python have what the tools need?
  out=$( ( . "$IDF_DIR/export.sh" >/tmp/idf_export.log 2>&1 || { echo "EXPORT-FAILED"; exit 0; }
           echo "IDFPY $(idf.py --version 2>&1 | tail -1)"
           echo "GCC $(xtensa-esp32s3-elf-gcc --version 2>/dev/null | head -1)"
           python -c 'import serial, esptool; print("PYMODS pyserial", serial.__version__, "esptool", esptool.__version__)' 2>&1 | tail -1
           python "$REPO/tools/devloop/flash_helper.py" ports 2>&1 | sed 's/^/PORT /' ) )
  if echo "$out" | grep -q EXPORT-FAILED; then
    fail "export.sh failed (last lines below); usually fixed by re-running: bash tools/mac/setup.sh"
    tail -5 /tmp/idf_export.log | sed 's/^/      /'
  else
    pass "export.sh works ($(echo "$out" | sed -n 's/^IDFPY //p'))"
    echo "$out" | grep -q '^GCC xtensa' && pass "xtensa toolchain: $(echo "$out" | sed -n 's/^GCC //p')" \
      || fail "xtensa-esp32s3-elf-gcc missing: bash tools/mac/setup.sh"
    echo "$out" | grep -q '^PYMODS' && pass "ESP-IDF Python has $(echo "$out" | sed -n 's/^PYMODS //p')" \
      || fail "ESP-IDF Python lacks pyserial / esptool: bash tools/mac/setup.sh"
    if echo "$out" | grep -q 'ESP32-S3 (the board)'; then
      pass "board connected: $(echo "$out" | grep 'the board' | sed 's/^PORT //' | head -1)"
    elif [ $NEED_BOARD = 1 ]; then
      fail "no ESP32-S3 on USB (a data cable? another port? docs/MACOS.md, 'No port')"
    else
      info "no ESP32-S3 on USB right now (fine unless you want to flash)"
    fi
  fi
else
  fail "ESP-IDF not found at $IDF_DIR: bash tools/mac/setup.sh (or set IDF_DIR)"
fi
if [ -n "$IDF_PATH" ]; then info "this shell has ESP-IDF loaded (IDF_PATH=$IDF_PATH)"
else info "this shell has not loaded ESP-IDF: run 'get_idf' (or . $IDF_DIR/export.sh) before building"; fi

echo "== Project ($REPO)"
cd "$REPO" || exit 1
br=$(git branch --show-current)
[ "$br" = main ] && fail "on branch main: work on your own branch (git switch -c <topic>)" || pass "branch $br"
info "remote: $(git remote get-url origin 2>/dev/null)"
[ -n "$(git config user.name)" ] && [ -n "$(git config user.email)" ] && pass "git user $(git config user.name) <$(git config user.email)>" \
  || fail "git user not set: git config --global user.name 'Your Name'; git config --global user.email you@example.com"
if have gh; then gh auth status >/dev/null 2>&1 && pass "GitHub CLI signed in ($(gh api user --jq .login 2>/dev/null))" \
  || fail "GitHub CLI not signed in: gh auth login"; fi
if [ -n "$GH_REPO" ] && have gh && gh auth status >/dev/null 2>&1; then
  perm=$(gh api "repos/$GH_REPO" --jq .permissions.push 2>/dev/null)
  [ "$perm" = true ] && pass "can push to $GH_REPO" || info "no push access to $GH_REPO (fine for a fork or a read-only checkout)"
fi
bad=$(git ls-files --eol 2>/dev/null | awk '$1=="i/lf" && $2=="w/crlf" && $NF !~ /\.bat$/ {print $NF}' | head -3)   # .bat: CRLF on purpose
[ -z "$bad" ] && pass "line endings: as in the repository" || fail "files checked out with CRLF ($bad): git config core.autocrlf false; git checkout -- ."
[ -d managed_components ] && pass "managed_components fetched (a firmware build ran)" \
  || info "managed_components/ missing: the first firmware build fetches them"
[ -f "$BUILD_DIR/$APP.bin" ] && info "firmware build: $BUILD_DIR/$APP.bin ($(date -r "$BUILD_DIR/$APP.bin" '+%Y-%m-%d %H:%M'))" \
  || info "no firmware build in $BUILD_DIR yet (docs/MACOS.md, 'Build')"
[ -f .devloop/stage/manifest.json ] && info "staged for flashing: .devloop/stage/ ($(date -r .devloop/stage/manifest.json '+%Y-%m-%d %H:%M'))" \
  || info "nothing staged in .devloop/stage yet"
[ -d tools/webtest/node_modules/@playwright/test ] && pass "webtest: npm packages installed" \
  || fail "webtest packages missing: (cd tools/webtest && npm ci && npx playwright install chromium)"
ls "$HOME/Library/Caches/ms-playwright"/chromium* >/dev/null 2>&1 && pass "Playwright Chromium installed" \
  || fail "Playwright's Chromium missing: (cd tools/webtest && npx playwright install chromium)"

echo "== Flash helper and display"
st=$(tr -d '\r\n' < .devloop/flash.status 2>/dev/null)
info "flash.status: ${st:-none (the helper never ran here)}"
if pgrep -f "tools/devloop/flash_helper.py" >/dev/null; then pass "flash helper running (pid $(pgrep -f tools/devloop/flash_helper.py | head -1))"
else info "flash helper not running (start it in its own terminal: get_idf; python tools/devloop/flash_helper.py)"; fi
ip=$(tr -d '\r\n' < .devloop/ip 2>/dev/null)
[ -z "$ip" ] && ip=$(grep -ho 'net: Connected, IP [0-9.]*' .devloop/serial_live.txt .devloop/serial_log.txt 2>/dev/null | tail -1 | awk '{print $4}')
if [ -n "$ip" ]; then
  code=$(curl -sk -m 5 -o /dev/null -w '%{http_code}' "https://$ip/api/info")
  if [ "$code" = 200 ]; then pass "display answers at https://$ip/ (HTTP $code, same network)"
  else fail "display at $ip doesn't answer from this Mac (HTTP $code): same Wi-Fi? macOS 'Local Network' permission for your terminal app (docs/MACOS.md)"; fi
else
  info "display address unknown yet (it appears in the log once the display is on Wi-Fi)"
fi

echo
[ $fails = 0 ] && echo "All checks passed." || echo "$fails check(s) failed: see docs/MACOS.md, 'Troubleshooting'."
[ $fails = 0 ]
