# Mix Streamer

All-in-one VST3 plugin na počúvanie mixu na diaľku: nekomprimované PCM 16/24-bit cez Cloudflare Tunnel do webového prehrávača. Klient nič neinštaluje.

```
DAW → Mix Streamer.vst3 ─┬─ lock-free FIFO → sender thread → PCM pakety (10 ms)
                         ├─ HTTP/WebSocket server 127.0.0.1:<náhodný port>  (web prehrávač + /listen)
                         └─ cloudflared (zabalený v plugine) → https://….trycloudflare.com → prehliadač
```

## Inštalácia

Hotové buildy: GitHub → **Actions** → posledný úspešný beh **Build** → *Artifacts*.

| Systém | Artefakt | Kam skopírovať priečinok `Mix Streamer.vst3` |
|---|---|---|
| Windows | `MixStreamer-VST3-Windows-x64` | `C:\Program Files\Common Files\VST3\` |
| macOS | `MixStreamer-VST3-macOS-Universal` | `~/Library/Audio/Plug-Ins/VST3/` |

Na macOS plugin nie je notarizovaný. Po skopírovaní spusti:

```bash
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Mix Streamer.vst3"
```

`cloudflared` je priamo v plugine (`Contents/Resources`). Netreba Node.js ani žiadny server.

## Použitie

1. Vlož **Mix Streamer** na master bus ako posledný insert.
2. Spusti prehrávanie. Po ~0,5 s audia sa spustí server a tunel; stav prejde *Starting tunnel…* → *Online* (3–10 s).
3. **Copy Link** a pošli link klientovi. Klient stlačí Play; pri slabom pripojení zvolí High (500 ms).

Stavy pluginu: *Waiting for audio* (nič nebeží), *Starting tunnel…*, *Online*, *Tunnel offline – retrying* (bez internetu, opakuje s back-off), *cloudflared not found*, *Another instance is streaming* (streamuje vždy len jedna inštancia).

Log tunela: `%TEMP%\mix-streamer-cloudflared.log` / `$TMPDIR/mix-streamer-cloudflared.log`.

## Build zo zdrojákov

JUCE 8.0.15 sa stiahne cez CMake FetchContent.

```bash
cd plugin
```

macOS (Universal arm64 + x86_64):

```bash
cmake -B build -G Xcode && cmake --build build --config Release
```

Windows (Visual Studio 2022):

```bash
cmake -B build -A x64 && cmake --build build --config Release
```

Lokálny build neobsahuje `cloudflared` – plugin ho potom hľadá v systéme (winget / Homebrew / PATH), alebo v `MIXSTREAMER_CLOUDFLARED`.

Testy: `-DMIXSTREAMER_BUILD_TESTS=ON`, potom `MixStreamerHeadlessTest` (s `MIXSTREAMER_NO_TUNNEL=1` bez tunela; `--serve 60` streamuje testovací sínus pre skúšku v prehliadači).

## Poznámky

- Bitrate na poslucháča: 48 kHz/24-bit ≈ 2,3 Mbit/s, 16-bit ≈ 1,5 Mbit/s.
- 16-bit s TPDF ditherom, 24-bit bit-exact.
- Prehrávač kompenzuje drift hodín DAW/klient (±0,2 %) a v prípade potreby prevzorkuje (Hermite).
- Quick tunnel dostane pri každom spustení novú URL. Stála URL = named tunnel na vlastnej doméne (zatiaľ neimplementované).
- Bez autentifikácie – kto má link, počúva.
- Server počúva len na 127.0.0.1, z LAN nie je dostupný; von ide výhradne cez tunel.
