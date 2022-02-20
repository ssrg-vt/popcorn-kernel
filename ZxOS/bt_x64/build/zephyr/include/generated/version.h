#ifndef _KERNEL_VERSION_H_
#define _KERNEL_VERSION_H_

/*  values come from cmake/version.cmake */

#define ZEPHYR_VERSION_CODE 132864
#define ZEPHYR_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))

#define KERNELVERSION          0x2070000
#define KERNEL_VERSION_NUMBER  0x20700
#define KERNEL_VERSION_MAJOR   2
#define KERNEL_VERSION_MINOR   7
#define KERNEL_PATCHLEVEL      0
#define KERNEL_VERSION_STRING  "2.7.0-rc2"

#endif /* _KERNEL_VERSION_H_ */
