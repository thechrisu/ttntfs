#!/bin/bash
# Enable (or disable) the TT NTFS Native FSKit module without System Settings.
#
# On macOS 26.3 the File System Extensions toggle in System Settings never
# turns on for a third-party module: the settings host (LoginItems.appex) talks
# to fskitd as an unentitled FSClient and fskitd answers EPERM
# ("Failed to enabled FSExtension: Error Domain=NSPOSIXErrorDomain Code=1").
# The per-user fskit_agent keeps the real enabled list in
#   ~/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist
# and reads it only at start, so: edit the list, SIGKILL the agent (SIGTERM is
# ignored, launchctl kickstart is refused), launchd respawns it on demand.
#
#   fskit/scripts/enable-module.sh            # enable (also prunes stray copies)
#   fskit/scripts/enable-module.sh --disable  # remove from the list
#
# Exit codes: 0 done, 1 failed (see the message — the list is left as it
# was), 2 the list cannot be edited on this macOS version (see below).
#
# macOS 27 closes the direct-edit route: the list can no longer be read
# or written by an app without Apple's private entitlement, so this
# script refuses there and prints the System Settings route instead of
# failing with a traceback (exit 2). The same fallback is taken when the
# list is not readable/writable, since Apple could extend the
# protection to a 26.x update. sw_vers is absent off macOS (the tests
# run on Linux), so a missing sw_vers means "not gated".
#
# Order matters after a reinstall: copy the app, LAUNCH IT ONCE (LaunchServices
# re-registers the bundle on first launch), then run this script. The agent
# caches the extension's plugin UUID; every re-registration (install, first
# launch, lsregister -f) changes it and pkd kills any running module instance,
# which unmounts every ttntfs volume. Unregistering the old bundle also drops
# the identifier from the enabled list, so the script must run again anyway.
set -euo pipefail

ID=${NTFS_MODULE_ID:-ch.techtag.ntfs.extension}
PLIST="$HOME/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist"
MODE=enable
[ "${1:-}" = "--disable" ] && MODE=disable

SETTINGS_HINT="this macOS version enables third-party file systems only in System Settings → General → Login Items & Extensions. Under Extensions, choose By Category, then File System Extensions, and turn TT NTFS Native on there; the switch in the By App list does not stay on."

# Version gate: on 27+ the list is unreadable/unwritable without Apple's
# private entitlement, so editing it here would fail with a permission
# error — or, worse, write a file the OS ignores. Refuse with the
# instruction, like the app's ModuleEnabler does.
if command -v sw_vers >/dev/null 2>&1; then
    MACOS_MAJOR=$(sw_vers -productVersion 2>/dev/null | cut -d. -f1)
    case "$MACOS_MAJOR" in ''|*[!0-9]*) MACOS_MAJOR=0 ;; esac
    if [ "$MACOS_MAJOR" -ge 27 ]; then
        echo "macOS $MACOS_MAJOR: $SETTINGS_HINT" >&2
        [ "$MODE" = disable ] && echo "switch it off there; this script cannot remove the entry on this version." >&2
        exit 2
    fi
fi

# Refuse while one of our volumes is mounted: restarting fskit_agent drops it.
# A mounted ttntfs volume looks like plain "ntfs ... fskit" in `mount`, which is
# indistinguishable from Apple's own driver on macOS 26, and the module process
# keeps running indefinitely after an unmount -- so neither is a usable signal.
# What does track a live volume is the block device: the module holds one open
# fd on /dev/diskN while serving, and none when idle.
serving=
# Capture, then match. Piping into `grep -q` is what broke this guard before:
# grep exits at the first match, lsof takes SIGPIPE, and `set -o pipefail` turns
# that into a failed pipeline -- so the `if` read false and the script cheerfully
# restarted fskit_agent out from under a mounted volume (seen 2026-09-13, it
# unmounted the user's disk).
for pid in $(pgrep -f 'Contents/Extensions/NTFSExtension' 2>/dev/null); do
    fds=$(lsof -p "$pid" 2>/dev/null || true)
    case "$fds" in *'/dev/disk'*|*'/dev/rdisk'*) serving=$pid; break;; esac
done
if [ -n "$serving" ]; then
    echo "a ttntfs volume is mounted (module pid $serving holds a block device open);" >&2
    echo "restarting fskit_agent would drop it. Unmount first:" >&2
    mount | grep -E '\(ntfs[,)]' | sed 's/^/  /' >&2
    echo "  diskutil unmount /dev/diskNsM" >&2
    exit 1
fi
[ -f "$PLIST" ] || { echo "no $PLIST (open System Settings > Login Items & Extensions once)" >&2; exit 1; }

