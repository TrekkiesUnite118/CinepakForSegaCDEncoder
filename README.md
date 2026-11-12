# scdcpk — Cinepak for Sega CD encoder

This tool Encodes video and audio into the Cinepak for Sega format. This was done by taking bgvanbur's previous research
and perl scripts and throwing them at Claude AI to make a more user friendly version. The code is entirely done by Claude AI.

Color reduction and palette creation is handled by the Tiled Palete Quantization tool made by Riden and the C++ port made by bbbbbr:
[tilepalquant](https://github.com/bbbbbr/tilepalquant) (MIT)

## Building

`build.bat` (MSVC; finds `vcvars64.bat` itself). Output: `scdcpk.exe`.

## Quick start

```
ffmpeg -i input.mp4 -vf scale=256:224 -r 15 -f yuv4mpegpipe -pix_fmt yuv420p - | scdcpk encode -i - --wav audio.wav -o 01_MOVIE.CIN
```

Video can be up to 256x224; sizes are padded to multiples of 8.

## VirtualDub2

Import `scdcpk.vdprof` (File > Export > Configure external encoders > Import), keeping it next to
`scdcpk.exe` so the program path resolves. Then File > Export > Using external encoder > "Sega CD Cinepak (scdcpk)".
Resize to at most 256x224 with VirtualDub's resize filter first and set the frame rate (12-15 fps seems to work best).

If the import fails, create the three profiles by hand:

| Profile | Type | Command line | Temp file |
|---|---|---|---|
| scdcpk video | Video, raw | `video -q --size %(width)x%(height) --fps %(fpsnum)/%(fpsden) -o "%(tempvideofile)"` | `%(outputname).video.cin` |
| scdcpk audio | Audio, WAV input, bypass compression | `audio -q --normalize -i - -o "%(tempaudiofile)"` | `%(outputname).pcm` |
| scdcpk mux | Multiplexer | `mux -q "%(tempvideofile)" "%(tempaudiofile)" -o "%(outputfile)"` | |

and an encoder set using them with extension `cin`.
VirtualDub2 can also send RGB: set the video profile's pixel format to `bgr24` and add `--pixfmt bgr24`
(avoids 4:2:0 chroma loss). Untested: if the picture comes out upside down, stay with yuv420p.

## Commands

```
scdcpk encode -i INPUT -o OUT.CIN [--wav AUDIO.WAV] [video options]
scdcpk video --size WxH --fps N -o OUT.CIN [video options]     raw frames on stdin
scdcpk audio [-i IN.WAV|-] -o OUT.PCM [--gain G] [--normalize]
scdcpk mux VIDEO.CIN [AUDIO.PCM] -o OUT.CIN
scdcpk info FILE.CIN
```

Video options:

| Option | Default | |
|---|---|---|
| `--palettes N` | 4 | palettes per frame (1–4). Fewer = flatter color, same bitrate |
| `--rate B` | 120000 | video bytes/s. Single-speed CD is 153600; audio uses ~16300 |
| `--fps N` / `A/B` | input / 15 | |
| `--lambda L` | | constant quality instead of rate control |
| `--repal T` | 1.5 | re-quantize palettes when error exceeds T x the last keyframe's; 0 = every frame |
| `--dither off/fast/slow` | off | tilepalquant dithering (costs bits) |
| `--dither-pattern P` | diag4 | diag4 horiz4 vert4 diag2 horiz2 vert2 |
| `--dither-weight W` | 0.5 | dither strength, 0.01–1.0 |
| `--quant-fraction F` | 0.1 | pixels sampled per palette-quantizer pass, 0.01–10 (higher = slower, better palettes) |
| `--iterations N` | 4 | codebook refinement passes |
| `--threads N` | all CPUs | worker threads; output is identical for any count |
| `--raw WxH`, `--pixfmt F`, `--full-range` | | raw input description |
| `--preview PAT` | | write decoded frames as BMP (`dec%05d.bmp`) |
| `--frames N` | | stop after N frames |

## Format notes

- Movies can have up to 4 palettes per frame. The encoder will try to only update palettes when it has to to avoid constant color flickering.
- Frames over 767 tiles (e.g. 256x224 = 896) need a player that puts tiles at VRAM $0000
  and the map at $E000; Sega's sample player stops at 767.

## Credits

- Format and audio/video interleaving follow bgvanbur's SCDTools Cinepak scripts.
- Palette quantization: [tilepalquant](https://github.com/bbbbbr/tilepalquant) by bbbbbr, a C++ port
  of rilden's [tiledpalettequant](https://github.com/rilden/tiledpalettequant).

Sega, Sega CD and Mega-CD are trademarks of Sega Corporation. This project is not affiliated with
or endorsed by Sega.

## License

GPL-2.0-or-later; see `LICENSE`. `third_party/tilepalquant` is MIT-licensed (see its `LICENSE`;
local changes are listed in its `MODIFICATIONS`).
