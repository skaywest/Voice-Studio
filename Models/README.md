# Local model library

This directory defines the local model-library contract. No voice models or model executables are bundled here.

## Planned user experience

- The user opens the in-app Voice Library and explicitly chooses a model to install.
- The app displays the model's name, voice category, language, size, required runtime, license, source, and checksum before installation.
- Downloads never start automatically or in the background.
- After installation, inference is local and does not require an internet connection.
- The user can select, remove, or inspect installed models from the app.
- The app reports model storage use and estimated/actual VRAM use when available.

## Catalog format

`catalog.json` is a versioned manifest. Each future model entry must include:

- `id`: stable, unique identifier.
- `name` and `description`: user-facing metadata.
- `category`: broad voice style (for example, radio, deep, robotic, or stylized).
- `language`: supported language or `multi`.
- `runtime`: explicitly supported local inference runtime and version range.
- `files`: exact filenames, sizes, and SHA-256 checksums.
- `source`: canonical source page and publisher/author.
- `license`: license identifier and a human-readable summary.
- `voice_rights`: provenance/consent statement for any voice resembling an identifiable person.

No entry may be marked installable until its source, redistribution terms, model license, file hashes, runtime compatibility, and voice-rights provenance have been reviewed. Unknown or unverifiable fields must be shown as unknown and block installation.

## Security and privacy rules

- No arbitrary installer, script, DLL, driver, or executable is accepted as a voice model.
- Model files are treated as untrusted data and stored in the app's dedicated model directory.
- Verify SHA-256 hashes before activation.
- No telemetry, account sign-in, cloud inference, or silent update checks.
- The user initiates every download, update, activation, and deletion.
- The AI worker remains a separate, visible, user-controlled process. It receives audio only through an explicitly designed local IPC channel.
- AI is disabled by default. Worker failure must return safely to the existing DSP/dry audio path.
- Never disable or bypass Windows security, FACEIT Anti-Cheat, Easy Anti-Cheat, or other protections.

## Voice likeness policy

The library may include original voices, public-domain material, and models whose license and voice rights permit the intended use. A model intended to imitate an identifiable real person requires documented permission. Do not present an unlicensed imitation of a streamer, actor, or other person as an official or authorized voice.

## Current status

The catalog is intentionally empty. The in-app model manager, downloader, integrity verifier, and inference worker are not implemented yet. This file is a design contract, not a claim that model installation already works.
