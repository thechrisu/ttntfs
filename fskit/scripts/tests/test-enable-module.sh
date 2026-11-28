#!/bin/bash
# Tests for enable-module.sh's two failure modes, both of which cost a mount.
#
#   1. It exited silently in the middle. `pluginkit -r` returns 1 for a stray
#      copy whose appex is gone -- a Trash copy, typically -- and under
#      `set -euo pipefail` a failing command inside the `| while` prune loop
#      killed the subshell and then the script, after the echo, with no error.
#      The module was left disabled while the run looked merely quiet (c4ec674).
#   2. Its mounted-volume guard never fired. It piped lsof into `grep -q`;
#      grep exits at the first match, lsof takes SIGPIPE, pipefail reports the
#      pipeline failed, and the `if` read false. The script then restarted
#      fskit_agent under two live volumes and FSKit dropped both (c4ec674).
#
# These run against the real script with no side effects: nothing here enables,
# disables, or signals anything. Static checks plus behaviour under a PATH of
# stubs, which is what makes the failure modes reproducible without hardware.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT="$HERE/../enable-module.sh"
pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1" >&2; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

[ -r "$SCRIPT" ] || { echo "no enable-module.sh at $SCRIPT" >&2; exit 2; }

echo "enable-module.sh"

# --- 1. the prune loop must not be able to abort the script ---------------
# Every command in it has to be explicitly best-effort; a bare invocation of a
# tool that can exit non-zero is the bug.
prune=$(awk '/unregistering stray copy/,/^    done/' "$SCRIPT")
check "prune: lsregister is best-effort" \
      '[ -n "$(printf %s "$prune" | grep -E "LSREGISTER.*(\|\| true|;[[:space:]]*true)")" ]'
check "prune: pluginkit -r is best-effort" \
      '[ -n "$(printf %s "$prune" | grep -E "pluginkit -r.*(\|\| true|;[[:space:]]*true)")" ]'

# --- 2. the guard must not pipe into grep -q ------------------------------
guard=$(awk '/^serving=/,/^fi$/' "$SCRIPT")
check "guard: does not pipe lsof into grep -q" \
      '! printf %s "$guard" | grep -qE "lsof[^|]*\|[[:space:]]*grep -q"'
check "guard: captures lsof output before matching" \
      'printf %s "$guard" | grep -qE "fds=\\\$\(lsof"'

# --- 3. it reports the state it actually reached --------------------------
# A silent early exit is the failure mode, so success must be asserted from the
# enabled list rather than from having reached the end of the script.
check "verifies the enabled list at the end" \
      'grep -q "is not in the enabled list\|is still in the enabled list" "$SCRIPT"'
check "prints the state it reached" \
      'grep -qE "is now \\\$\{?MODE" "$SCRIPT"'

# --- 4. behaviour: a stray copy whose appex is missing must not abort ------
# Reproduces bug 1 against the real prune loop, with stubs on PATH so nothing
# is touched: lsregister lists a stray path, pluginkit fails on it the way the
# real one does for a Trash copy.
STUB=$(mktemp -d)
STRAY="$STUB/Installed.app"
mkdir -p "$STRAY/Contents"          # deliberately no Contents/Extensions
cat > "$STUB/lsregister" <<EOF
#!/bin/bash
[ "\${1:-}" = "-dump" ] && echo "	path:	$STRAY (0x1234)"
exit 0
EOF
cat > "$STUB/pluginkit" <<'EOF'
#!/bin/bash
# -r on a bundle with no appex is exactly the case that returned 1
[ "${1:-}" = "-r" ] && exit 1
[ "${1:-}" = "-m" ] && { echo "+   ch.techtag.ntfs.extension(1.0)"; exit 0; }
exit 0
EOF
chmod +x "$STUB/lsregister" "$STUB/pluginkit"

