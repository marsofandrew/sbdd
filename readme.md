# Simple Block Device Driver
Implementation of Linux Kernel 5.4.X simple block device.

## Build
- regular:
`$ make`
- with requests debug info:
uncomment `CFLAGS_sbdd.o := -DDEBUG` in `Kbuild`

## Clean
`$ make clean`

## References
- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Linux Kernel Development](https://rlove.org)
- [Linux Kernel Teaching](https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html)
- [Linux Kernel Sources](https://github.com/torvalds/linux)


# Proxy Block Device Driver
Implementation of proxy device driver

Supported parameters:
- `device_path` - path to device to which bio requests will be forwarded.
- `capacity_mib`, if 0 then capacity will be the same as capacity of device provided in device_path.
 Default: 100

## Build
- regular:
  `$ make`
- with requests debug info:
  uncomment `CFLAGS_sbdd.o := -DDEBUG` in `Kbuild`

## Clean
`$ make clean`

# Tests

Repository contains smoke test for both drivers and could be executed in the following way:
1. Load module
2. Run tests `./test.sh {path to created device}`
3. Unload module

# Notes

I've created separate module for proxying bio, because it could be used over sbdd driver as well as
over any other block device drivers
