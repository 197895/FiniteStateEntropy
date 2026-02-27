#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <hdf5.h>
#include <H5PLextern.h>

#include "fse.h"
#include "huf.h"
#include "huf_frame.h"
#include "H5Z-FSE_config.h"

#define H5Z_FILTER_FSE_ID 32028

static int read_le32(const unsigned char* p, unsigned* out) {
    if (!p || !out) return 0;
    *out = (unsigned)p[0]
         | ((unsigned)p[1] << 8)
         | ((unsigned)p[2] << 16)
         | ((unsigned)p[3] << 24);
    return 1;
}

static int read_le64(const unsigned char* p, size_t* out) {
    unsigned lo, hi;
    if (!read_le32(p, &lo) || !read_le32(p + 4, &hi)) return 0;
    if (sizeof(size_t) < 8 && hi != 0) return 0;
    *out = ((size_t)hi << 32) | (size_t)lo;
    return 1;
}

static int parse_fse_raw_size(const void* cSrc, size_t cSrcSize, size_t* rawSize) {
    const unsigned char* p = (const unsigned char*)cSrc;
    if (!p || cSrcSize < 20 || !rawSize) return 0;
    if (p[0] != 'M' || p[1] != 'F' || p[2] != 'S' || p[3] != 'E') return 0;
    return read_le64(p + 8, rawSize);
}

static int resolve_codec_on_compress(size_t cd_nelmts,
                                     const unsigned int cd_values[],
                                     unsigned* codec) {
    if (!codec) return 0;
    if (cd_nelmts >= 1) {
        if (cd_values[0] == H5Z_FSE_CODEC_ANS || cd_values[0] == H5Z_FSE_CODEC_HUFFMAN) {
            *codec = cd_values[0];
            return 1;
        }
        return 0;
    }
    {
        const char* cfg = getenv("H5Z_FSE_CONFIG");
        if (cfg && cfg[0] != '\0' && H5Z_FSE_loadCodecFromConfig(cfg, codec)) {
            return 1;
        }
    }
    *codec = H5Z_FSE_CODEC_ANS;
    return 1;
}

