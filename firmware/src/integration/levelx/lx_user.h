#ifndef PF_LX_USER_H
#define PF_LX_USER_H

#include <stdint.h>

#define LX_STANDALONE_ENABLE
#define LX_NOR_ENABLE_CONTROL_BLOCK_FOR_DRIVER_INTERFACE
#define LX_NOR_ENABLE_MAPPING_BITMAP
#define LX_NOR_SECTOR_MAPPING_CACHE_SIZE 4096

/* Keep the on-flash ABI 32-bit on both RP MCUs and 64-bit host tests. */
#define VOID void
typedef char CHAR;
typedef char BOOL;
typedef unsigned char UCHAR;
typedef int INT;
typedef unsigned int UINT;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef short SHORT;
typedef unsigned short USHORT;
#define ALIGN_TYPE_DEFINED
#define ALIGN_TYPE uintptr_t

#endif
