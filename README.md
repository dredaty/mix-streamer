# Mix Streamer

VST3 plugin, ktorý posiela stereo výstup z DAW ako nekomprimované PCM (16/24-bit) cez lokálny relay server a Cloudflare Tunnel do webového prehrávača klienta.

```
DAW ─ VST3 ─ ws://127.0.0.1:8765/ingest ─ server ─ :3000 (web + /listen) ─ cloudflared ─ https://…trycloudflare.com ─ prehliadač
```

## Server (`server/`)

Požiadavky: Node.js ≥ 18, `cloudflared` v PATH
(macOS: `brew install cloudflared`, Windows: `winget install --id Cloudflare.cloudflared`).

```bash
cd server
npm install
npm run build
npm start
```

Verejná URL sa vypíše do konzoly a uloží do `~/.mix-streamer-url` (Windows: `%USERPROFILE%\.mix-streamer-url`).

| Príkaz / premenná | Popis |
|---|---|
| `npm run start:local` | bez tunela, do súboru sa zapíše LAN URL `http://<ip>:3000` |
| `npm test` | end-to-end selftest (server + testovací zdroj + listener) |
| `node dist/fake-source.js 48000 24` | testovací sínus namiesto DAW |
| `WEB_PORT`, `INGEST_PORT` | porty (default 3000 / 8765) |
| `CLOUDFLARED_BIN` | cesta k `cloudflared`, ak nie je v PATH |
| `TUNNEL_DEBUG=1`, `STATS=1` | log cloudflared / štatistiky prenosu |

## Plugin (`plugin/`)

JUCE 8.0.15 sa stiahne automaticky cez CMake FetchContent (prvý configure trvá dlhšie).

**macOS** (Universal Binary arm64 + x86_64, Xcode Command Line Tools + CMake ≥ 3.22):

```bash
cd plugin
cmake -B build -G Xcode
cmake --build build --config Release
```

Výstup: `build/MixStreamer_artefacts/Release/VST3/Mix Streamer.vst3` → kopírovať do `~/Library/Audio/Plug-Ins/VST3/`.

**Windows** (Visual Studio 2022, x64):

```bash
cd plugin
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Výstup: `build\MixStreamer_artefacts\Release\VST3\Mix Streamer.vst3` → kopírovať do `C:\Program Files\Common Files\VST3\`.

Voliteľné: `-DMIXSTREAMER_COPY_PLUGIN=ON` (automatická inštalácia po builde, na Windows vyžaduje admin), `-DMIXSTREAMER_BUILD_TESTS=ON` (headless test `MixStreamerHeadlessTest`, potrebuje bežiaci server).

## Použitie

1. Spusti server (`npm start`), počkaj na `PUBLIC LISTEN URL`.
2. Vlož **Mix Streamer** na master bus (ako posledný insert).
3. V plugine: *Connected*, zvoľ 16/24-bit, **Copy Link** a pošli link klientovi.
4. Klient otvorí link, stlačí Play; pri slabom pripojení zvolí High (500 ms).

## Poznámky

- Bitrate: 48 kHz/24-bit ≈ 2,3 Mbit/s, 16-bit ≈ 1,5 Mbit/s na poslucháča (bez kompresie).
- 16-bit používa TPDF dither. 24-bit je bit-exact (overené testom).
- Prehrávač kompenzuje drift hodín DAW vs. klient úpravou pomeru prevzorkovania o max. ±0,2 %; ak sa sample rate streamu a zvukovky klienta líšia, prevzorkuje (Hermite).
- Quick tunnel (`trycloudflare.com`) dostane pri každom štarte novú URL. Pre stálu URL treba named tunnel na vlastnej doméne.
- Aktívny je vždy len jeden zdroj – ďalšia inštancia pluginu nahradí predchádzajúcu.
- Bez autentifikácie: kto má link, počúva.
