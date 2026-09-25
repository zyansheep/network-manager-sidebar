#!/usr/bin/env bash
# Save WPA/enterprise profiles through libnm and check secrets are preserved or
# replaced. Creates disconnected, autoconnect-disabled nm-sidebar-test-*
# profiles with dummy credentials and deletes them; it never activates them.
# --ui also drives the details form on a temporary profile (no window shown).
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
[[ -d build ]] || meson setup build --buildtype=debugoptimized
meson compile -C build test-profile-save test-profile-page
build/test-profile-save
build/test-profile-save --enterprise
if [[ ${1:-} == --ui ]]; then
  build/test-profile-page
  build/test-profile-page --inherited-mac
fi
