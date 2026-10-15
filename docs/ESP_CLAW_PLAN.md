# mpv2-claw: ESP-Claw on the MP4 robot, one brain, zero setup

Living plan and working memory for this project. **Read this first** in any new
session; update the status table when a phase moves.

## Decisions (made with the owner, 2026-10-04)

| Topic | Decision |
|---|---|
| Base | `mpv2-claw` is built on **mp4-claw's tree** (ESP-Claw already integrated and proven on this board) with mpv2's parts moved in (robot first; Pi/ROS link, display extras, PWA changes next). `mpv2` stays untouched as the fallback firmware. |
| Brain | **One brain: the ESP-Claw agent on the robot**, for PWA chat *and* voice. One session store, one long-term memory. |
| Voice | Wake word on the ESP (esp-sr) -> Opus stream to the **MangDang voice_gateway** (xiaozhi protocol, server VAD + ASR) -> `stt` text -> ESP-Claw agent on the robot -> `speak` text -> gateway TTS -> Opus -> speaker. No xiaozhi.me, no activation code. |
| Zero setup | A user only joins Wi-Fi (PWA / MangDang app). The robot authenticates to the MangDang gateway with its UUID + AES key (already in firmware). The gateway holds the provider keys. Power users can still enter their own LLM key at `/settings`; that overrides the gateway. |
| LLM | **Poe first** (server `AI_ACCOUNT=poe`), easy switch later; DeepSeek option: **`deepseek-flash`** (OpenAI-compatible, `https://api.deepseek.com`, tool calls supported), configured on the server in `.env`, low/no thinking for voice latency. `deepseek-chat` was discontinued 2026-07-24 -- do not use it. |
| STT / TTS | DeepSeek has none. One-key account on the server: SiliconFlow (DeepSeek V4 Flash + SenseVoice + CosyVoice) by default; plugins are swappable. |
| Web market | Stays on the **OpenClaw cloud** (`35.220.215.160:8080`): marketplace proxy, encrypted `/v1/chat/ingress` socket, `openclaw_action` deploys, PWA permission pop-up. |
| Market from chat/voice | The agent decides: new capability **`openclaw_cloud`** forwards the request to the OpenClaw cloud over the existing AES-GCM socket and returns its answer. Permission pop-up still applies (auto-deny when no PWA is open). |
| ROS / Pi slave | Unchanged. In slave mode voice is off and the agent's movement tools refuse. |

## Architecture

```
PWA chat ──ws /v1/chat/ui──┐
                            ├─> ESP-Claw agent (on ESP) ──HTTPS──> MangDang gateway /v1/llm  ──> DeepSeek
voice: wake -> rec -> STT ──┘        │  tools: robot, display, skills (WASM), Lua, scheduler, memory
            (gateway /v1/stt)        │  tool openclaw_cloud ──AES-GCM ws──> OpenClaw cloud (market skills)
reply text -> gateway /v1/tts -> PCM -> speaker
PWA Store/Skills ──/v1/marketplace/*──> gateway (unchanged)
Pi (ROS) ──UART 921600──> pi_link (unchanged; slave mode overrides everything)
```

## Server: `voice_gateway` (in `C:\esp6\projects\server`) -- written 2026-10-04

