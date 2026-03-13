#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#include "fse.h"
#include "huf.h"
#include "huf_frame.h"

typedef enum {
    BENCH_ALGO_FSE_ANS = 0,
    BENCH_ALGO_FSE_HUFFMAN = 1
} bench_algo_t;

typedef struct {
    size_t offset;
    size_t len;
} chunk_t;

typedef struct {
    double comp_ms;
    double decomp_ms;
    size_t comp_bytes;
    int ok;
    char error[256];
} run_stats_t;

static double elapsed_ms(clock_t start, clock_t end) {
    return (double)(end - start) * 1000.0 / (double)CLOCKS_PER_SEC;
}

static int load_file(const char* path, uint8_t** data, size_t* size) {
    FILE* f = fopen(path, "rb");
    long sz;
    uint8_t* buf;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (uint8_t*)malloc((size_t)sz);
    if (!buf && sz > 0) {
        fclose(f);
        return 0;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data = buf;
    *size = (size_t)sz;
    return 1;
}

static int parse_algo(const char* s, bench_algo_t* algo) {
    if (!strcmp(s, "fse_ans")) {
        *algo = BENCH_ALGO_FSE_ANS;
        return 1;
    }
    if (!strcmp(s, "fse_huffman")) {
        *algo = BENCH_ALGO_FSE_HUFFMAN;
        return 1;
    }
    return 0;
}

static size_t algo_compress_bound(bench_algo_t algo, size_t src_size) {
    if (algo == BENCH_ALGO_FSE_ANS) {
        return FSE_compressBound(src_size);
    }
    return HUF_frameCompressBound(src_size);
}

static size_t algo_compress(bench_algo_t algo,
                            void* dst,
                            size_t dst_capacity,
                            const void* src,
                            size_t src_size) {
    if (algo == BENCH_ALGO_FSE_ANS) {
        return FSE_compress(dst, dst_capacity, src, src_size);
    }
    return HUF_frameCompress(dst, dst_capacity, src, src_size);
}

static size_t algo_decompress(bench_algo_t algo,
                              void* dst,
                              size_t dst_capacity,
                              const void* src,
                              size_t src_size) {
    if (algo == BENCH_ALGO_FSE_ANS) {
        return FSE_decompress(dst, dst_capacity, src, src_size);
    }
    return HUF_frameDecompress(dst, dst_capacity, src, src_size);
}

static int algo_is_error(bench_algo_t algo, size_t code) {
    if (algo == BENCH_ALGO_FSE_ANS) {
        return FSE_isError(code);
    }
    return HUF_isError(code);
}

static const char* algo_error_name(bench_algo_t algo, size_t code) {
    if (algo == BENCH_ALGO_FSE_ANS) {
        return FSE_getErrorName(code);
    }
    return HUF_getErrorName(code);
}

static int format_chunk_label(size_t chunk_bytes, char* out, size_t out_cap) {
    const size_t kib = 1024;
    const size_t mib = 1024 * 1024;
    if (chunk_bytes == 0) return snprintf(out, out_cap, "full") > 0;
    if ((chunk_bytes % mib) == 0) return snprintf(out, out_cap, "%zuM", chunk_bytes / mib) > 0;
    if ((chunk_bytes % kib) == 0) return snprintf(out, out_cap, "%zuK", chunk_bytes / kib) > 0;
    return snprintf(out, out_cap, "%zuB", chunk_bytes) > 0;
}

static int parse_chunks(const char* arg, double** chunks_mb, size_t* count) {
    char* dup = (char*)malloc(strlen(arg) + 1);
    char* tok;
    size_t n = 0;
    size_t cap = 8;
    double* arr = (double*)malloc(cap * sizeof(double));
    if (!dup || !arr) {
        free(dup);
        free(arr);
        return 0;
    }
    strcpy(dup, arg);

    tok = strtok(dup, ",");
    while (tok) {
        double v = atof(tok);
        if (n == cap) {
            double* next;
            cap *= 2;
            next = (double*)realloc(arr, cap * sizeof(double));
            if (!next) {
                free(dup);
                free(arr);
                return 0;
            }
            arr = next;
        }
        arr[n++] = v;
        tok = strtok(NULL, ",");
    }

    free(dup);
    if (n == 0) {
        free(arr);
        return 0;
    }
    *chunks_mb = arr;
    *count = n;
    return 1;
}

static int parse_size_t_arg(const char* arg, size_t* value) {
    unsigned long long parsed;
    char* end = NULL;
    errno = 0;
    parsed = strtoull(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0') {
        return 0;
    }
    *value = (size_t)parsed;
    return ((unsigned long long)(*value) == parsed);
}

static chunk_t* build_chunks(size_t total_size, size_t chunk_size, size_t* out_count) {
    size_t cap = 16;
    size_t n = 0;
    size_t off = 0;
    chunk_t* chunks = (chunk_t*)malloc(cap * sizeof(chunk_t));
    if (!chunks) return NULL;

    if (chunk_size == 0 || chunk_size >= total_size) {
        chunks[0].offset = 0;
        chunks[0].len = total_size;
        *out_count = 1;
        return chunks;
    }

    while (off < total_size) {
        size_t len = chunk_size;
        if (len > total_size - off) len = total_size - off;
        if (n == cap) {
            chunk_t* next;
            cap *= 2;
            next = (chunk_t*)realloc(chunks, cap * sizeof(chunk_t));
            if (!next) {
                free(chunks);
                return NULL;
            }
            chunks = next;
        }
        chunks[n].offset = off;
        chunks[n].len = len;
        ++n;
        off += len;
    }
    *out_count = n;
    return chunks;
}

static run_stats_t run_once(bench_algo_t algo,
                            const uint8_t* input,
                            const chunk_t* chunks,
                            size_t chunk_count,
                            uint8_t* cbuf,
                            size_t cbuf_cap,
                            uint8_t* dbuf,
                            size_t dbuf_cap) {
    size_t i;
    run_stats_t st;
    st.comp_ms = 0.0;
    st.decomp_ms = 0.0;
    st.comp_bytes = 0;
    st.ok = 1;
    st.error[0] = '\0';

    for (i = 0; i < chunk_count; ++i) {
        size_t csize;
        size_t dsize;
        clock_t t0, t1;
        const uint8_t* in_ptr = input + chunks[i].offset;
        size_t in_len = chunks[i].len;
        (void)cbuf_cap;
        (void)dbuf_cap;

        t0 = clock();
        csize = algo_compress(algo, cbuf, cbuf_cap, in_ptr, in_len);
        t1 = clock();
        st.comp_ms += elapsed_ms(t0, t1);

        if (algo_is_error(algo, csize)) {
            st.ok = 0;
            snprintf(st.error, sizeof(st.error), "Compression failed: %s", algo_error_name(algo, csize));
            return st;
        }
        if (csize == 0) {
            st.ok = 0;
            snprintf(st.error, sizeof(st.error), "Compression returned empty output");
            return st;
        }

        t0 = clock();
        dsize = algo_decompress(algo, dbuf, in_len, cbuf, csize);
        t1 = clock();
        st.decomp_ms += elapsed_ms(t0, t1);

        if (algo_is_error(algo, dsize)) {
            st.ok = 0;
            snprintf(st.error, sizeof(st.error), "Decompression failed: %s", algo_error_name(algo, dsize));
            return st;
        }
        if (dsize != in_len || memcmp(in_ptr, dbuf, in_len) != 0) {
            st.ok = 0;
            snprintf(st.error, sizeof(st.error), "Decompression mismatch");
            return st;
        }

        st.comp_bytes += csize;
    }

    return st;
}

int main(int argc, char** argv) {
    const char* input_type = NULL;
    const char* input_path = NULL;
    const char* chunks_arg = "0";
    const char* csv_path = NULL;
    size_t warmup_iters = 1;
    size_t measure_iters = 10;
    bench_algo_t algo = BENCH_ALGO_FSE_ANS;
    uint8_t* input = NULL;
    size_t input_size = 0;
    double* chunks_mb = NULL;
    size_t chunks_mb_count = 0;
    size_t ci;
    FILE* csv = NULL;

    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <-u2|-u4> <input.bin> [--algo fse_ans|fse_huffman] [--chunks 0,0.125,0.5] [--warmup N] [--iters N] [--csv out.csv]\n",
                argv[0]);
        return 1;
    }

    input_type = argv[1];
    input_path = argv[2];
    if (strcmp(input_type, "-u2") != 0 && strcmp(input_type, "u2") != 0 &&
        strcmp(input_type, "-u4") != 0 && strcmp(input_type, "u4") != 0) {
        fprintf(stderr, "Unknown input type: %s (use -u2 or -u4)\n", input_type);
        return 1;
    }

    for (ci = 3; ci < (size_t)argc; ++ci) {
        if (!strcmp(argv[ci], "--algo") && ci + 1 < (size_t)argc) {
            if (!parse_algo(argv[++ci], &algo)) {
                fprintf(stderr, "Unknown --algo: %s\n", argv[ci]);
                return 1;
            }
        } else if (!strcmp(argv[ci], "--chunks") && ci + 1 < (size_t)argc) {
            chunks_arg = argv[++ci];
        } else if (!strcmp(argv[ci], "--warmup") && ci + 1 < (size_t)argc) {
            if (!parse_size_t_arg(argv[++ci], &warmup_iters)) {
                fprintf(stderr, "Invalid --warmup: %s\n", argv[ci]);
                return 1;
            }
        } else if (!strcmp(argv[ci], "--iters") && ci + 1 < (size_t)argc) {
            if (!parse_size_t_arg(argv[++ci], &measure_iters) || measure_iters == 0) {
                fprintf(stderr, "Invalid --iters: %s (must be >= 1)\n", argv[ci]);
                return 1;
            }
        } else if (!strcmp(argv[ci], "--csv") && ci + 1 < (size_t)argc) {
            csv_path = argv[++ci];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[ci]);
            return 1;
        }
    }

    if (!load_file(input_path, &input, &input_size)) {
        fprintf(stderr, "Failed to read input file: %s\n", input_path);
        return 1;
    }
    if (input_size == 0) {
        fprintf(stderr, "Input file is empty.\n");
        free(input);
        return 1;
    }
    if ((!strcmp(input_type, "-u2") || !strcmp(input_type, "u2")) && (input_size % 2 != 0)) {
        fprintf(stderr, "Input size must be multiple of 2 for u2.\n");
        free(input);
        return 1;
    }
    if ((!strcmp(input_type, "-u4") || !strcmp(input_type, "u4")) && (input_size % 4 != 0)) {
        fprintf(stderr, "Input size must be multiple of 4 for u4.\n");
        free(input);
        return 1;
    }

    if (!parse_chunks(chunks_arg, &chunks_mb, &chunks_mb_count)) {
        fprintf(stderr, "Failed to parse --chunks: %s\n", chunks_arg);
        free(input);
        return 1;
    }

    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "Failed to open CSV: %s\n", csv_path);
            free(chunks_mb);
            free(input);
            return 1;
        }
        fprintf(csv, "chunk_label,chunk_bytes,ratio_pct,comp_mbps,decomp_mbps\n");
    }

    printf("Command-line arguments:\n");
    printf("  Input type: %s\n", input_type);
    printf("  Input file: %s\n", input_path);
    printf("  Algo: %s\n", (algo == BENCH_ALGO_FSE_ANS) ? "fse_ans" : "fse_huffman");
    printf("  Chunks (MB): %s\n", chunks_arg);
    printf("  Warmup iters: %zu\n", warmup_iters);
    printf("  Measured iters: %zu\n", measure_iters);
    if (csv_path) {
        printf("  CSV: %s\n", csv_path);
    }
    printf("\n");

    printf("%-8s | %-9s | %-13s | %-13s\n", "Chunk", "Ratio", "Comp MB/s", "Decomp MB/s");
    printf("----------------------------------------------------\n");

    for (ci = 0; ci < chunks_mb_count; ++ci) {
        size_t chunk_bytes;
        size_t chunk_count;
        chunk_t* chunks;
        size_t k;
        size_t max_chunk_len = 0;
        size_t max_chunk_cap = 0;
        uint8_t* cbuf;
        uint8_t* dbuf;
        size_t iter;
        const size_t total_iters = warmup_iters + measure_iters;
        double best_ratio_pct = 0.0;
        double max_comp_mbps = 0.0;
        double max_decomp_mbps = 0.0;
        double ratio_pct;
        double comp_mbps;
        double decomp_mbps;
        char label[32];

        if (chunks_mb[ci] <= 0.0) {
            chunk_bytes = 0;
        } else {
            chunk_bytes = (size_t)(chunks_mb[ci] * 1024.0 * 1024.0);
            if (chunk_bytes == 0) chunk_bytes = 1;
        }

        chunks = build_chunks(input_size, chunk_bytes, &chunk_count);
        if (!chunks) {
            fprintf(stderr, "Out of memory while building chunks.\n");
            if (csv) fclose(csv);
            free(chunks_mb);
            free(input);
            return 1;
        }

        for (k = 0; k < chunk_count; ++k) {
            size_t cap = algo_compress_bound(algo, chunks[k].len);
            if (cap == 0) {
                fprintf(stderr, "compressBound overflow for chunk size %zu\n", chunks[k].len);
                free(chunks);
                if (csv) fclose(csv);
                free(chunks_mb);
                free(input);
                return 1;
            }
            if (chunks[k].len > max_chunk_len) max_chunk_len = chunks[k].len;
            if (cap > max_chunk_cap) max_chunk_cap = cap;
        }

        cbuf = (uint8_t*)malloc(max_chunk_cap);
        dbuf = (uint8_t*)malloc(max_chunk_len);
        if (!cbuf || !dbuf) {
            fprintf(stderr, "Out of memory while allocating run buffers.\n");
            free(cbuf);
            free(dbuf);
            free(chunks);
            if (csv) fclose(csv);
            free(chunks_mb);
            free(input);
            return 1;
        }

        for (iter = 0; iter < total_iters; ++iter) {
            double run_ratio_pct;
            double run_comp_mbps;
            double run_decomp_mbps;
            run_stats_t st = run_once(algo, input, chunks, chunk_count, cbuf, max_chunk_cap, dbuf, max_chunk_len);
            if (!st.ok) {
                fprintf(stderr, "%s\n", st.error);
                free(cbuf);
                free(dbuf);
                free(chunks);
                if (csv) fclose(csv);
                free(chunks_mb);
                free(input);
                return 1;
            }
            if (iter < warmup_iters) continue;

            run_ratio_pct = 100.0 * (double)st.comp_bytes / (double)input_size;
            run_comp_mbps = (st.comp_ms > 0.0) ? (((double)input_size / 1e6) / (st.comp_ms / 1e3)) : 0.0;
            run_decomp_mbps = (st.decomp_ms > 0.0) ? (((double)input_size / 1e6) / (st.decomp_ms / 1e3)) : 0.0;

            if ((iter == warmup_iters) || (run_ratio_pct < best_ratio_pct)) {
                best_ratio_pct = run_ratio_pct;
            }
            if (run_comp_mbps > max_comp_mbps) {
                max_comp_mbps = run_comp_mbps;
            }
            if (run_decomp_mbps > max_decomp_mbps) {
                max_decomp_mbps = run_decomp_mbps;
            }
        }

        ratio_pct = best_ratio_pct;
        comp_mbps = max_comp_mbps;
        decomp_mbps = max_decomp_mbps;

        format_chunk_label(chunk_bytes, label, sizeof(label));
        printf("%-8s | %-8.2f%% | %-13.1f | %-13.1f\n", label, ratio_pct, comp_mbps, decomp_mbps);

        if (csv) {
            fprintf(csv, "%s,%zu,%.2f,%.1f,%.1f\n",
                    label, chunk_bytes == 0 ? input_size : chunk_bytes, ratio_pct, comp_mbps, decomp_mbps);
        }

        free(cbuf);
        free(dbuf);
        free(chunks);
    }

    if (csv) fclose(csv);
    free(chunks_mb);
    free(input);
    return 0;
}
