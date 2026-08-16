#!/bin/bash
# Downloads the third-party JS/CSS the web control page depends on
# (jQuery, Bootstrap + the Darkly theme, Tether, React, ReactDOM,
# Babel-standalone) into web/static/, so http://oleander.local works with
# no internet access.
#
# Previously these were loaded straight from public CDNs by the browser on
# every page load. Oleander is designed to run headless on the local
# network via mDNS (oleander.local) -- a venue or rehearsal space Wi-Fi
# network commonly has no internet uplink at all, in which case every one
# of those CDN requests fails and the control page never renders (blank
# page, not a degraded one, since React/Babel never load). Vendoring the
# exact pinned files locally removes that dependency entirely.
#
# Run this once during setup (install.sh does this automatically) and
# again any time you bump a version below. Requires internet access at the
# time it's run (e.g. run it on the Pi during initial setup, while it's
# still on a network with internet).
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_STATIC_DIR="$SCRIPT_DIR/../web/static"

download_asset() {
  local url="$1"
  local dest="$2"
  if curl -fsSL "$url" -o "$dest"; then
    echo "  OK: $(basename "$dest")"
  else
    echo "  WARNING: could not download $(basename "$dest")."
    echo "           Re-run this script once this machine has internet"
    echo "           access -- until then, the web UI will fail to load."
  fi
}

echo "Downloading web UI assets into $WEB_STATIC_DIR ..."
mkdir -p "$WEB_STATIC_DIR"

download_asset "https://code.jquery.com/jquery-3.2.1.min.js" \
  "$WEB_STATIC_DIR/vendor-jquery.min.js"
download_asset "https://cdn.jsdelivr.net/npm/bootswatch@4.5.2/dist/darkly/bootstrap.min.css" \
  "$WEB_STATIC_DIR/vendor-bootstrap-darkly.min.css"
download_asset "https://cdnjs.cloudflare.com/ajax/libs/tether/1.4.0/js/tether.min.js" \
  "$WEB_STATIC_DIR/vendor-tether.min.js"
download_asset "https://maxcdn.bootstrapcdn.com/bootstrap/4.0.0-alpha.6/js/bootstrap.min.js" \
  "$WEB_STATIC_DIR/vendor-bootstrap.min.js"
download_asset "https://unpkg.com/react@16.0.0/umd/react.production.min.js" \
  "$WEB_STATIC_DIR/vendor-react.min.js"
download_asset "https://unpkg.com/react-dom@16.0.0/umd/react-dom.production.min.js" \
  "$WEB_STATIC_DIR/vendor-react-dom.min.js"
download_asset "https://unpkg.com/babel-standalone@6.26.0/babel.min.js" \
  "$WEB_STATIC_DIR/vendor-babel-standalone.min.js"

echo "Done."
