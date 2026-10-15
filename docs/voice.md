# Voice: what it would take, and whether this chip can do it

Status: **nothing here is built yet.** This is the analysis and the plan.

## The starting position

**ESP-Claw has no audio at all.** Not a microphone driver, not a codec, not
STT, not TTS, not an audio capability group. Its capability list is
`cap_agent_mgr, cap_boards, cap_cli, cap_files, cap_http_request, cap_im_local,
cap_im_platform, cap_llm_config, cap_llm_inspect, cap_lua, cap_mcp_client,
cap_mcp_server, cap_router_mgr, cap_scheduler, cap_session_mgr, cap_skill_mgr,
cap_system, cap_web_search` -- text in, text out, end to end.

So voice is not a matter of enabling something. Every layer is ours to write.
That is the single most important fact for planning this.

## What the MP4 board gives us

Better hardware than most ESP32-S3 voice boards, and it is already described
in `boards/mp4_esp32_core/board_devices.yaml`:

- **ES7210**, four-channel microphone ADC on I2S, `adc_channel_mask: "0111"`,
  labels `['NA', 'RE', 'FR', 'FL']`. Two real microphones plus **`RE`, an
  echo-cancellation reference** taken from the amplifier output through the
  R31/R32/R35/R36 divider. That reference is what makes barge-in possible --
  interrupting the robot while it is talking -- and it is already wired and
  declared. Nothing consumes it yet.
- **MAX98357A** class-D amplifier, raw I2S, no control interface at all. It is
  `chip: internal` to the board manager. **`esp_codec_dev_set_out_vol()` is a
  no-op on this path**: volume must be applied in software, on the samples,
  before they reach I2S. Anything ported from a board with a real codec will
  silently fail to change volume.

The generic ESP32-S3 devkit has none of this. Its `board_devices.yaml` is
`devices: []`. **No schematic has been provided for the board with the mic and
speaker**, so which codec it uses, on which pins, is unknown -- and that
decides most of the board-level work.

## Can the chip do it? Yes, with one real constraint

| Resource | Budget | Verdict |
|---|---|---|
| CPU | 2 x 240 MHz. AFE (AEC+VAD+wake word) is roughly a third of one core; Opus encode at 16 kHz/60 ms and decode at 24 kHz together are of the same order | Fits. xiaozhi runs exactly this workload on the same silicon |
| PSRAM | 8 MB octal, and the current build leaves ~5.2 MB free | Not a constraint. Audio ring buffers and Opus state belong here |
| Flash | app is 3.62 MB with esp-sr in a 4352K slot -- about **840 KB spare**. Opus encoder+decoder is ~150-250 KB, `esp_websocket_client` ~30 KB | Fits, with margin to watch |
| **Internal RAM** | the measured number on a booted device was **116 KB free, 63 KB largest contiguous block** | **This is the constraint.** See below |

Internal RAM is where this gets decided. A TLS WebSocket session is ~40-50 KB
on its own, DMA buffers for I2S must be internal, and esp-sr's AFE wants a
sizeable internal working set for latency reasons. 63 KB of contiguous space
is not much to open a TLS socket into.

Mitigations, in the order they should be tried:

1. `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` is 512 -- raising the threshold pushes
   more allocations to PSRAM.
2. Turn off what the robot does not need while voice is running:
   `cap_mcp_server`, the marketplace proxy's in-flight slots,
   `max_open_sockets` on the HTTP server (currently 12).
3. Plain `ws://` to a server on the LAN instead of `wss://` removes the TLS
   working set entirely. Acceptable at home, not on a network you do not own.

`main_log_heap()` already prints free and largest-block at boot and around the
agent; add a call after the audio pipeline starts and the answer stops being a
guess.

## The architecture question: which brain?

This is the decision that matters, and it is not a technical one -- both work.

xiaozhi's server does **ASR + LLM + TTS**. ESP-Claw's agent is *also* an LLM
with tools, memory, a scheduler and the MPX skill runtime. Run both naively
and you have two brains arguing over one robot.

### Option A -- the server is the brain

```
mic -> Opus -> WebSocket -> server: VAD, ASR, LLM, TTS -> Opus -> speaker
                                      |
                                      +-- MCP back over the same socket
                                          to call the robot's tools
```

xiaozhi's protocol carries `"features": {"mcp": true}` in the hello, and the
server supports device-side MCP. ESP-Claw already ships `cap_mcp_server`, so
the robot's capability groups can be exposed to the server's LLM.

- **For:** by far the least firmware work. TTS is solved. Latency is lowest,
  because the server streams and never waits for a whole utterance. Proven on
  this exact silicon.
