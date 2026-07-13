#!/bin/sh
# Builds zapos_disk.img: a 64MB raw FAT32 disk image (no partition table --
# a "superfloppy" layout, so the FAT32 boot sector sits at LBA 0) with a
# few sample files for the File Manager / filesystem demo. Requires mtools
# (mformat/mmd/mcopy) -- works without root or loop devices.
set -e

cd "$(dirname "$0")/.."
IMG=zapos_disk.img
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mformat -F -v ZAPOS -i "$IMG" ::

cat > "$STAGE/README.TXT" <<'EOF'
Welcome to the ZapOS filesystem.

This file lives on a real FAT32 disk image (zapos_disk.img),
read by a from-scratch ATA PIO driver and a from-scratch FAT32
driver -- no existing filesystem code was used.

Open the File Manager window to browse around.
EOF

cat > "$STAGE/ABOUTFS.TXT" <<'EOF'
ZapOS filesystem support:
 - drivers/ata.c  : ATA PIO disk driver (IDENTIFY, PIO read/write)
 - fs/fat32.c     : FAT32 driver (BPB parsing, FAT walking,
                    directory listing, file read + write)
 - gui/compositor : the File Manager window that browses it

Try writing NOTES.TXT from within ZapOS (via the File Manager)
and rebooting -- it persists.
EOF

: > "$STAGE/NOTES.TXT"

mmd -i "$IMG" ::/DOCS
mcopy -i "$IMG" "$STAGE/README.TXT" ::/README.TXT
mcopy -i "$IMG" "$STAGE/NOTES.TXT" ::/NOTES.TXT
mcopy -i "$IMG" "$STAGE/ABOUTFS.TXT" ::/DOCS/ABOUTFS.TXT

# A short 48kHz/16-bit/stereo test tone for the File Manager's audio
# playback (net/ac97.c only supports that exact format -- no
# resampling). Generated on the fly rather than committed as a binary
# asset; skipped gracefully if python3 isn't available.
if command -v python3 >/dev/null 2>&1; then
  python3 - "$STAGE/SONG.WAV" <<'PYEOF'
import wave, struct, math, sys

rate = 48000
seconds = 2.5
amplitude = 12000

with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    n = int(rate * seconds)
    for i in range(n):
        t = i / rate
        # a little melody instead of a flat tone: three notes in sequence
        freq = 440.0 if t < seconds / 3 else (554.37 if t < 2 * seconds / 3 else 659.25)
        sample = int(amplitude * math.sin(2 * math.pi * freq * t))
        frames += struct.pack("<hh", sample, sample)
    w.writeframes(bytes(frames))
PYEOF
  mcopy -i "$IMG" "$STAGE/SONG.WAV" ::/SONG.WAV
  echo "Added SONG.WAV (48kHz/16-bit/stereo test tone)"
else
  echo "python3 not found -- skipping SONG.WAV test asset"
fi

# A tiny standalone ELF32 executable for the File Manager's "run .ELF"
# feature (kernel/elf.c), built fresh here rather than committed as a
# binary asset -- same freestanding flags as the kernel itself, linked
# at the loader's fixed user-program base (userprogs/user.ld).
if command -v gcc >/dev/null 2>&1 && command -v ld >/dev/null 2>&1; then
  gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -nostdlib -O2 \
      -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
      -c userprogs/hello.c -o "$STAGE/hello.o"
  ld -m elf_i386 -T userprogs/user.ld -nostdlib -o "$STAGE/TEST.ELF" "$STAGE/hello.o"
  mcopy -i "$IMG" "$STAGE/TEST.ELF" ::/TEST.ELF
  echo "Added TEST.ELF (sample ELF32 program for the File Manager to run)"
else
  echo "gcc/ld not found -- skipping TEST.ELF test asset"
fi

echo "Built $IMG"
mdir -i "$IMG" ::

if command -v qemu-img >/dev/null 2>&1; then
  qemu-img convert -f raw -O vmdk "$IMG" zapos_disk.vmdk
  echo "Also built zapos_disk.vmdk (for VMware/VirtualBox -- attach as an IDE disk)"
fi
