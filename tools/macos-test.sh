#!/bin/bash
# macos-test.sh - the macOS test of Conker: Recomped & Reloaded on an Apple Silicon Mac: builds the game from your own
# disc image, starts it, and packs the logs to send back. It's a test: expect it to stop somewhere, the logs are the
# point. The logs hold no game files, and nothing of the game leaves this Mac.
#
#   bash macos-test.sh /path/to/Conker.iso   first run: download, extract the disc, build (an hour or more), play
#   bash macos-test.sh                        later runs: update to the newest test, rebuild what changed, play
#   bash macos-test.sh --debug                as a later run, with Vulkan's validation layer, and fn+F3 in the game
#                                             saves a frame's render targets into the logs (slower; for a problem)
#
# Needs: macOS on Apple Silicon, Xcode's command line tools, Homebrew (it says how to get them if missing).
# Everything goes in ~/ConkerMacTest. The logs end up in ~/Desktop/conker-macos-logs-<date>.zip, also when it fails.
# env: JOBS (parallel compiles, default 3 for an 8 GB Mac), BRANCH (default macos)
set -uo pipefail

DEBUG=0
ISO=""
for arg in "$@"; do
    case "$arg" in
        --debug) DEBUG=1 ;;
        *) ISO="$arg" ;;
    esac
done
TOP="$HOME/ConkerMacTest"
SRC="$TOP/live-and-recompiled"
BUILD="$SRC/out/build/macos"
LOGS="$TOP/logs"
JOBS="${JOBS:-3}"
BRANCH="${BRANCH:-macos}"
REPO="https://github.com/Mabec-games/live-and-recompiled.git"
STAMP="$(date +%Y%m%d-%H%M%S)"
ZIP="$HOME/Desktop/conker-macos-logs-$STAMP.zip"

