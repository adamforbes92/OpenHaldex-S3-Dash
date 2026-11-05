#!/usr/bin/env bash
# Regenerate the two big LVGL fonts (rev digits, boost value).
#
# LVGL ships Montserrat only up to 48 px, so the dashboard's 96 px rev digits
# and 56 px boost value are generated from Montserrat Bold here. The output is
# committed, so a normal build does not need this script or node.
#
# Needs node (no npm) plus lv_font_conv. If you have neither, fetch the
# tarball it ships in and point LV_FONT_CONV at the CLI inside it:
#
#   curl -sSL -o /tmp/lvfc.tgz \
#     https://registry.npmjs.org/lv_font_conv/-/lv_font_conv-1.5.3.tgz
#   mkdir -p /tmp/lvfc && tar xzf /tmp/lvfc.tgz -C /tmp/lvfc
#   LV_FONT_CONV="node /tmp/lvfc/package/lv_font_conv.js" tools/make_fonts.sh
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
conv="${LV_FONT_CONV:-node $here/lv_font_conv.js}"
ttf="tools/fonts/Montserrat-Bold.ttf"
out="src/ui/fonts"

# Generate from the repo root with relative paths: lv_font_conv records the
# command in the file header, and an absolute path there would bake one
# developer's checkout directory into the committed source.
cd "$root"

[ -f "$ttf" ] || { echo "missing $ttf" >&2; exit 1; }
mkdir -p "$out"

# Digits and the dash: '-' is what draws '--' when a field is absent.
$conv --font "$ttf" --size 96 --bpp 4 --format lvgl \
      --symbols "0123456789-" --lv-font-name lv_font_dash_rpm_96 \
      -o "$out/font_rpm_96.c"

# Boost value, e.g. "1.55".
$conv --font "$ttf" --size 56 --bpp 4 --format lvgl \
      --symbols "0123456789." --lv-font-name lv_font_dash_num_56 \
      -o "$out/font_num_56.c"

echo "wrote $out/font_rpm_96.c and $out/font_num_56.c"