Modelled on xiaozhi-esp32-server; plugin providers; **one AI account** (`AI_ACCOUNT` +
`AI_API_KEY` in `.env`). Default **`poe`** (owner's choice, 2026-10-04): LLM = a Poe bot
(`AI_LLM_MODEL`, default Claude-Haiku-4.5); Poe has no speech API, so ASR = SenseVoice on the
server CPU and TTS = Edge voices, both keyless. Switch later with one line (`siliconflow`, `openai`, ...).
Port 8000, own Docker service next to core_gateway. See `voice_gateway/README.md`.

- `POST /xiaozhi/ota/` -- zero-setup provisioning (URL + device token, no activation code).
- `GET /xiaozhi/v1/` -- xiaozhi protocol v1 voice socket. Server VAD (Silero) -> ASR -> brain -> TTS, Opus 16 k up / 24 k down, 60 ms.
  - **server brain** (default; today's mpv2): LLM here, robot tools via MCP on the socket.
  - **device brain** (`hello.features.brain = "device"`; mpv2-claw): server sends `stt`, robot answers with
    `{"type":"speak","text":...,"last":bool}`; robot may also speak unprompted.
- `POST /v1/llm/chat/completions` -- OpenAI-compatible LLM for the ESP-Claw agent. API key = **robot token**
  `"<uuid>.<hex HMAC-SHA256(aes_key, 'mpx-ai-v1:'+uuid)[:32]>"`; model forced to the server's.
- Limits per device / robot; `python -m vg.check` tests the account with real requests;
  `python -m tests.test_e2e` tests everything offline (passes).
- Firmware side (mpv2-claw) therefore needs: compute the robot token at boot; set ESP-Claw LLM
  base URL `http://<server>:8000/v1/llm` + token as key (unless the user set their own in /settings);
  voice link: provision against `http://<server>:8000/xiaozhi/ota/`, send `brain:"device"`, route
  `stt` text into the agent, send the agent's reply as `speak`.

## Firmware phases and gates

| # | Work | Gate (on hardware) | Status |
|---|---|---|---|
| 0 | Copy mpv2 -> mpv2-claw; this plan | folder exists, plan saved | done |
| S | Server voice_gateway (OTA, voice WS, server+device brain, /v1/llm) | offline e2e test passes; then deploy + `vg.check`; then old mpv2 talks via `voice provision` | code done, deploy pending |
| 1 | mp4-claw tree + mpv2 robot (minipupperesp moves) + 7 MB factory partition (nvs kept at 0x9000) + screen 180 + voice zero-setup (gateway OTA) + agent LLM via gateway | boots, walks from PWA, screen right, heap logged; "Hi ESP" talks; PWA chat answers | built 2026-10-04 (5.38 MB), awaiting hardware test |
| 2 | PWA chat -> on-device agent (mp4-claw chat bridge) with robot/display/skill caps; LLM via gateway (fallback: key in /settings) | "stand up", "happy face" from chat; memory survives reboot | todo |
| 3 | Voice one-brain: wake -> rec -> gateway STT -> agent -> gateway TTS -> speaker; face shows listening/thinking/speaking | "Hi ESP, sit down" | todo |
| 4 | Market: OpenClaw upstream socket kept for Store deploys + `openclaw_cloud` capability | deploy a market skill from Store; "install X skill" by chat and by voice | todo |
| 5 | Pi/ROS regression | slave mode, ROS walking, video, HOST_INFO; agent + voice stand down | ported 2026-10-07 (components/mpx_pi_link = mpv2 pi_link/pi_audio/pi_video, protocol byte-identical; host test against the bridge's protocol.py passes), awaiting hardware test |

## Memory rules (RAM) -- these cost crashes before

- Internal RAM is the constraint. mpv2 boots with ~49 KB internal free. Log
  `log_internal_ram()` at every stage. **Gate: >= 30 KB internal free during a
  voice turn + LLM call.** If not, trim before adding.
- Task stacks in PSRAM are fine **except** for code that touches flash (NVS,
  FAT/LittleFS, `esp_partition_mmap`): run that through `platform/flash_safe`
  (internal-stack worker) or the cache-frozen assert fires
  (`s_task_stack_is_sane_when_cache_frozen`).
- Keep: `SPIRAM_MALLOC_ALWAYSINTERNAL=0`, `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`,
  `MBEDTLS_EXTERNAL_MEM_ALLOC=y`, `SPIRAM_XIP_FROM_PSRAM=y`. Big static buffers
  get `EXT_RAM_BSS_ATTR`.
- I2S DMA must be internal; `i2s std init: ESP_ERR_NO_MEM` means internal RAM ran out.
- LWIP has 20 sockets: marketplace proxy keeps its 15 s GET cache and max 3 in flight.
- Never two TLS sessions opened at the same moment if avoidable (STT, LLM, TTS run in sequence).

## Working rules

- Never edit `third-party/esp-claw/`; use its extension points (capability
  groups, Lua modules, skills, router rules, shadowing components).
- mpv2 sources are CRLF; keep line endings.
- Console is UART0 (CH340K). The `display`, `wake`, `audio`, `voice` console commands stay.
- Do not run git in the project folders from automation; never commit/push unless asked.
- `-Werror=comment`: never put a slash-asterisk pair inside a block comment.
- Rerun `idf.py reconfigure` after adding files to a component that globs sources.

## Owner's setup

- Pi 5 `ubuntu@192.168.1.102` (hostname `pupper`), ROS 2 Jazzy, workspace `~/ros2_ws/src/ros2jazzympv2`.
- Gateway / OpenClaw cloud: `35.220.215.160:8080`, robot UUID `MPX-DOG-01`.
- Fallback firmware: `C:\esp\projects\mpv2`. Reference ESP-Claw integration: `C:\esp\projects\mp4-claw` (read its `AGENTS.md`).
