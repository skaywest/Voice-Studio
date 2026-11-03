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