# Permission fallback: if Apple protected the list on this build too,
# the write below dies with a bare Python traceback under `set -e`.
# Check first and fall back to the Settings route with the instruction,
# like the app does on a permission error. The directory must be
# writable too — the backup copy goes there.
if [ ! -r "$PLIST" ] || [ ! -w "$PLIST" ] || [ ! -w "$(dirname "$PLIST")" ]; then
    echo "cannot update the FSKit module list (permission denied): $SETTINGS_HINT" >&2
    exit 2
fi

# Prune stray registrations. LaunchServices registers every copy of the bundle it
# sees, including Xcode's build output, and System Settings then lists the
# extension twice. Both the enabled list and the pluginkit election name the
# bundle ID, not a path, so with two copies registered it is undefined which one
# fskit_agent launches -- and if that is a build directory that later gets
# deleted, probes fail and the disk falls back to Apple's read-only driver.
APP=${NTFS_APP:-/Applications/TT NTFS Native.app}
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [ -x "$LSREGISTER" ]; then
    APP_NAME=$(basename "$APP")
    "$LSREGISTER" -dump 2>/dev/null \
        | sed -n "s|^[[:space:]]*path:[[:space:]]*\(/.*/${APP_NAME}\) (0x[0-9a-f]*)\$|\1|p" \
        | sort -u | while IFS= read -r found; do
        # /private/tmp and /tmp are the same place; compare canonical paths.
        [ "$(cd "$found" 2>/dev/null && pwd -P)" = "$(cd "$APP" 2>/dev/null && pwd -P)" ] && continue
        echo "unregistering stray copy: $found"
        # Best effort, and it must stay that way: pluginkit -r exits 1 for a copy
        # whose appex is gone or was never registered -- a Trash copy, typically --
        # and under `set -e` inside this `| while` subshell that killed the whole
        # script right here, after the echo, silently, leaving the module NOT
        # enabled while the run looked like it had merely been quiet.
        "$LSREGISTER" -u "$found" >/dev/null 2>&1 || true
        pluginkit -r "$found/Contents/Extensions/NTFSExtension.appex" >/dev/null 2>&1 || true
    done
fi

cp "$PLIST" "$PLIST.bak"
python3 - "$PLIST" "$ID" "$MODE" <<'PY'
import plistlib, sys
path, ident, mode = sys.argv[1:4]
with open(path, 'rb') as f:
    modules = plistlib.load(f)
if not isinstance(modules, list):
    print(f"error: {path} is not a list of module IDs (found {type(modules).__name__}); refusing to edit it", file=sys.stderr)
    sys.exit(1)
if mode == 'enable' and ident not in modules:
    modules.append(ident)
if mode == 'disable':
    modules = [m for m in modules if m != ident]
with open(path, 'wb') as f:
    plistlib.dump(modules, f)
print('enabled modules:', ', '.join(modules))
PY

PID=$(pgrep -x fskit_agent || true)
if [ -n "$PID" ]; then
    kill -9 "$PID"
    echo "fskit_agent $PID killed; launchd respawns it on the next FSKit request"
fi
sleep 1
# pluginkit election is a separate switch; keep it in step.
if [ "$MODE" = enable ]; then pluginkit -e use -i "$ID"; else pluginkit -e ignore -i "$ID"; fi
STATE=$(pluginkit -m -i "$ID" | cut -c1-1)
echo "pluginkit: ${STATE:-?} $ID   (+ enabled, - disabled)"

# The state above is printed either way; make it a check too. A `-`
# after enabling (or `+` after disabling) means the election did not
# take, and exiting 0 would claim success while the module stays off.
if [ "$MODE" = enable ] && [ "${STATE:-?}" = "-" ]; then
    echo "FAILED: pluginkit still reports $ID as disabled" >&2
    exit 1
fi
if [ "$MODE" = disable ] && [ "${STATE:-?}" = "+" ]; then
    echo "FAILED: pluginkit still reports $ID as enabled" >&2
    exit 1
fi

# Say what the state actually is rather than what was attempted. A silent early
# exit once left the module disabled and the run looked successful.
LISTED=$(python3 -c "
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    modules = plistlib.load(f)
print('yes' if isinstance(modules, list) and sys.argv[2] in modules else 'no')
" "$PLIST" "$ID")
if [ "$MODE" = enable ] && [ "$LISTED" != yes ]; then
    echo "FAILED: $ID is not in the enabled list" >&2
    exit 1
fi
if [ "$MODE" = disable ] && [ "$LISTED" = yes ]; then
    echo "FAILED: $ID is still in the enabled list" >&2
    exit 1
fi
echo "$ID is now ${MODE}d"
echo "verify:  fskit/scripts/mount-test.sh"
