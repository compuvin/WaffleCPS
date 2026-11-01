#!/bin/sh
set -eu
source_png="$1"
output_icns="$2"
iconset="${output_icns%.icns}.iconset"
mkdir -p "$iconset"
render() {
    sips -z "$1" "$1" "$source_png" --out "$iconset/$2" >/dev/null
}
render 16 icon_16x16.png
render 32 icon_16x16@2x.png
render 32 icon_32x32.png
render 64 icon_32x32@2x.png
render 128 icon_128x128.png
render 256 icon_128x128@2x.png
render 256 icon_256x256.png
render 512 icon_256x256@2x.png
render 512 icon_512x512.png
render 1024 icon_512x512@2x.png
iconutil -c icns -o "$output_icns" "$iconset"
