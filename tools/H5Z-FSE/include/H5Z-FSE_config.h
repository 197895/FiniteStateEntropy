#ifndef H5Z_FSE_CONFIG_H
#define H5Z_FSE_CONFIG_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define H5Z_FSE_CODEC_ANS      0u
#define H5Z_FSE_CODEC_HUFFMAN  1u

static int H5Z_FSE_codecFromString(const char* value, unsigned* codec) {
    if (!value || !codec) return 0;
    if (strcmp(value, "fse_ans") == 0) {
        *codec = H5Z_FSE_CODEC_ANS;
        return 1;
    }
    if (strcmp(value, "fse_huffman") == 0) {
        *codec = H5Z_FSE_CODEC_HUFFMAN;
        return 1;
    }
    return 0;
}

static char* H5Z_FSE_trim(char* s) {
    char* end;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) --end;
    end[1] = '\0';
    return s;
}

static int H5Z_FSE_loadCodecFromConfig(const char* path, unsigned* codec) {
    FILE* f;
    char line[256];
    if (!path || !codec) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        char* eq;
        p = H5Z_FSE_trim(p);
        if (*p == '\0' || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        {
            char* key = H5Z_FSE_trim(p);
            char* val = H5Z_FSE_trim(eq + 1);
            if (strcmp(key, "algo") == 0 || strcmp(key, "algorithm") == 0) {
                int ok = H5Z_FSE_codecFromString(val, codec);
                fclose(f);
                return ok;
            }
        }
    }
    fclose(f);
    return 0;
}

#endif /* H5Z_FSE_CONFIG_H */

