# Sourced by the image gates: the ELF of the env just built, and the toolchain's nm that reads it.
ENV_NAME="${1:-PixC_V1}"
ELF=".pio/build/${ENV_NAME}/firmware.elf"
BIN=".pio/build/${ENV_NAME}/firmware.bin"
test -f "$ELF" || { echo "error: $ELF is missing, nothing to inspect"; exit 1; }
NM=$(ls "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"/packages/toolchain-xtensa-esp32s3/bin/*-nm 2>/dev/null | head -1 || true)
test -x "$NM" || { echo "error: no xtensa-esp32s3 nm under ${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages: the build did not install the toolchain this gate reads with"; exit 1; }
