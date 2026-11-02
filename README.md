# Voice Studio

Local-first VST3 voice-processing plugin for Windows and Wave Link.

## Safety contract
- Audio processing only through the host's normal VST3 interface.
- No game-process access, memory reads/writes, DLL injection, hooks, anti-cheat bypasses, kernel drivers, or stealth behavior.
- No telemetry, runtime network calls, account sign-in, or cloud inference in the plugin.
- AI inference is opt-in and disabled by default.
- Non-AI DSP remains functional when the AI engine is absent or stopped.
- Any future AI worker must be a visible, user-controlled process with explicit start/stop status.
- Never disable or evade FACEIT Anti-Cheat, Easy Anti-Cheat, or other security software.

## Current DSP implementation
The DSP path is opt-in via **ENABLE DSP PROCESSING** (enabled by default). It includes:
- 80 Hz high-pass filter for low-frequency rumble.
- +2.5 dB presence peak around 3.2 kHz for speech intelligibility.
- Compressor at -18 dB threshold, 3:1 ratio, 8 ms attack, 100 ms release.
- Output level control from -18 dB to +12 dB.
Each processing stage can be switched independently. With DSP disabled, audio is passed through unchanged.

These are conservative starting settings, not a substitute for microphone-specific tuning. Use the output level control carefully to avoid clipping.

## AI status
AI is not implemented and cannot be enabled in this build. No AI engine, models, external binaries, drivers, or background services are bundled.

## Planned AI architecture
A separate, optional, visible local inference worker may be added after its license, dependencies, model format, and IPC design are reviewed. AI will be off by default. Disabling AI will retain the model for 30 seconds, then release it; **UNLOAD AI NOW** will request immediate shutdown. Worker failure/timeouts must fall back to the non-AI DSP path.

## Planned voice presets
Radio Operator, Deep Male, Female Voice, Robot, Demon, Anime Voice. These are targets for future tuning/model configurations, not currently implemented voice models.
