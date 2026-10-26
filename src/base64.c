/* base64.c —— 标准 Base64 编码 */
#include "base64.h"

static const char ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encoded_size(size_t len)
{
    return ((len + 2) / 3) * 4;
}

size_t base64_encode(const unsigned char *src, size_t len, char *dst)
{
    if (dst == NULL) return 0;
    if (len == 0) { dst[0] = 0; return 0; }
    if (src == NULL) return 0;

    size_t out = 0;
    size_t i = 0;

    for (; i + 2 < len; i += 3) {
        unsigned int v = ((unsigned int)src[i] << 16) |
                         ((unsigned int)src[i + 1] << 8) |
                         (unsigned int)src[i + 2];
        dst[out++] = ALPHABET[(v >> 18) & 0x3F];
        dst[out++] = ALPHABET[(v >> 12) & 0x3F];
        dst[out++] = ALPHABET[(v >> 6) & 0x3F];
        dst[out++] = ALPHABET[v & 0x3F];
    }

    size_t rest = len - i;
    if (rest == 1) {
        unsigned int v = (unsigned int)src[i] << 16;
        dst[out++] = ALPHABET[(v >> 18) & 0x3F];
        dst[out++] = ALPHABET[(v >> 12) & 0x3F];
        dst[out++] = '=';
        dst[out++] = '=';
    } else if (rest == 2) {
        unsigned int v = ((unsigned int)src[i] << 16) |
                         ((unsigned int)src[i + 1] << 8);
        dst[out++] = ALPHABET[(v >> 18) & 0x3F];
        dst[out++] = ALPHABET[(v >> 12) & 0x3F];
        dst[out++] = ALPHABET[(v >> 6) & 0x3F];
        dst[out++] = '=';
    }

    dst[out] = 0;
    return out;
}
