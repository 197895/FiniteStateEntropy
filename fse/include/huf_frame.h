/* ******************************************************************
 * HUF framed helpers
 * Copyright (c) 2026
 *
 * This source code is licensed under both the BSD-style license and GPLv2.
 ****************************************************************** */

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef HUF_FRAME_H
#define HUF_FRAME_H

#include <stddef.h>

/* Framed Huffman block format (magic "MHUF"), similar to FSE frame wrapper. */
size_t HUF_frameCompressBound(size_t srcSize);
size_t HUF_frameCompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
size_t HUF_frameDecompress(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize);

/* Parse frame metadata without decompressing payload. */
size_t HUF_frameInfo(const void* cSrc, size_t cSrcSize, size_t* frameSize, size_t* rawSize);

#endif /* HUF_FRAME_H */

#if defined(__cplusplus)
}
#endif

