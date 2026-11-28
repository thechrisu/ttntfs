# Enabling the FSKit module, per macOS release

The System Settings switch and the direct-edit route have swapped places
across releases, and neither is documented by Apple. This is the record of
what was actually verified, so the next release is checked against a matrix
instead of rediscovered from a user report. Re-verify on every new major
and minor release; the tripwires are the script's version gate (it refuses
to edit the list on 27+) and the app's permission fallback (a permission
error on the list means Apple protected it on that build too).

FSClient is the authority on whether the module is enabled — not the
Settings display, which keeps showing the module as off while it is
enabled and mounting on 26, and not `pluginkit -m`, which reports the
election, not the FSKit state. Verify with the app's status, or
`fskit/scripts/mount-test.sh`.

## Matrix

| macOS | Settings switch | App / script direct edit | Verified |
|---|---|---|---|
| 26.3 | Broken: LoginItems.appex talks to fskitd as an unentitled FSClient, fskitd answers EPERM (`Failed to enabled FSExtension: NSPOSIXErrorDomain Code=1`, one millisecond after every click, no `fskit_agent` activity). Any third-party client gets the same EPERM, team ID or not. | Works: add the bundle ID to `fskit_agent`'s list (`~/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist`), SIGKILL the agent (SIGTERM ignored, `launchctl kickstart` refused), launchd respawns it on demand. | 26.3 |
| 26.6.2 | Same as 26.3. | Same as 26.3. Verified a volume stays mounted across later launches of an already-registered bundle. | 2026-09-13 |
| 27 | Works, but only under Extensions → By Category → File System Extensions. The switch in the By App list does not stay on (reported on #2, 2026-11-20). | Refused: the list can no longer be read or written by an app without Apple's private entitlement. The app opens System Settings and waits for FSClient to report the module enabled (up to 5 minutes); `enable-module.sh` refuses and prints the same instruction. | #2, 2026-11-20 |
| 27.x / 28 | Unknown. Check the Settings switch first; if it works, the direct route is unnecessary. If the list is protected on a 26.x update, the app falls back to Settings on a permission error, and the script does the same. | — | — |

## Install order (all versions)

Copy the app to /Applications, launch it once (LaunchServices
re-registers the bundle with a new plugin UUID; pkd then kills every
running module instance, unmounting its volumes), then enable, then
mount. Launching an already-registered bundle later is harmless.

## Disabling / uninstalling

On 26 the app and `enable-module.sh --disable` remove the entry from
the list and restart the agent; on 27 only System Settings can switch
it off, and the uninstaller opens Settings and asks the user to do it
there before continuing. Dragging the app to the Trash is not a clean
uninstall on any version: the module stays in FSKit's enabled list and
the bundle stays registered, so Settings keeps listing an extension
that no longer exists.

## What is Apple-blocked, not fixable here

- The Settings toggle on 26: only Apple can entitle LoginItems.appex.
- Direct list access on 27: only Apple's private entitlement opens it.
- Mounting in CI without signing: see GAPS §4.5 (needs a paid account;
  the owner declined, 2026-11-25).

## Files

- App path: `fskit/NTFS/ModuleEnabler.swift` (version gate, permission
  fallback, read-back from FSClient, honest outcomes).
- Script path: `fskit/scripts/enable-module.sh` (+ `--disable`).
- Uninstaller: `fskit/scripts/uninstall.sh` (same list, same rules).
- Tests: `fskit/scripts/tests/test-enable-module.sh` (stub-based,
  runs on Linux; wired into `tools/pre-push.sh`).
