#!/usr/bin/env bash
# Builds Shelf Lite firmware for Heltec V3 and V4 and writes flashable .bin files to docs/firmware/
# Needs: arduino-cli with the esp32 core 2.0.17 and the libraries listed in README.md
set -euo pipefail
cd "$(dirname "$0")"
python3 tools/build_web.py
OUT=docs/firmware; mkdir -p "$OUT"
ESPTOOL="esptool.py"
[ -f "$HOME/Arduino/hardware/espressif/esp32/tools/esptool/esptool.py" ] && ESPTOOL="python3 $HOME/Arduino/hardware/espressif/esp32/tools/esptool/esptool.py"

# When the esp32 core is installed by hand (no Boards Manager), point the build at its tools.
CORE="$HOME/Arduino/hardware/espressif/esp32"
TOOLPROPS=()
if [ -d "$CORE/tools/xtensa-esp32s3-elf" ]; then
  TOOLPROPS=(--build-property "runtime.tools.esptool_py.path=$CORE/tools/esptool"
             --build-property "runtime.tools.xtensa-esp32s3-elf-gcc.path=$CORE/tools/xtensa-esp32s3-elf")
  command -v ctags >/dev/null && TOOLPROPS+=(--build-property "runtime.tools.ctags.path=$(dirname "$(command -v ctags)")")
fi

build() {  # board flash_size partitions cdc flags
  local b=$1 fs=$2 part=$3 cdc=$4 flags=$5
  cp "partitions/$part" HeltecShelfLite/partitions.csv
  local bd="build/$b"
  arduino-cli compile --fqbn "espressif:esp32:esp32s3:FlashSize=$fs,FlashMode=qio,PSRAM=disabled,USBMode=hwcdc,CDCOnBoot=$cdc,CPUFreq=240" \
    --build-property "compiler.cpp.extra_flags=$flags -DCONFIG_ASYNC_TCP_RUNNING_CORE=0 -DCONFIG_ASYNC_TCP_STACK_SIZE=12288" \
    --build-property "upload.maximum_size=2097152" \
    ${TOOLPROPS[@]+"${TOOLPROPS[@]}"} \
    --build-path "$bd" --warnings default HeltecShelfLite
  local boot_app0="$bd/boot_app0.bin"
  [ -f "$boot_app0" ] || boot_app0="$(find "$HOME/Arduino" "$HOME/.arduino15" -name boot_app0.bin 2>/dev/null | head -1)"
  $ESPTOOL --chip esp32s3 merge_bin -o "$OUT/shelf-lite-$b.bin" --flash_mode keep --flash_freq keep --flash_size "${fs}B" \
    0x0 "$bd/HeltecShelfLite.ino.bootloader.bin" 0x8000 "$bd/HeltecShelfLite.ino.partitions.bin" \
    0xe000 "$boot_app0" 0x10000 "$bd/HeltecShelfLite.ino.bin"
  cp "$bd/HeltecShelfLite.ino.bin" "$OUT/shelf-lite-$b-app-only.bin"
  rm -f HeltecShelfLite/partitions.csv
}

build v3 8M  v3_8mb.csv  default ""
build v4 16M v4_16mb.csv cdc     "-DSHELF_BOARD_V4"
ls -la "$OUT"