- **Against:** ESP-Claw's agent, memory, sessions, scheduler and the MPX WASM
  skills are bypassed. The marketplace becomes decoration.

### Option B -- ESP-Claw is the brain

```
mic -> wake word (on device) -> record -> STT endpoint -> text
    -> mpx_voice_submit_text() -> ESP-Claw agent (Poe + robot tools + skills)
    -> reply text -> TTS endpoint -> audio -> speaker
```

`mpx_voice_submit_text()` is the seam this was designed around from the start.

- **For:** keeps everything already built. One brain. Skills, memory and the
  marketplace stay meaningful.
- **Against:** three serial round trips (STT, then LLM, then TTS) instead of
  one streaming pipe -- realistically 2-4 s to first sound. And the xiaozhi
  server does not cleanly expose standalone ASR/TTS endpoints, so this means
  talking to provider APIs directly rather than reusing that server.

### Recommendation

**Build the audio layers first and defer the brain decision**, because layers
1-3 below are identical either way. Only where the text goes differs.

Then choose on this basis: if the robot's *skills* are the product, Option B.
If a talking robot this month is the product, Option A.

## The four layers

Each is independently testable, which matters -- debugging a voice pipeline
end to end is miserable.

**Layer 1 -- audio in and out.** `esp_codec_dev` on the ES7210 and the I2S
output. Test: record 5 s to a WAV in `/fatfs`, serve it over the existing
`/v1/files` route, listen on a laptop. Then play a WAV back. Remember the
software-volume constraint.

**Layer 2 -- wake and endpointing.** esp-sr AFE with AEC fed from the `RE`
channel, WakeNet for the wake word, VAD to decide when the person stopped
talking. The `model` partition (2904K) and `MP4_VOICE_ENABLE` already exist.
Test: log a line on wake, no network involved.

**Layer 3 -- transport.** Opus encode/decode plus a WebSocket client. Test
against a trivial echo server before involving any AI: speak, have the server
send the same Opus back, hear yourself. This isolates codec and framing bugs
from model behaviour completely.

**Layer 4 -- the brain.** Whichever of A or B. Only now does anything need an
ASR or an LLM.

## The wire protocol, if Option A

Device connects with `Authorization: Bearer ...`, `Protocol-Version`,
`Device-Id` (MAC), `Client-Id` (UUID), then:

```json
{"type":"hello","version":1,"features":{"mcp":true,"aec":true},
 "transport":"websocket",
 "audio_params":{"format":"opus","sample_rate":16000,"channels":1,
                 "frame_duration":60}}
```

Server replies with its own `audio_params` and a `session_id` to store. Then:

1. Device: `{"type":"listen","state":"start"}`, then binary Opus frames.
2. Server: `{"type":"stt","text":"..."}` -- the recognised text.
3. Server: `{"type":"tts","state":"start"}`, then binary Opus frames at 24 kHz.
4. Server: `{"type":"tts","state":"stop"}`.

Also `abort` (device, `"reason":"wake_word_detected"` -- this is barge-in),
`llm` (server, emotion hints, which map straight onto
`cap_display_face_set_expression`), and `mcp` (both directions, JSON-RPC 2.0).

Uplink 16 kHz, downlink 24 kHz. Three binary framings exist; v1 is raw Opus
and is the one to implement.

## Choosing providers, given where this robot is

The OpenAI 403 -- `Country, region, or territory not supported` -- is a
standing constraint, not a one-off. It rules out OpenAI's Whisper and TTS
endpoints from here, the same way it ruled out their chat endpoint.

The xiaozhi server supports, among others: ASR via FunASR (local), SherpaASR
(local), Volcano Engine, iFlytek, Tencent, Alibaba, Baidu; TTS via EdgeTTS
(free), FishSpeech, GPT-SoVITS, Index-TTS, PaddleSpeech locally, or the same
cloud vendors. VAD is SileroVAD locally.

Running it locally needs 2 cores / 2 GB for API-only, 4 cores / 8 GB with
FunASR doing ASR on the box. A local server on the LAN also means plain `ws://`
is defensible, which is worth ~40-50 KB of internal RAM.

## Order of work

1. Identify the board with the mic and speaker. Nothing pin-level can start
   without it.
2. Layer 1 on real hardware, both directions.
3. Layer 2: wake word only. This alone is a usable feature and needs no server.
4. Stand up the xiaozhi server on the LAN. Confirm it works with an off-the-
   shelf xiaozhi device or its own test client first -- do not debug a new
   server and new firmware against each other.
5. Layer 3 against an echo server.
6. Layer 4, having chosen A or B.
7. Re-measure internal RAM at every step, not at the end.
