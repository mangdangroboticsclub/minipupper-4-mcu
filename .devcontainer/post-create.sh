#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x /root/.espressif/python_env/idf5.5_py3.12_env/bin/python ]]; then
	echo "ESP-IDF tools are missing; installing esp32s3 toolchain..."
	/opt/esp/idf/install.sh esp32s3
fi

# Install PWA (Svelte + Vite + Tailwind) dependencies
PWA_DIR="${PWD}/pwa-claw"
if [[ -f "${PWA_DIR}/package.json" ]]; then
	echo "Installing PWA dependencies..."
	cd "${PWA_DIR}"
	npm install
	cd - > /dev/null
fi

echo "Dev container setup complete."
