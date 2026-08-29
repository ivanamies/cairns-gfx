#!/bin/zsh
# #228 H5: re-bake the load_flow runtime golden (committed PNG; gated to
# metal until #296 fixes vk's render of this workload).
#
# Usage: scripts/regenerate_load_flow_golden.sh
#
# Process: runs verify_load_flow.sh metal, ignoring the golden-cmp failure
# (since we're regenerating), then promotes tmp/_lf_metal_*.png to
# goldens/load_flow_*_metal.png. The pixel-delta checks still apply, so
# a totally-broken run won't bake.
set -e
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")/.."

mkdir -p goldens

# Delete current goldens so verify_load_flow's "missing" path takes over
# (the missing-golden branch still runs the JS + pixel-delta steps; only
# the cmp-against-committed step is skipped).
/bin/rm -f goldens/load_flow_loaded_metal.png \
            goldens/load_flow_animated_metal.png \
            goldens/load_flow_empty_metal.png

# Run; ignore the golden-missing exit since we're about to bake.
scripts/verify_load_flow.sh metal 2>&1 || true

# Sanity: did the dumps actually appear?
for f in tmp/_lf_metal_empty.png tmp/_lf_metal_loaded.png tmp/_lf_metal_animated.png; do
    [ -f "$f" ] || { echo "$f missing -- harness did not produce dumps" >&2; exit 1; }
done

cp tmp/_lf_metal_empty.png    goldens/load_flow_empty_metal.png
cp tmp/_lf_metal_loaded.png   goldens/load_flow_loaded_metal.png
cp tmp/_lf_metal_animated.png goldens/load_flow_animated_metal.png

echo "load_flow goldens regenerated:"
ls -la goldens/load_flow_*_metal.png
