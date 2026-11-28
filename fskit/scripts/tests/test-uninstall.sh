#!/bin/bash
# Tests for uninstall.sh's two failure modes.
#
#   1. Its mounted-volume guard never fired. Step 1 piped lsof into
#      `grep -q`; grep exits at the first match, lsof takes SIGPIPE,
#      pipefail reports the pipeline failed, and the `|| continue` fired
#      even while a volume was being served. The uninstall then proceeded
#      under a mounted volume — the same pipefail guard bug enable-module.sh
#      had (c4ec674).
#   2. On macOS 27 it edited FSKit's enabled-module list directly, which
#      the OS now protects, so step 2 died with a permission error (or
#      wrote a file the OS ignores) halfway through the uninstall.
#
# These run against the real script with no side effects: nothing here
# unmounts, signals, or deletes anything. Static checks plus behaviour
# under a PATH of stubs, with a throwaway HOME.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT="$HERE/../uninstall.sh"
pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1" >&2; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

[ -r "$SCRIPT" ] || { echo "no uninstall.sh at $SCRIPT" >&2; exit 2; }

echo "uninstall.sh"

# --- 1. the guard must not pipe into grep -q ------------------------------
guard=$(awk '/^for pid in .*Contents\/Extensions\/NTFSExtension/,/^done$/' "$SCRIPT")
check "guard: does not pipe lsof into grep -q" \
      '! printf %s "$guard" | grep -qE "lsof[^|]*\|[[:space:]]*grep -q"'
check "guard: captures lsof output before matching" \
      'printf %s "$guard" | grep -qE "fds=\\\$\(lsof"'

# --- 2. step 2 must validate the plist shape ------------------------------
check "step 2: refuses a plist that is not a list" \
      'grep -q "isinstance(modules, list)" "$SCRIPT"'

# --- 3. behaviour: stub end-to-end runs of the real script ----------------
# Stubs: sw_vers reports the version in $1; pgrep returns a module pid only
# when SERVING=1; lsof prints a block-device fd line for that pid and then
# enough filler to fill the pipe buffer — the old `lsof | grep -q` guard
# only failed because grep exited early and lsof took SIGPIPE, so the
# filler is what makes the negative control honest. The stub has no
# explicit exit, so the SIGPIPE status (141) propagates under pipefail.
# pluginkit/pkill/mount succeed and do nothing; nothing is unmounted or
# deleted. The app bundle and all traces live under a throwaway HOME.
mkstub() { # $1 = macOS version
    STUB=$(mktemp -d)
    cat > "$STUB/sw_vers" <<EOF
#!/bin/bash
[ "\${1:-}" = "-productVersion" ] && echo "$1"
exit 0
EOF
    cat > "$STUB/pgrep" <<'EOF'
#!/bin/bash
case "$*" in
  *Contents/Extensions/NTFSExtension*) [ "${SERVING:-0}" = 1 ] && echo 12345 || exit 1 ;;
  *fskit_agent*) exit 1 ;;
esac
exit 1
EOF
    cat > "$STUB/lsof" <<'EOF'
#!/bin/bash
echo "NTFSExt 12345 user 10u REG 1,2 0 /dev/disk2"
seq 1 20000
EOF
    cat > "$STUB/pluginkit" <<'EOF'
#!/bin/bash
exit 0
EOF
    cat > "$STUB/pkill" <<'EOF'
#!/bin/bash
exit 1
EOF
    cat > "$STUB/mount" <<'EOF'
#!/bin/bash
exit 0
EOF
    chmod +x "$STUB/sw_vers" "$STUB/pgrep" "$STUB/lsof" "$STUB/pluginkit" "$STUB/pkill" "$STUB/mount"
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
run_e2e() { # $1 = stub dir, $2 = home dir, $3 = SERVING; runs with --yes
    _stub=$1; _home=$2; _serving=$3
    SERVING="$_serving" HOME="$_home" PATH="$_stub:$PATH" bash "$SCRIPT" --yes >"$_home/out.txt" 2>&1
    echo $?
}

mkstub 26.6.2
mkhome ch.techtag.ntfs.extension
rc=$(run_e2e "$STUB" "$THOME" 1)
if [ "$rc" = 1 ] && grep -q "serving a mounted volume" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = yes ]; then
    ok "guard: refuses under a served volume, list untouched"
else bad "guard: refuses under a served volume, list untouched (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

mkstub 26.6.2
mkhome ch.techtag.ntfs.extension
rc=$(run_e2e "$STUB" "$THOME" 0)
if [ "$rc" = 0 ] && grep -q "removed ch.techtag.ntfs.extension from the FSKit enabled list" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = no ]; then
    ok "26.6.2: removes the entry, finishes"
else bad "26.6.2: removes the entry, finishes (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

mkstub 27.0
mkhome ch.techtag.ntfs.extension
rc=$(run_e2e "$STUB" "$THOME" 0)
if [ "$rc" = 0 ] && grep -q "cannot be edited from a script" "$THOME/out.txt" && [ "$(plist_has_id "$PLIST_F")" = yes ]; then
    ok "27.0: skips the list edit, tells the user, finishes"
else bad "27.0: skips the list edit, tells the user, finishes (rc=$rc, out=$(cat "$THOME/out.txt" | tr '\n' '|'))"; fi
rm -rf "$STUB" "$THOME"

echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
