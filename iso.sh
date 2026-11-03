#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp $SYSROOT/boot/myos.kernel isodir/boot/myos.kernel
python3 - "$SYSROOT/boot/myos.kernel" isodir/boot <<'PY'
import pathlib
import struct
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
boot_dir = pathlib.Path(sys.argv[2])
header = source.find(b"\x02\xb0\xad\x1b", 0, 8192)
if header < 0:
    raise SystemExit("iso.sh: Multiboot header was not found.")
flags = struct.unpack_from("<I", source, header + 4)[0]
if not flags & (1 << 2):
    raise SystemExit("iso.sh: kernel does not request framebuffer information.")

modes = {
    "default": (0, 0, 0),
    "800x600": (800, 600, 32),
    "1024x768": (1024, 768, 32),
    "1280x720": (1280, 720, 32),
    "1280x800": (1280, 800, 32),
    "1920x1080": (1920, 1080, 32),
}
for name, (width, height, depth) in modes.items():
    image = bytearray(source)
    struct.pack_into("<III", image, header + 36, width, height, depth)
    (boot_dir / f"myos-{name}.kernel").write_bytes(image)
PY
cat > isodir/boot/grub/grub.cfg << EOF
set default=0
set timeout=5
insmod all_video
insmod gfxterm

menuentry "Naroch (GRUB default mode)" {
	set gfxmode=auto
	terminal_output gfxterm
	set gfxpayload=keep
	multiboot /boot/myos-default.kernel
	boot
}

menuentry "Naroch (800x600)" {
	set gfxmode=800x600x32
	terminal_output gfxterm
	set gfxpayload=keep
	multiboot /boot/myos-800x600.kernel
	boot
}

menuentry "Naroch (1024x768)" {
	set gfxmode=1024x768x32
	terminal_output gfxterm
	set gfxpayload=keep
	multiboot /boot/myos-1024x768.kernel
	boot
}

menuentry "Naroch (1280x720)" {
	set gfxmode=1280x720x32
	terminal_output gfxterm
	set gfxpayload=keep
	multiboot /boot/myos-1280x720.kernel
	boot
}

menuentry "Naroch (1280x800)" {
	set gfxmode=1280x800x32
	terminal_output gfxterm
	set gfxpayload=keep
	multiboot /boot/myos-1280x800.kernel
	boot
}

menuentry "Naroch (1920x1080)" {
	set gfxmode=1920x1080x32
	terminal_output gfxterm
	set gfxpayload=keep
	multiboot /boot/myos-1920x1080.kernel
	boot
}
EOF
grub-mkrescue -o myos.iso isodir
