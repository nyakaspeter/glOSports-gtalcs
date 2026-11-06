#!/bin/sh
# A small static FFmpeg for the video player (loader/movie.c): the MP4
# demuxer and the H.264 and AAC decoders only, for the GA36-MB's Cortex-A7
# (NEON). Source: ffmpeg.org's ffmpeg-6.1.2.tar.xz (sha256 3b624649725e...),
# unpacked in upstream/ffmpeg-6.1.2 (not committed). Output: build/ffmpeg/
# (include/ and lib/libav*.a), linked into the loader by build.sh.
set -eu
cd "$(dirname "$0")/.."
[ -d upstream/ffmpeg-6.1.2 ] || { echo "build-ffmpeg.sh: upstream/ffmpeg-6.1.2 missing (ffmpeg.org, ffmpeg-6.1.2.tar.xz)" >&2; exit 1; }
docker run --rm -v "$PWD":/w -w /w/upstream/ffmpeg-6.1.2 gtalcs-builder:bookworm sh -c '
set -e
./configure --prefix=/w/build/ffmpeg --enable-cross-compile --cross-prefix=arm-linux-gnueabihf- \
	--arch=arm --cpu=cortex-a7 --target-os=linux --enable-neon --enable-pic \
	--enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
	--disable-everything --disable-network --disable-autodetect --enable-pthreads \
	--disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-postproc \
	--enable-demuxer=mov --enable-decoder=h264,aac --enable-parser=h264,aac --enable-protocol=file \
	--extra-cflags="-O2 -mfloat-abi=hard -mfpu=neon-vfpv4" >/dev/null
make -j8 >/dev/null
make install >/dev/null
'
ls -la build/ffmpeg/lib
