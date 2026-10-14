#!/bin/sh
# Copy the shared headers into both sketch folders (Arduino can't include files outside a sketch).
cd "$(dirname "$0")"
for d in cat_collar cat_base; do cp common/protocol.h common/config_json.h "$d/"; done
echo "copied common headers into cat_collar and cat_base"
