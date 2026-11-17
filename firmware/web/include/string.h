/* the two string functions the engine uses, for a freestanding WebAssembly build */
#pragma once
typedef __SIZE_TYPE__ size_t;
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
