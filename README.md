# rockfuse

### Introduction

rockfuse is a FUSE filesystem driver for mounting RockChip (e)MMC over USB 
through the `rockusb` protocol. The `rockusb` protocol is part of the RockChip 
recovery ROM as well as part of rockchip u-boot. This is useful to quickly loopback
mount the various partitions if you want to make minor updates. (Replace your kernel image, DTB, or files in the root filesystem)

This small tool was mostly written out of a personal necessity to be able to test various things on a [Pinebook Pro](https://www.pine64.org/pinebook-pro/). As such, it cuts a lot of corners and is currently hardcoded for the rockusb USB VID/PID of a RK3399. Sensible PR's are welcome.



### Requirements

* libusb-1.0
* libfuse-2.9 (someone port to libfuse3 plz? :))


### Build instructions
```
make
```

### Usage instructions

To start `rockusb` from u-boot, type this into a u-boot shell:
```
rockusb 0 mmc 0
```

now you can simply invoke `rockfuse` with your mountpoint of choice.

```
./rockfuse /mnt/point
```

`/mnt/point` should now be populated with the following entries:

```
$ ls -la /mnt/point
total 4
drwxr-xr-x 2 root root           0 Jan  1  1970 .
drwxrwxr-x 4 user user        4096 Nov  7 22:50 ..
-rw-rw-rw- 1 root root   117440512 Jan  1  1970 boot.img
-rw-rw-rw- 1 root root 62537072640 Jan  1  1970 full.img
-rw-rw-rw- 1 root root     8355840 Jan  1  1970 loader1.img
-rw-rw-rw- 1 root root     4194304 Jan  1  1970 loader2.img
-rw-rw-rw- 1 root root 62402854912 Jan  1  1970 root.img
-rw-rw-rw- 1 root root     4194304 Jan  1  1970 trust.img

```

The various `img` file entries point to specific portions of the eMMC nand. `full.img` is the full eMMC nand.

Lets say we want to mount the boot partition to replace our kernel we could now do something like:
```
$ mount -t vfat -o loop /mnt/point/boot.img /mnt/boot_fat
```