# Run the real prune block with LSREGISTER pointed at the stub. Extracted whole
# (`if [ -x "$LSREGISTER" ] ... fi`) rather than by its body, so it is the
# script's own loop being exercised, not a paraphrase of it.
block=$(awk '/^if \[ -x "\$LSREGISTER" \]/,/^fi$/' "$SCRIPT" | sed "s|^LSREGISTER=.*|LSREGISTER=$STUB/lsregister|")
prune_rc=$(
  PATH="$STUB:$PATH" NTFS_APP="/nonexistent/Installed.app" bash -c '
    set -euo pipefail
    APP="/nonexistent/Installed.app"
    LSREGISTER='"$STUB"'/lsregister
    '"$block"'
    echo REACHED_END' 2>/dev/null | tail -1
)
check "prune loop survives a stray copy with no appex" '[ "$prune_rc" = REACHED_END ]'
# --- 5. the macOS 27 gate, and the 26 path end to end --------------------
# Full runs of the real script with stubs on PATH and a throwaway HOME,
# so the version gate and the happy path are exercised, not just grepped.
# Stubs: sw_vers reports $1, pgrep finds nothing, pluginkit reports the
# election state $2 for -m and succeeds for everything else.
mkstub() {
    STUB=$(mktemp -d)
    cat > "$STUB/sw_vers" <<EOF
#!/bin/bash
[ "\${1:-}" = "-productVersion" ] && echo "$1"
exit 0
EOF
    cat > "$STUB/pgrep" <<'EOF'
#!/bin/bash
exit 1
EOF
    cat > "$STUB/pluginkit" <<EOF
#!/bin/bash
[ "\${1:-}" = "-m" ] && { echo "$2   ch.techtag.ntfs.extension(1.0)"; exit 0; }
exit 0
EOF
    chmod +x "$STUB/sw_vers" "$STUB/pgrep" "$STUB/pluginkit"
}
# shellcheck disable=SC2120
# mkhome's callers pass zero or more entries; $@ is the API.
mkhome() { # each arg becomes one entry in the initial enabled list
    THOME=$(mktemp -d)
    PLIST_F="$THOME/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist"
    mkdir -p "$(dirname "$PLIST_F")"
    python3 - "$PLIST_F" "$@" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump(sys.argv[2:], f)
PY
}
plist_has_id() {
    python3 -c "
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    m = plistlib.load(f)
print('yes' if isinstance(m, list) and sys.argv[2] in m else 'no')
" "$1" ch.techtag.ntfs.extension
}
run_e2e() { # $1 = stub dir, $2 = home dir; remaining args passed to the script
    _stub=$1; _home=$2; shift 2
    HOME="$_home" PATH="$_stub:$PATH" bash "$SCRIPT" "$@" >"$_home/out.txt" 2>&1
    echo $?
}

check "gate: refuses on macOS 27" \
      'grep -q "sw_vers -productVersion" "$SCRIPT" && grep -q "\-ge 27" "$SCRIPT"'

mkstub 26.6.2 +
mkhome
rc=$(run_e2e "$STUB" "$THOME")
if [ "$rc" = 0 ] && grep -q "is now enabled" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = yes ]; then
    ok "26.6.2: enable end to end"
else bad "26.6.2: enable end to end (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

mkstub 27.0 +
mkhome
rc=$(run_e2e "$STUB" "$THOME")
if [ "$rc" = 2 ] && grep -q "System Settings" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = no ]; then
    ok "27.0: refuses with the Settings route, list untouched"
else bad "27.0: refuses with the Settings route, list untouched (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

# No sw_vers at all (Linux): the gate must not fire.
STUB=$(mktemp -d)
cat > "$STUB/pgrep" <<'EOF'
#!/bin/bash
exit 1
EOF
cat > "$STUB/pluginkit" <<'EOF'
#!/bin/bash
[ "${1:-}" = "-m" ] && { echo "+   ch.techtag.ntfs.extension(1.0)"; exit 0; }
exit 0
EOF
chmod +x "$STUB/pgrep" "$STUB/pluginkit"
mkhome
rc=$(run_e2e "$STUB" "$THOME")
if [ "$rc" = 0 ] && [ "$(plist_has_id "$PLIST_F")" = yes ]; then
    ok "no sw_vers: gate does not fire"
else bad "no sw_vers: gate does not fire (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

# --- 6. a protected list falls back to Settings, not a traceback --------
if [ "$(id -u)" -eq 0 ]; then
    ok "protected list: skipped as root (permission bits do not bind root)"
else
    mkstub 26.6.2 +
    mkhome
    chmod 444 "$PLIST_F"
    rc=$(run_e2e "$STUB" "$THOME")
    if [ "$rc" = 2 ] && grep -q "System Settings" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = no ] && [ ! -e "$PLIST_F.bak" ]; then
        ok "protected list: Settings route, list and backup untouched"
    else bad "protected list: Settings route, list and backup untouched (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
    chmod 644 "$PLIST_F"
    rm -rf "$STUB" "$THOME"
fi

# --- 7. pluginkit disagreeing with the list is a failure, not a pass -----
mkstub 26.6.2 -
mkhome
rc=$(run_e2e "$STUB" "$THOME")
if [ "$rc" = 1 ] && grep -q "FAILED: pluginkit still reports" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = yes ]; then
    ok "pluginkit '-': fails loudly, list updated"
else bad "pluginkit '-': fails loudly, list updated (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

# --- 8. a plist that is not a list is refused, not mangled ----------------
mkstub 26.6.2 +
THOME=$(mktemp -d)
PLIST_F="$THOME/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist"
mkdir -p "$(dirname "$PLIST_F")"
python3 - "$PLIST_F" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({"modules": []}, f)
PY
rc=$(run_e2e "$STUB" "$THOME")
SHAPE=$(python3 -c "
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    print(type(plistlib.load(f)).__name__)
" "$PLIST_F")
if [ "$rc" = 1 ] && grep -q "not a list" "$THOME/out.txt" && [ "$SHAPE" = dict ]; then
    ok "dict plist: refused, file unchanged"
else bad "dict plist: refused, file unchanged (rc=$rc, shape=$SHAPE, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
