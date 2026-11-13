# Sourced by the image gates: the ELF of the env just built, and the toolchain's nm that reads it.
ENV_NAME="${1:-PixC_V1}"
ELF=".pio/build/${ENV_NAME}/firmware.elf"
BIN=".pio/build/${ENV_NAME}/firmware.bin"
test -f "$ELF" || { echo "error: $ELF is missing, nothing to inspect"; exit 1; }
# Platform espressif32 8.0.0 has one Xtensa toolchain for every chip (toolchain-xtensa-esp-elf, GCC
# 14.2); it was toolchain-xtensa-esp32s3 (GCC 8.4) on IDF 4.4.
NM="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"
test -x "$NM" || { echo "error: no xtensa-esp32s3 nm under ${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-xtensa-esp-elf: the build did not install the toolchain this gate reads with"; exit 1; }
