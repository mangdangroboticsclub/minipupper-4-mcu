# Voice, Option A: the cloud is the brain and the robot is its hands

The question this answers: *can the cloud tell ESP-Claw to change the screen?*

**Yes -- and less new code than you would expect, because the machinery for it
is already in the build.**

## How the round trip works

```
  you speak
      |
      v
  mic -> ES7210 -> Opus -> WebSocket -----------> xiaozhi server
                                                    ASR -> LLM
                                                       |
        device <---- {"type":"mcp", tools/call } <-----+   LLM decided to
          |                                                call a robot tool
          v
   claw_cap_call("display_show_emotion", {"emotion":"sad"})
          |                                    <-- the face changes here
          v
        device -----> {"type":"mcp", result } -----> server
                                                    LLM continues
                                                       |
  speaker <- Opus <- {"type":"tts"} + audio <---------+  "okay, feeling sad"
```

So the screen changes *before* the robot finishes speaking, and the model can
see whether the call succeeded and say so. That is the behaviour you asked
for.

## Why this is cheaper than it looks

Three pieces already exist and do not need writing:

1. **`cap_mcp_server`** already exposes ESP-Claw's capability groups as MCP
   tools. Every tool the agent can call -- display, robot, files, Lua, skills
   -- is already described in MCP form.
2. **`claw_cap_list()` and `claw_cap_call()`** are the whole execution
   interface. Listing tools and running one are two function calls.
3. **The MCP C SDK's transport is a plug-in table.**
   `esp_mcp_transport_t` in `managed_components/espressif__mcp-c-sdk` is a
   struct of function pointers -- `init`, `start`, `register_endpoint`,
   `emit_message`, and so on -- and `cap_mcp_server` simply passes
   `esp_mcp_transport_http_server` into it.

So the bridge is **one new transport implementation** that moves JSON-RPC over
the xiaozhi WebSocket instead of over HTTP. Nothing about tools, schemas or
dispatch changes. That is a few hundred lines, not a subsystem.

xiaozhi's protocol is built for this: the device advertises
`"features": {"mcp": true}` in its hello, and `mcp` messages flow both ways as
JSON-RPC 2.0.

## The board: Santy Control (schematic dated 2025-12-17)

Read off the schematic, and it is a better fit than the bare devkit:

| | |
|---|---|
| MCU | ESP32-S3-WROOM-1-**N16R8** -- 16 MB flash, 8 MB octal PSRAM |
| Mic ADC | **ES7210** at I2C `0x41` -- the same part as the MP4 board |
| Amplifier | **MAX98357A** -- also the same as the MP4 board |
| I2S | MCLK `IO38`, BCK `IO14`, WS `IO13`, DIN `IO12`, DOUT `IO45` |
| I2C | SDA `IO1`, SCL `IO2`, 1 k pull-ups (R15/R16) |
| Mics | two analog ZTS6216 into MIC1/MIC2 |
| **AEC reference** | amplifier OUTP/OUTN divided by R31/R32/R35/R36 into **MIC3** |
| IMU | **QMI8658A** at `0x6a` -- *not* the MP4's BMI270 |
| Display | SPI TFT: SCK `IO41`, MOSI `IO40`, DC `IO39`, backlight `IO42` |
| Motors | two DC motors via DRV8833 on `IO19`/`IO20` and `IO47`/`IO48` |
| Console | CH340K on U0TXD/U0RXD |

Three things worth calling out:

- **The audio architecture is identical to the MP4 board** -- same ADC, same
  amplifier, same trick of feeding the amplifier output back into MIC3 as an
  echo reference. So one `mpx_audio` component serves both boards; only pin
  numbers differ. That is a large saving, and it means voice work done now is
  not throwaway when the MP4 arrives.
- **`IO19` and `IO20` drive Motor A.** Those are the ESP32-S3's native USB
  pins, so this board has **no USB-Serial-JTAG** -- the console must be UART0
  through the CH340K. The generic board file already defaults that way.
- **`esp_codec_dev_set_out_vol()` will do nothing.** The MAX98357A has no
  control interface (SD_MODE pulled up through R24, GAIN only to test point
  TP1). Volume must be applied in software before samples reach I2S.

16 MB flash means `partitions_16MB.csv` applies unchanged -- 4352K app slots
and the 2904K `model` partition for wake-word models.

## Phases, each with a gate you can actually check

**Phase 0 -- board definition.** `boards/santy_control/` with the ES7210,
the MAX98357A as `chip: internal`, the ST7789 on SPI, and the QMI8658A.
*Gate:* `selftest --other-board` finds `0x41` and `0x6a` on the I2C scan, and
the display test pattern looks right.

**Phase 1 -- audio in and out (`components/mpx_audio`).** Capture 16 kHz mono
from the ES7210; play 16/24 kHz to the amplifier with software volume.
*Gate:* record five seconds to `/fatfs`, download it through the existing
`/v1/files` route, and listen on a laptop. Then play a WAV back. No network,
no AI.

**Phase 2 -- Opus and the socket (`components/mpx_voice_link`).** Opus encode
at 16 kHz/60 ms, decode at 24 kHz, `esp_websocket_client`, the hello
handshake, and the `listen`/`stt`/`tts` messages.
*Gate:* point it at a bare echo server that returns the same Opus frames.
Hearing yourself proves codec and framing without a single model involved.

**Phase 3 -- the MCP bridge (`components/mpx_mcp_ws`).** Implement
`esp_mcp_transport_t` over the socket from Phase 2 and hand it to
`cap_mcp_server` instead of `esp_mcp_transport_http_server`.
*Gate:* say "make a sad face" and watch the eyes change. **This is the phase
that answers your question**, and it is deliberately after the transport works,
because debugging MCP through a broken audio path is miserable.

**Phase 4 -- wake word and barge-in.** esp-sr AFE with AEC fed from MIC3,
WakeNet for the wake word, and `{"type":"abort","reason":"wake_word_detected"}`
so you can interrupt the robot mid-sentence.
*Gate:* talk over the robot and have it stop.

**Phase 5 -- polish.** The server's `llm` messages carry emotion hints; map
them onto `cap_display_face_set_expression` so the face tracks the
conversation without a tool call at all.

## Budget

Flash is fine: 3.62 MB used of a 4352K slot with esp-sr already in, and audio
plus Opus plus the WebSocket client is roughly 300 KB.

**Internal RAM stays the thing to watch.** A booted device reported 116 KB
free with a 63 KB largest block. Running the server on your LAN over plain
`ws://` rather than `wss://` avoids a TLS session's 40-50 KB, which is the
cheapest win available and is defensible on a network you control. Add a
`main_log_heap()` call after the audio pipeline starts, and measure at every
phase rather than at the end.

## What stays and what goes

Being straight about the cost of Option A: with the cloud as the brain,
ESP-Claw's own agent, its memory, its scheduler and the MPX WASM skills are
not in the conversation path. What survives -- and it is the important part --
is that **every capability stays reachable**, because the cloud calls them
through MCP. The robot keeps its abilities; it loses its own reasoning.

Phases 0-2 are identical under either architecture. If you later want the
on-device agent back, only Phase 3 is replaced -- by
`mpx_voice_submit_text()` feeding `cap_im_local` instead of a transport
bridge. Nothing built before then is wasted.
