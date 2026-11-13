#!/bin/sh
# Installs every car downloaded into tmp_cars/ (.zip, .viv, or unpacked
# folders) as a new car in gamedata/carmodel, leaving the cars already there
# alone: tools/import_car.py does the work and says what it did.
#
#   tools/import_tmp_cars.sh            # install them
#   tools/import_tmp_cars.sh --dry-run  # only say what would be done
#
# A car already installed from the same download comes in again under the
# next free name, so move what is done out of tmp_cars/ (or delete it).
cd "$(dirname "$0")/.." || exit 1
set -- "$@"
found=0
for source in tmp_cars/*; do
    [ -e "$source" ] || continue
    found=1
done
if [ "$found" = 0 ]; then
    echo "tmp_cars/ is empty: put the downloaded .zip files there first"
    exit 1
fi
exec python3 tools/import_car.py . tmp_cars/* "$@"
