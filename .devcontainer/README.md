# Dev Container Notes

This project uses a pinned ESP-IDF container image for reproducible builds.

## What is persisted

The dev container mounts named volumes for:
- `build/` artifacts
- ccache (`/root/.ccache`)
- pip cache (`/root/.cache/pip`)
- ESP-IDF tools/components (`/root/.espressif`)

This keeps host and container build state isolated while preserving build speed between container restarts.

## First open

1. Open the folder in container via VS Code.
2. Wait for `postCreateCommand` to finish.
3. Build with `idf.py build`.

## Flashing over USB (Linux)

The container is run with `--privileged` by default. If needed, add explicit devices in `.devcontainer/devcontainer.json` `runArgs`, for example:

- `"--device=/dev/ttyUSB0:/dev/ttyUSB0"`
- `"--device=/dev/ttyACM0:/dev/ttyACM0"`

Then rebuild/reopen the container.
