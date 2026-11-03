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
The DSP path is controlled by **ENABLE DSP PROCESSING** and is enabled by default. When disabled, audio is passed through unchanged. It includes:
- 80 Hz high-pass filter for low-frequency rumble.
- +2.5 dB presence peak around 3.2 kHz for speech intelligibility.
- Compressor at -18 dB threshold, 3:1 ratio, 8 ms attack, 100 ms release.
- Output level control from -18 dB to +12 dB.
Each processing stage can be switched independently. With DSP disabled, audio is passed through unchanged.

These are conservative starting settings, not a substitute for microphone-specific tuning. Use the output level control carefully to avoid clipping.

## AI status
AI inference is not implemented and cannot be enabled in this build. The build includes a separate transport-only `VoiceStudioWorker.exe`; it can be started explicitly from the Effects page and stopped from the same page. The Worker validates the local IPC connection and reports READY, but deliberately rejects AI inference because no inference runtime or licensed model is integrated. No model is bundled or downloaded.

## Worker control (Windows build)
The GitHub Actions artifact contains both the VST3 bundle and `VoiceStudioWorker.exe`. Install the VST3 as usual, keep the Worker executable in a folder you control, then open the Effects page and choose **SELECT WORKER EXE & START**. The UI reports the Worker lifecycle and **STOP WORKER** closes it. The Worker is not started automatically. Closing the plugin also stops it. A READY Worker is only an IPC/transport status, not an active voice converter.

## External control foundation

A versioned local command ABI has been added for future FIFINE D6, keyboard
shortcut and macro-pad integration. It defines stable preset IDs
(`original`, `robot`, `recurrent`, `radio_operator`, `deep_male`,
`female`, `demon`, `anime`) and validated DSP, mute, gain and AI commands.
The local named-pipe endpoint and actual AI preset engine are not implemented
yet; no network control listener is opened.

## Planned AI architecture
A separate, optional, visible local inference worker may be added after its license, dependencies, model format, and IPC design are reviewed. AI will be off by default. Disabling AI will retain the model for 30 seconds, then release it; **UNLOAD AI NOW** will request immediate shutdown. Worker failure/timeouts must fall back to the non-AI DSP path.

## Planned voice presets
Radio Operator, Deep Male, Female Voice, Robot, Demon, Anime Voice. These are targets for future tuning/model configurations, not currently implemented voice models.
