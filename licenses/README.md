# Third-party components

The port builds on and ships with:

| Component | Licence | Text |
|---|---|---|
| loader code carried over from [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita) | MIT | `gtasa_vita.LICENSE.txt` |
| loader code carried over from [max_vita](https://github.com/fgsfds/max_vita) | MIT | `max_vita.LICENSE.txt` |
| FFmpeg 6.1.2 (MP4 demuxer, H.264/AAC decoders), linked statically into the loader for the video player; source: [ffmpeg.org](https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz), built by `tools/build-ffmpeg.sh` | LGPL-2.1 or later | `ffmpeg.COPYING.LGPLv2.1.txt` |
| OpenAL Soft 1.19.1 (`lib/libopenal.so.1`, Debian bookworm `libopenal1` 1:1.19.1-2) | LGPL-2.0 or later | `openal-soft.copyright.txt` |
| sndio, libbsd, libmd (`lib/`, Debian bookworm, OpenAL Soft's dependencies) | ISC, BSD-3-Clause | `sndio.copyright.txt`, `libbsd.copyright.txt`, `libmd.copyright.txt` |
| ARM's Mali user-space driver (`glos/lib/libMali.so`) | ARM proprietary | `libMali.txt` |
| ARM's Mali kernel driver (`glos/mali.ko`) | GPL-2.0 | goodluckOS-mali-driver |

The game itself is not included: its files come from your own copy.
