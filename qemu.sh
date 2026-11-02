#!/bin/sh
set -e
. ./iso.sh

qemu-system-$(./target-triplet-to-arch.sh $HOST) -boot order=d -cdrom myos.iso -drive file=fat16.img,format=raw,if=ide,index=0 -M pc -m 2G