mkdir -p "$TOP" "$LOGS"
rm -f "$LOGS"/*.log "$LOGS"/*.txt
exec > >(tee "$LOGS/script.log") 2>&1

step() { echo; echo "==== $*"; }

# the logs, whatever happened: this script's, each step's, the game's own, crash reports, the machine
pack_logs() {
    step "Packing the logs"
    {
        echo "date: $(date)"
        sw_vers 2>/dev/null
        echo "arch: $(uname -m)"
        echo "chip: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
        echo "memory: $(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 )) GB"
        echo "xcode: $(xcode-select -p 2>/dev/null) $(clang --version 2>/dev/null | head -1)"
        echo "cmake: $(cmake --version 2>/dev/null | head -1)"
        echo "commit: $(git -C "$SRC" log -1 --format='%h %s' 2>/dev/null)"
        command -v brew >/dev/null && brew list --versions molten-vk vulkan-loader 2>/dev/null
    } > "$LOGS/system.txt"
    command -v vulkaninfo >/dev/null && vulkaninfo --summary > "$LOGS/vulkaninfo.txt" 2>&1
    rm -rf "$LOGS/game" "$LOGS/crash" "$LOGS/capture"
    [ -d "$BUILD/logs" ] && cp -R "$BUILD/logs" "$LOGS/game"
    # fn+F3's frame captures from this run (--debug): the render targets as pictures, and the frame's draws
    [ $DEBUG -eq 1 ] && [ -d "$BUILD/cap" ] && cp -R "$BUILD/cap" "$LOGS/capture"
    [ -f "$BUILD/settings.toml" ] && cp "$BUILD/settings.toml" "$LOGS/"
    mkdir -p "$LOGS/crash"
    # macOS's crash reports for the game from the last 10 hours
    find "$HOME/Library/Logs/DiagnosticReports" -name 'Game*' -mmin -600 -exec cp {} "$LOGS/crash/" \; 2>/dev/null
    (cd "$TOP" && ditto -c -k --keepParent logs "$ZIP") && echo "Logs: $ZIP" ||
        echo "Couldn't make the zip: the logs are in $LOGS"
    echo "Please attach that zip to the GitHub issue (or send it), with a line on what you saw."
}
trap pack_logs EXIT

fail() { echo; echo "STOPPED: $*"; exit 1; }

step "Checking this Mac"
[ "$(uname -s)" = Darwin ] || fail "this is for macOS"
[ "$(uname -m)" = arm64 ] || echo "warning: this test is for Apple Silicon; this Mac is $(uname -m)"
xcode-select -p >/dev/null 2>&1 ||
    fail "Xcode's command line tools are missing: run   xcode-select --install   then this script again"
if ! command -v brew >/dev/null; then
    [ -x /opt/homebrew/bin/brew ] && eval "$(/opt/homebrew/bin/brew shellenv)"
fi
command -v brew >/dev/null ||
    fail "Homebrew is missing: install it from https://brew.sh (one command there), then run this script again"
BREW="$(brew --prefix)"

step "Installing the build tools and Vulkan for Metal (MoltenVK) with Homebrew"
PKGS="cmake ninja git molten-vk vulkan-loader vulkan-tools"
[ $DEBUG -eq 1 ] && PKGS="$PKGS vulkan-validationlayers"
brew install $PKGS 2>&1 | tee "$LOGS/brew.log" ||
    fail "Homebrew couldn't install the tools (brew.log)"

step "Getting the test ($BRANCH)"
if [ -d "$SRC/.git" ]; then
    git -C "$SRC" fetch --depth 1 origin "$BRANCH" && git -C "$SRC" reset --hard FETCH_HEAD ||
        fail "couldn't update the source"
else
    git clone --depth 1 --branch "$BRANCH" "$REPO" "$SRC" || fail "couldn't download the source"
fi
git -C "$SRC" log -1 --format='test version: %h %s'

if [ ! -f "$SRC/assets/default.xbe" ]; then
    step "Extracting the disc image"
    [ -n "$ISO" ] || fail "the first run needs your disc image:   bash macos-test.sh /path/to/Conker.iso"
    [ -f "$ISO" ] || fail "no such file: $ISO"
    XISO="$(command -v extract-xiso || true)"
    if [ -z "$XISO" ]; then
        # the disc's file system reader, built once from its source
        XSRC="$TOP/extract-xiso"
        [ -d "$XSRC/.git" ] || git clone --depth 1 https://github.com/XboxDev/extract-xiso.git "$XSRC" ||
            fail "couldn't download extract-xiso"
        cmake -S "$XSRC" -B "$XSRC/build" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
            cmake --build "$XSRC/build" > "$LOGS/extract-xiso-build.log" 2>&1 || fail "couldn't build extract-xiso"
        XISO="$XSRC/build/extract-xiso"
    fi
    mkdir -p "$SRC/assets"
    # options before the image: macOS's getopt stops at the first operand (Linux's moves them), so a -d after it was
    # read as a second file
    "$XISO" -q -x -d "$SRC/assets" "$ISO" > "$LOGS/extract.log" 2>&1 || fail "couldn't extract the disc image"
    [ -f "$SRC/assets/default.xbe" ] || fail "the disc image has no default.xbe: is it Conker: Live & Reloaded (USA)?"
fi

step "Configuring"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$LOGS/configure.log" 2>&1 ||
    fail "configuring failed (configure.log)"

step "Building the game (the first time takes an hour or more; $JOBS at a time)"
cmake --build "$BUILD" --target Game -- -j "$JOBS" -k 0 > "$LOGS/build.log" 2>&1
BUILT=$?
grep -E "error:|FAILED:" "$LOGS/build.log" > "$LOGS/build-errors.txt"
[ $BUILT -eq 0 ] || fail "the build failed: $(grep -c 'FAILED:' "$LOGS/build.log") file(s), see build-errors.txt"

step "Starting the game"
touch "$BUILD/portable.txt"  # its settings and logs beside it, in $BUILD
rm -rf "$BUILD/cap"
echo "Play a little (the pub menu, a level), then quit. If it doesn't start or crashes, that's fine: the logs show why."
echo "Also press F1 once: say whether the settings menu shows up (it's drawn apart from the game)."
[ $DEBUG -eq 1 ] && echo "Debug run: once the pub menu should be up, press fn+F3 once (it saves that frame), wait 10 s, quit."
(
    cd "$BUILD" || exit 1
    # Homebrew's Vulkan loader and MoltenVK, which the game opens by name. Exported here and the game started
    # directly: macOS drops DYLD_ variables on the way through its own programs (env, a new shell)
    export DYLD_FALLBACK_LIBRARY_PATH="$BREW/lib:/usr/local/lib:/usr/lib"
    MVK_ICD="$BREW/etc/vulkan/icd.d/MoltenVK_icd.json"
    [ -f "$MVK_ICD" ] && export VK_DRIVER_FILES="$MVK_ICD"
    export XBE_VERBOSE=1  # warnings too, with their threads: a test wants all of it
    if [ $DEBUG -eq 1 ]; then
        export XBE_VULKAN_DEBUG=1 XBE_CAPTURE_DIR=cap
        export VK_LAYER_PATH="$BREW/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d"
    fi
    exec ./Game
) > "$LOGS/run.log" 2>&1
echo "The game exited with code $?"