static size_t H5Z_filter_fse(unsigned int flags,
                             size_t cd_nelmts,
                             const unsigned int cd_values[],
                             size_t nbytes,
                             size_t* buf_size,
                             void** buf) {
    unsigned codec = H5Z_FSE_CODEC_ANS;
    void* out = NULL;
    size_t out_cap = 0;
    size_t out_size = 0;

    if (!buf || !*buf || !buf_size) {
        fprintf(stderr, "[H5Zfse] invalid buffer args\n");
        return 0;
    }
    if (nbytes == 0) return nbytes;

    if (flags & H5Z_FLAG_REVERSE) {
        if (nbytes >= 4 && ((const unsigned char*)*buf)[0] == 'M' &&
            ((const unsigned char*)*buf)[1] == 'H' &&
            ((const unsigned char*)*buf)[2] == 'U' &&
            ((const unsigned char*)*buf)[3] == 'F') {
            codec = H5Z_FSE_CODEC_HUFFMAN;
        } else {
            codec = H5Z_FSE_CODEC_ANS;
        }

        if (codec == H5Z_FSE_CODEC_ANS) {
            if (!parse_fse_raw_size(*buf, nbytes, &out_cap)) {
                fprintf(stderr, "[H5Zfse] reverse ans: parse raw size failed\n");
                return 0;
            }
            out = malloc(out_cap);
            if (!out) {
                fprintf(stderr, "[H5Zfse] reverse ans: malloc failed (%zu)\n", out_cap);
                return 0;
            }
            out_size = FSE_decompress(out, out_cap, *buf, nbytes);
            if (FSE_isError(out_size) || out_size != out_cap) {
                fprintf(stderr, "[H5Zfse] reverse ans: decompress failed code=%zu out_cap=%zu\n", out_size, out_cap);
                free(out);
                return 0;
            }
        } else {
            size_t frame_size = 0;
            if (HUF_isError(HUF_frameInfo(*buf, nbytes, &frame_size, &out_cap))) {
                fprintf(stderr, "[H5Zfse] reverse huf: frame info failed\n");
                return 0;
            }
            if (frame_size != nbytes) {
                fprintf(stderr, "[H5Zfse] reverse huf: frame size mismatch (%zu vs %zu)\n", frame_size, nbytes);
                return 0;
            }
            out = malloc(out_cap);
            if (!out) {
                fprintf(stderr, "[H5Zfse] reverse huf: malloc failed (%zu)\n", out_cap);
                return 0;
            }
            out_size = HUF_frameDecompress(out, out_cap, *buf, nbytes);
            if (HUF_isError(out_size) || out_size != out_cap) {
                fprintf(stderr, "[H5Zfse] reverse huf: decompress failed code=%zu out_cap=%zu\n", out_size, out_cap);
                free(out);
                return 0;
            }
        }
    } else {
        if (!resolve_codec_on_compress(cd_nelmts, cd_values, &codec)) {
            fprintf(stderr, "[H5Zfse] forward: resolve codec failed, cd_nelmts=%zu\n", cd_nelmts);
            return 0;
        }
        if (codec == H5Z_FSE_CODEC_ANS) {
            out_cap = FSE_compressBound(nbytes);
            if (out_cap == 0) {
                fprintf(stderr, "[H5Zfse] forward ans: compress bound overflow, nbytes=%zu\n", nbytes);
                return 0;
            }
            out = malloc(out_cap);
            if (!out) {
                fprintf(stderr, "[H5Zfse] forward ans: malloc failed (%zu)\n", out_cap);
                return 0;
            }
            out_size = FSE_compress(out, out_cap, *buf, nbytes);
            if (FSE_isError(out_size) || out_size == 0) {
                fprintf(stderr, "[H5Zfse] forward ans: compress failed code=%zu cap=%zu nbytes=%zu\n", out_size, out_cap, nbytes);
                free(out);
                return 0;
            }
        } else {
            out_cap = HUF_frameCompressBound(nbytes);
            if (out_cap == 0) {
                fprintf(stderr, "[H5Zfse] forward huf: compress bound overflow, nbytes=%zu\n", nbytes);
                return 0;
            }
            out = malloc(out_cap);
            if (!out) {
                fprintf(stderr, "[H5Zfse] forward huf: malloc failed (%zu)\n", out_cap);
                return 0;
            }
            out_size = HUF_frameCompress(out, out_cap, *buf, nbytes);
            if (HUF_isError(out_size) || out_size == 0) {
                fprintf(stderr, "[H5Zfse] forward huf: compress failed code=%zu cap=%zu nbytes=%zu\n", out_size, out_cap, nbytes);
                free(out);
                return 0;
            }
        }
    }

    free(*buf);
    *buf = out;
    *buf_size = out_cap;
    return out_size;
}

static htri_t H5Z_can_apply_fse(hid_t dcpl_id, hid_t type_id, hid_t space_id) {
    (void)dcpl_id;
    (void)type_id;
    (void)space_id;
    return 1;
}

static herr_t H5Z_set_local_fse(hid_t dcpl_id, hid_t type_id, hid_t space_id) {
    (void)dcpl_id;
    (void)type_id;
    (void)space_id;
    return 1;
}

static const H5Z_class2_t H5Z_FSE[1] = {{
    H5Z_CLASS_T_VERS,
    (H5Z_filter_t)H5Z_FILTER_FSE_ID,
    1,
    1,
    "fse_ans_or_huffman",
    H5Z_can_apply_fse,
    H5Z_set_local_fse,
    H5Z_filter_fse
}};

H5PL_type_t H5PLget_plugin_type(void) {
    return H5PL_TYPE_FILTER;
}

const void* H5PLget_plugin_info(void) {
    return H5Z_FSE;
}
