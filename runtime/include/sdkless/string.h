/* Stand-in for <string.h> without a system SDK (see math.h here). */
#ifndef X2_SDKLESS_STRING_H
#define X2_SDKLESS_STRING_H

typedef __SIZE_TYPE__ size_t;
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);

#endif
