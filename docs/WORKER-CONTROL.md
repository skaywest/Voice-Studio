# Worker control prototype

## Current behaviour
- The VST3 processor owns the Windows Worker controller, independently of the editor window.
- The editor starts the Worker only after an explicit file selection and stops it on request.
- The editor polls status on JUCE's message thread; the audio callback never performs process creation, pipe I/O, waits, or UI work.
- The processor detaches the realtime mapping and shuts down the Worker when the processor is destroyed.
- The Worker is transport-only. It announces READY, services IPC/audio transport, and rejects the AI start command as unsupported. No inference is claimed.

## Getting the executable
The Windows CI artifact includes:
- `VoiceStudio.vst3`
- `VoiceStudioWorker.exe`
- `network-import-audit.txt`

Choose the exact `VoiceStudioWorker.exe` from the artifact in the plugin's Effects page. The plugin verifies the filename and launches the selected executable directly (no command shell).

## Known limitation
The Worker does not load RVC/PyTorch or any other model. AI conversion remains unavailable. The UI must not label READY as AI processing.


## Thread ownership and UI responsiveness

The editor never calls `WindowsWorkerController` directly. Selecting the executable queues a launch request to a processor-owned control thread and returns immediately. That thread owns all controller, named-pipe, process, and mapping lifecycle calls, including the bounded startup handshake and status polling. The editor timer reads only atomic status snapshots; it does not poll IPC.

Stopping queues a stop request. Processor destruction signals the control thread and joins it; the thread shuts down the Worker, detaches the realtime mapping after active audio leases drain, and only then releases the mapping and process resources. The audio callback remains independent of this control thread and never waits for it.

The Worker currently remains transport-only and rejects AI inference requests. A READY status confirms IPC startup, not an available voice model or conversion engine.


## External controls: FIFINE D6, macro pads and hotkeys

A versioned, transport-neutral command contract is defined in
`Source/AudioIpc/VoiceControlCommandAbi.h`. It is intended to be the stable
payload for a future owner-only local Windows named-pipe endpoint. The endpoint
is not exposed yet; no network listener or remote-control surface is created.

Preset IDs are permanent protocol values:
- `0` — `original`
- `1` — `robot`
- `2` — `recurrent`
- `3` — `radio_operator`
- `4` — `deep_male`
- `5` — `female`
- `6` — `demon`
- `7` — `anime`

The command ABI also reserves validated operations for DSP enable, mute,
output gain (-18 to +12 dB), and AI enable. Messages include a version, fixed
size, request ID, flags and reserved fields; malformed, unknown, non-finite,
and out-of-range values are rejected.

This is the integration foundation, not a claim that D6 is already connected.
Once the local endpoint and preset engine exist, D6 software actions can send
`selectPreset(robot)`, `selectPreset(recurrent)`, or `selectPreset(original)`.
The same contract can be used by keyboard shortcuts and other macro pads.
At present, preset IDs are protocol names only: the Robot and Recurrent AI
voice models and inference engine are not installed or active.
