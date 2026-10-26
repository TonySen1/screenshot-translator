/* base64.h —— 标准 Base64 编码（RFC 4648，带 '=' 填充） */
#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

/* 编码 len 字节需要的缓冲长度（不含结尾 0） */
size_t base64_encoded_size(size_t len);

/* 编码 src 到 dst。dst 至少要有 base64_encoded_size(len) + 1 字节。
   返回写入的字符数（不含结尾 0），失败返回 0。 */
size_t base64_encode(const unsigned char *src, size_t len, char *dst);

#endif /* BASE64_H */
