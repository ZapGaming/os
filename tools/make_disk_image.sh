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

echo "Built $IMG"
mdir -i "$IMG" ::

if command -v qemu-img >/dev/null 2>&1; then
  qemu-img convert -f raw -O vmdk "$IMG" zapos_disk.vmdk
  echo "Also built zapos_disk.vmdk (for VMware/VirtualBox -- attach as an IDE disk)"
fi
