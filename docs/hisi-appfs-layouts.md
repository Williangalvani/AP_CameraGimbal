# Hi3516DV500 appfs layouts

The hisi firmware replaces only the camera's application partition (appfs).
The boot loader, kernel, rootfs and the vendor's data partitions stay as the
vendor flashed them. An appfs image (`make hisi_appfs`) is a vendor update
container with a single entry that names the partition to write, for example
`/dev/mtd4`. The container's header also carries a device code,
DEV/SDK/PLATFORM, which must match `[UPDATE_SET]` in the camera's
`/appfs/fconfigs/param/updatetype.ini`.

Where appfs lives depends on which vendor firmware line was flashed onto the
camera, not on the hardware. The same unit has carried both.

## The two layouts

| | U35.5 line (e.g. `18.010.U35.5_0054_V16`) | MAIN line (e.g. `18.010.25.3_MAIN_V9(240601)`) |
|---|---|---|
| Device code | 19/2/51 | 19/2/35 |
| `mtd4` | `appfs`, 36 MiB, UBI with one volume holding a squashfs | `fconfigs`, 16 MiB: vendor parameters, network settings, logins, `updatetype.ini` |
| `mtd5` | `fconfigs`, 12 MiB | `appfs`, 48 MiB, a raw squashfs mounted from `mtdblock5` |
| Our image | UBI in `/dev/mtd4`, up to 288 erase blocks | squashfs padded with `0xff` to whole 128 KiB erase blocks, in `/dev/mtd5`, up to 384 |
| `make hisi_appfs` output | `build/HISI_AP_<version>_<hash>_appfs.img` | `build/HISI_AP_<version>_<hash>_appfs_p35.img` |

To tell which one a camera has, from its shell:

```sh
cat /proc/mtd
grep _TYPE /appfs/fconfigs/param/updatetype.ini
```

## Why the partition matters

The rootfs updater (TCP 9091), the vendor web page and our Status page all
write whatever partition the image names. On a MAIN camera, `/dev/mtd4` is
`fconfigs`, so an image built for the U35.5 layout would overwrite the
camera's settings, including its IP address and logins, with a UBI image of
our application. The updater and telnet start from the rootfs and would still
run, but the camera would come up without its network settings.

This happened in practice: a camera that had been flashed with our U35.5-layout
images was reflashed with the vendor's MAIN V9 firmware, and the next upload of
our usual image stalled at 0%. The updater compares the image's device code
with its own (19/2/35 against our 19/2/51) and refused the image. That check is
what kept `fconfigs` intact.

## What the tools check

`include/apcam/hisi_firmware.h` (`APPFS_LAYOUTS`, used by the web uploader) and
`tools/hisi_firmware.py` (`LAYOUTS`, used by the build and `check`) hold the
same table. The device code picks the layout, and an image must match it:

- the device code, checked first;
- exactly one entry, naming that layout's partition;
- the reboot flag set, header flag byte `0x1e` zero, and a matching entry CRC;
- for 19/2/51, a valid UBI image whose volume holds a squashfs;
- for 19/2/35, a squashfs whose `bytes_used` fits in the payload, padded to
  whole erase blocks, and no larger than the partition.

The Status page reads the device code from the camera's `updatetype.ini`, and
falls back to 19/2/51 if it can't. It accepts uploads of up to 50331892 bytes,
the size of the larger (MAIN) layout's image. Each layout's own limit still
applies after that. `tests/test_hisi_firmware.py` checks that the Python and
C++ checkers agree, including on images built for the wrong layout.

## Building and flashing

```sh
make hisi_appfs HISI_DEVICE_CODE=19/2/35   # MAIN layout; the default is 19/2/51
python3 tools/hisi_firmware.py check --device-code 19/2/35 build/HISI_AP_…_appfs_p35.img
```

Any of the three uploaders then takes the image. On the MAIN firmware, the
updater writes only the blocks the image covers. The rest of `mtd5` keeps
older data, which the kernel never reads, because squashfs stops at its own
`bytes_used`. To confirm a flash, compare the MD5 of the first N erase blocks
of the partition with the image's payload:
`dd if=/dev/mtd5 bs=131072 count=N | md5sum`.
