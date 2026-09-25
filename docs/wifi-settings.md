# Native Wi-Fi settings

Fork additions on top of upstream: a combined details/settings page for Wi-Fi
profiles, used from the Wi-Fi rows instead of `nm-connection-editor`.

## Implementation

New files are in `src/actions/` and `src/sections/`:

- `connection-settings.c`: combined details/settings page and Forget confirmation.
  A small field registry tracks original values, dirty styling and per-field
  undo. Live labels update independently of draft controls. Saving rebases the
  form without navigating away; libnm versions are adopted only when the cached
  settings match the saved snapshot. Drafts are normalized before saving so
  derived properties (such as legacy MAC randomization) match NetworkManager.
- The settings section of `nm-sidebar.css`: compact form styling scoped to these pages.
  The Connection Information overview and per-profile diagnostics share the
  same selectable label/value rows, with 12px text and minimal vertical padding.
- `profile-model.c`: validates IP fields on an editor-owned clone. Untouched
  addresses, routes and other settings keep their existing attributes.
- `profile-save.c`: retrieves and merges saved secrets, persists with Update2,
  and checks for concurrent changes. iwd can change a profile's version during
  GetSecrets, so the post-read check also compares the non-secret settings.
- Call sites in `wifi.c`, `connection_info.c` and `app.c` connect the pages to the
  Wi-Fi rows and the navigation view. This also fixes an upstream scroll-adjustment
  notify callback that used the wrong signature and could crash on allocation.

Wi-Fi settings/add pages are native. The upstream external editor remains available
for VPN profiles. Existing EAP methods/certificates, custom routes,
and settings without a field in the form are preserved. Save does not reconnect
an active network. Back discards the draft. A credentials-read failure prevents
saving rather than risking removal of a saved secret.

## Layout and editability

- Connect/Disconnect and Auto-connect share one row. Only explicit Save persists
  a switch, text or dropdown edit. The fixed bottom bar appears when dirty, with
  a changed-field count, Revert all and Save changes. Each edited field also has
  its own undo button; reverting to the original value clears the dirty state.
- Names, replacement password, hidden SSID, metering and common MAC policies are
  compact editable rows. A blank password retains the saved secret. Existing
  custom MAC addresses remain available as an option without being overwritten.
  The MAC dropdown shows policy names without a separate Default entry. Its
  inherited value is the `default_wifi_mac` meson option; set it to NetworkManager's
  global `wifi.cloned-mac-address` (the NixOS package passes
  `networking.networkmanager.wifi.macAddress`, shown as Stable per network for `stable-ssid`).
  Saving other fields leaves an inherited MAC policy unset; changing the policy
  stores an explicit per-network override.
  With iwd, `General.AddressRandomization=network` implements the default and
  enables NM's mirrored `AlwaysRandomizeAddress`/`AddressOverride` options.
  Unsupported Keep current/Stable per profile choices are omitted, and Device
  address writes the compatible adapter's permanent MAC as a literal override.
  Legacy unsupported keywords resolve to the global policy actually used by
  iwd. The wpa_supplicant backend retains the full NetworkManager policy list.
- IPv4/IPv6 expanders summarize saved method and DNS policy. Addresses and DNS
  entered there are saved overrides, not the currently assigned values.
- Live IPs, gateways and DNS are selectable text below the form. Device state,
  hardware address, driver, BSSID, radio, routes and UUID belong in diagnostics.
  Existing authentication methods, certificates and BSSID/band locks are displayed
  there and retained; the form does not implement every NetworkManager option.

New enterprise connections support PEAP/MSCHAPv2 and TTLS/PAP with server-domain
and CA-certificate validation. Existing enterprise methods and certificates are
preserved while identity, password and server-domain fields can be edited.

## Verification

`meson test -C build` runs the isolated profile-model regression test.

For the live libnm persistence check, run `tests/live-save.sh` from an active
desktop session with permission to manage NetworkManager profiles. On NixOS, enter
the build environment first: `nix develop ~/nixos-conf#nm-sidebar`.
It creates temporary **disconnected, autoconnect-disabled** profiles with dummy
credentials, tests WPA and enterprise password preservation/replacement and
conflict rejection, and deletes each profile afterward. It never activates them.
If the test process is forcibly terminated, remove any `nm-sidebar-test-*`
profile it left behind using the sidebar's saved-network details page.

Add `--ui` to exercise the actual form on a temporary disconnected profile
(requires a graphical session; no test window is shown). It checks draft isolation,
per-field undo, Revert all, conditional Save visibility, validation, repeated saves,
password-input reset, disabled-IP normalization and MAC policy display/preservation
for both custom addresses and inherited defaults.

UI smoke check: toggle the sidebar, open a connected row, edit/revert a field,
expand IP settings and diagnostics, and open/cancel Forget. Verify Wi-Fi stays
connected.
