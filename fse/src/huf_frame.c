/* ******************************************************************
 * HUF framed helpers
 ****************************************************************** */

#include <string.h> /* memcpy, memset */

#include "mem.h"
#include "huf.h"
#include "huf_frame.h"
#include "error_private.h"

#define HUF_FRAME_BLOCK_SIZE  (128U * 1024U)
#define HUF_FRAME_HEADER_SIZE 20U   /* magic(4) + blockSize(4) + rawSize(8) + blockCount(4) */
#define HUF_BLOCK_HEADER_SIZE 9U    /* mode(1) + rawSize(4) + storedSize(4) */

typedef enum {
    HUF_BLOCKMODE_HUF = 0,
    HUF_BLOCKMODE_RAW = 1,
    HUF_BLOCKMODE_RLE = 2
} HUF_blockMode_t;

static size_t HUF_blockCount(size_t srcSize) {
    if (srcSize == 0) return 0;
    return (srcSize + HUF_FRAME_BLOCK_SIZE - 1) / HUF_FRAME_BLOCK_SIZE;
}

static void HUF_writeFrameHeader(BYTE* dst, size_t srcSize, U32 blockCount) {
    dst[0] = 'M';
    dst[1] = 'H';
    dst[2] = 'U';
    dst[3] = 'F';
    MEM_writeLE32(dst + 4, HUF_FRAME_BLOCK_SIZE);
    MEM_writeLE64(dst + 8, (U64)srcSize);
    MEM_writeLE32(dst + 16, blockCount);
}

static size_t HUF_parseFrame(const void* cSrc,
                             size_t cSrcSize,
                             size_t* frameSize,
                             size_t* rawSize) {
    const BYTE* const ip = (const BYTE*)cSrc;
    size_t offset = HUF_FRAME_HEADER_SIZE;
    size_t rawAcc = 0;
    U32 blockCount;
    size_t i;

    if (frameSize) *frameSize = 0;
    if (rawSize) *rawSize = 0;

    if (!cSrc) return ERROR(srcSize_wrong);
    if (cSrcSize < HUF_FRAME_HEADER_SIZE) return ERROR(srcSize_wrong);
    if (ip[0] != 'M' || ip[1] != 'H' || ip[2] != 'U' || ip[3] != 'F') return ERROR(corruption_detected);
    if (MEM_readLE32(ip + 4) != HUF_FRAME_BLOCK_SIZE) return ERROR(corruption_detected);
    if (MEM_readLE64(ip + 8) > (U64)(size_t)-1) return ERROR(srcSize_wrong);

    blockCount = MEM_readLE32(ip + 16);
    for (i = 0; i < blockCount; ++i) {
        U32 blockRaw;
        U32 blockStored;
        BYTE mode;
        if (offset > cSrcSize || cSrcSize - offset < HUF_BLOCK_HEADER_SIZE) return ERROR(corruption_detected);
        mode = ip[offset];
        blockRaw = MEM_readLE32(ip + offset + 1);
        blockStored = MEM_readLE32(ip + offset + 5);
        if (mode > HUF_BLOCKMODE_RLE) return ERROR(corruption_detected);
        if (blockStored > cSrcSize - (offset + HUF_BLOCK_HEADER_SIZE)) return ERROR(corruption_detected);
        if (rawAcc > (size_t)-1 - (size_t)blockRaw) return ERROR(corruption_detected);
        rawAcc += (size_t)blockRaw;
        offset += HUF_BLOCK_HEADER_SIZE + (size_t)blockStored;
    }

    if (rawAcc != (size_t)MEM_readLE64(ip + 8)) return ERROR(corruption_detected);
    if (offset != cSrcSize) return ERROR(corruption_detected);
    if (frameSize) *frameSize = offset;
    if (rawSize) *rawSize = rawAcc;
    return 0;
}

size_t HUF_frameCompressBound(size_t srcSize) {
    size_t const blockCount = HUF_blockCount(srcSize);
    size_t const perBlockBound = HUF_BLOCK_HEADER_SIZE + HUF_compressBound(HUF_FRAME_BLOCK_SIZE);
    size_t const maxSizeT = (size_t)-1;
    if (blockCount > (maxSizeT - HUF_FRAME_HEADER_SIZE) / perBlockBound) return 0;
    return HUF_FRAME_HEADER_SIZE + blockCount * perBlockBound;
}

