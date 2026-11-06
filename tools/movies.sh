#!/bin/sh
# The intro, converted for the console's player (loader/movie.c): the APK's
# res/raw/intro.m4v (1920x1080 MPEG-4 Part 2, 3 Mbit/s: far too heavy to
# decode in time; a 4:3 picture between black side bars) -> the 1440x1080
# picture cropped out, 640x480 H.264 baseline (no B-frames: the cheapest to
# decode on the Cortex-A7), AAC 44.1 kHz stereo, as the name the game asks
# for. Out: gamedata/movies/Movies/LCS_INTRO_1080.m4v, copied into game/.
#   tools/movies.sh path/to/intro.m4v
set -eu
cd "$(dirname "$0")/.."
in=${1:?usage: tools/movies.sh path/to/res/raw/intro.m4v}
out=gamedata/movies/Movies
mkdir -p "$out"
ffmpeg -v error -y -i "$in" -vf crop=1440:1080:240:0,scale=640:480:flags=lanczos \
	-c:v libx264 -profile:v baseline -level 3.0 -preset slow -crf 20 -pix_fmt yuv420p \
	-c:a aac -b:a 128k -ar 44100 -ac 2 -movflags +faststart "$out/LCS_INTRO_1080.m4v"
ls -la "$out"
