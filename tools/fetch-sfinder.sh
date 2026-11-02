#!/bin/sh
# Downloads solution-finder (https://github.com/knewjade/solution-finder) into
# tools/sfinder/. Needs Java 8+ to run.
set -e
cd "$(dirname "$0")"
VERSION=1.43
rm -rf sfinder sfinder.zip
curl -sSfL -o sfinder.zip "https://github.com/knewjade/solution-finder/releases/download/v$VERSION/solution-finder-$VERSION.zip"
mkdir sfinder
unzip -q sfinder.zip -d sfinder
rm sfinder.zip
# The zip may contain a top-level folder; flatten it.
if [ ! -f sfinder/sfinder.jar ]; then
  inner=$(dirname "$(find sfinder -name sfinder.jar | head -1)")
  mv "$inner"/* sfinder/ && rmdir "$inner"
fi
echo "solution-finder $VERSION installed in tools/sfinder"