size_t HUF_frameCompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    BYTE* const ostart = (BYTE*)dst;
    BYTE* op = ostart;
    BYTE* const oend = ostart + dstCapacity;
    const BYTE* ip = (const BYTE*)src;
    size_t remaining = srcSize;
    size_t const blockCount = HUF_blockCount(srcSize);
    size_t blockNb;

    if (dstCapacity < HUF_FRAME_HEADER_SIZE) return ERROR(dstSize_tooSmall);
    if (blockCount > (U32)(-1)) return ERROR(srcSize_wrong);

    HUF_writeFrameHeader(op, srcSize, (U32)blockCount);
    op += HUF_FRAME_HEADER_SIZE;

    for (blockNb = 0; blockNb < blockCount; ++blockNb) {
        size_t const rawSize = remaining < HUF_FRAME_BLOCK_SIZE ? remaining : HUF_FRAME_BLOCK_SIZE;
        BYTE* const bh = op;
        size_t cSize;
        U32 storedSize;
        BYTE mode;

        if ((size_t)(oend - op) < HUF_BLOCK_HEADER_SIZE) return ERROR(dstSize_tooSmall);
        op += HUF_BLOCK_HEADER_SIZE;

        cSize = HUF_compress(op, (size_t)(oend - op), ip, rawSize);
        if (HUF_isError(cSize)) return cSize;

        if (cSize > 1) {
            mode = HUF_BLOCKMODE_HUF;
            storedSize = (U32)cSize;
            op += cSize;
        } else if (cSize == 1) {
            mode = HUF_BLOCKMODE_RLE;
            storedSize = 1;
            if ((size_t)(oend - op) < 1) return ERROR(dstSize_tooSmall);
            op[0] = ip[0];
            op += 1;
        } else {
            mode = HUF_BLOCKMODE_RAW;
            storedSize = (U32)rawSize;
            if ((size_t)(oend - op) < rawSize) return ERROR(dstSize_tooSmall);
            memcpy(op, ip, rawSize);
            op += rawSize;
        }

        bh[0] = mode;
        MEM_writeLE32(bh + 1, (U32)rawSize);
        MEM_writeLE32(bh + 5, storedSize);

        ip += rawSize;
        remaining -= rawSize;
    }

    return (size_t)(op - ostart);
}

size_t HUF_frameDecompress(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize) {
    const BYTE* const ip = (const BYTE*)cSrc;
    BYTE* const ostart = (BYTE*)dst;
    BYTE* op = ostart;
    BYTE* const oend = ostart + dstCapacity;
    U32 const blockCount = MEM_readLE32(ip + 16);
    size_t offset = HUF_FRAME_HEADER_SIZE;
    size_t rawAcc = 0;
    size_t i;

    if (HUF_isError(HUF_parseFrame(cSrc, cSrcSize, NULL, NULL))) return ERROR(corruption_detected);
    if ((size_t)MEM_readLE64(ip + 8) > dstCapacity) return ERROR(dstSize_tooSmall);

    for (i = 0; i < blockCount; ++i) {
        BYTE mode = ip[offset];
        U32 blockRaw = MEM_readLE32(ip + offset + 1);
        U32 blockStored = MEM_readLE32(ip + offset + 5);
        const BYTE* blockPayload = ip + offset + HUF_BLOCK_HEADER_SIZE;
        offset += HUF_BLOCK_HEADER_SIZE;

        if ((size_t)(oend - op) < (size_t)blockRaw) return ERROR(dstSize_tooSmall);

        if (mode == HUF_BLOCKMODE_HUF) {
            size_t const regenerated = HUF_decompress(op, blockRaw, blockPayload, blockStored);
            if (HUF_isError(regenerated)) return regenerated;
            if (regenerated != blockRaw) return ERROR(corruption_detected);
        } else if (mode == HUF_BLOCKMODE_RAW) {
            if (blockStored != blockRaw) return ERROR(corruption_detected);
            memcpy(op, blockPayload, blockRaw);
        } else if (mode == HUF_BLOCKMODE_RLE) {
            if (blockStored != 1 || blockRaw == 0) return ERROR(corruption_detected);
            memset(op, blockPayload[0], blockRaw);
        } else {
            return ERROR(corruption_detected);
        }

        op += blockRaw;
        rawAcc += blockRaw;
        offset += blockStored;
    }

    if (rawAcc != (size_t)MEM_readLE64(ip + 8)) return ERROR(corruption_detected);
    if (offset != cSrcSize) return ERROR(corruption_detected);
    return rawAcc;
}

size_t HUF_frameInfo(const void* cSrc, size_t cSrcSize, size_t* frameSize, size_t* rawSize) {
    return HUF_parseFrame(cSrc, cSrcSize, frameSize, rawSize);
}

