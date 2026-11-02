#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKING_DIR=$PWD
export PATH="$HOME/opt/cross/bin:$PATH"

usage() {
    echo "Usage: $0 [-o output.elf] [--install[=fat16.img]] source.c|source.cpp [source2 ...]" >&2
}

sources=()
output=
install_image=
use_cxx=0
while (($#)); do
    case "$1" in
        --install)
            install_image="$ROOT_DIR/fat16.img"
            shift
            ;;
        --install=*)
            install_image=${1#--install=}
            if [[ -z "$install_image" ]]; then
                echo "FAT16 image path cannot be empty." >&2
                exit 2
            fi
            if [[ "$install_image" != /* ]]; then
                install_image="$WORKING_DIR/$install_image"
            fi
            shift
            ;;
        -o)
            if (($# < 2)) || [[ -z "$2" ]]; then
                usage
                exit 2
            fi
            output=$2
            shift 2
            ;;
        --)
            shift
            while (($#)); do
                sources+=("$1")
                shift
            done
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
        *)
            sources+=("$1")
            shift
            ;;
    esac
done

if ((${#sources[@]} == 0)); then
    usage
    exit 2
fi

for i in "${!sources[@]}"; do
    source=${sources[$i]}
    if [[ "$source" != /* ]]; then
        source="$WORKING_DIR/$source"
    fi
    if [[ ! -f "$source" ]]; then
        echo "Source file not found: ${sources[$i]}" >&2
        exit 1
    fi
    case "${source##*.}" in
        c)
            ;;
        cc|cpp|cxx|C|CXX)
            use_cxx=1
            ;;
        *)
            echo "Unsupported source type: ${sources[$i]}" >&2
            exit 2
            ;;
    esac
    sources[$i]=$source
done

if [[ -z "$output" ]]; then
    first_source=${sources[0]##*/}
    output="${first_source%.*}.elf"
fi
if [[ "$output" != /* ]]; then
    output="$WORKING_DIR/$output"
fi

cd "$ROOT_DIR"
. ./config.sh

if ! command -v "${HOST}-gcc" >/dev/null 2>&1; then
    echo "Cross compiler ${HOST}-gcc not found. Add it to PATH or install it under \$HOME/opt/cross/bin." >&2
    exit 1
fi
if ((use_cxx)) && ! command -v "${HOST}-g++" >/dev/null 2>&1; then
    echo "C++ cross compiler ${HOST}-g++ not found." >&2
    exit 1
fi

export CXX="${HOST}-g++"
"$MAKE" -C "$ROOT_DIR/libc" DESTDIR="$SYSROOT" install

driver="${HOST}-gcc"
if ((use_cxx)); then
    driver="${HOST}-g++"
fi

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/naroch-elf.XXXXXX")
cleanup() {
    find "$temporary_directory" -maxdepth 1 -type f -delete
    rmdir "$temporary_directory"
}
trap cleanup EXIT

objects=()
for i in "${!sources[@]}"; do
    source=${sources[$i]}
    object="$temporary_directory/$i.o"
    case "${source##*.}" in
        c)
            "${HOST}-gcc" \
                --sysroot="$SYSROOT" -m32 -O2 -Wall -Wextra \
                -std=gnu11 -ffreestanding -fno-pie -fno-pic \
                -fno-stack-protector -fno-builtin \
                -I"$ROOT_DIR/libc/include" -c "$source" -o "$object"
            ;;
        *)
            "${HOST}-g++" \
                --sysroot="$SYSROOT" -m32 -O2 -Wall -Wextra \
                -std=gnu++17 -ffreestanding -fno-pie -fno-pic \
                -fno-stack-protector -fno-builtin -fno-exceptions -fno-rtti \
                -I"$ROOT_DIR/libc/include" -c "$source" -o "$object"
            ;;
    esac
    objects+=("$object")
done

"$driver" \
    --sysroot="$SYSROOT" -m32 \
    -nostdlib -Wl,-T,"$ROOT_DIR/libc/user/user.ld" \
    "$SYSROOT/usr/lib/crt0.o" "${objects[@]}" \
    -L"$SYSROOT/usr/lib" -Wl,--start-group -lu -lgcc -Wl,--end-group \
    -o "$output"

echo "Built $output"

if [[ -n "$install_image" ]]; then
    if [[ ! -f "$install_image" ]]; then
        echo "FAT16 image not found: $install_image" >&2
        exit 1
    fi
    if ! command -v mcopy >/dev/null 2>&1; then
        echo "mcopy is required to install into a FAT16 image (install mtools)." >&2
        exit 1
    fi
    if ! mcopy -o -i "$install_image" "$output" "::${output##*/}"; then
        echo "Failed to copy $output into FAT16 image: $install_image" >&2
        exit 1
    fi
    echo "Installed ${output##*/} into $install_image"
fi
