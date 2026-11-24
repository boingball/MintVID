#include <stddef.h>
#include <stdint.h>
void *memset(void *dst,int c,size_t n){ unsigned char *d=(unsigned char*)dst; while(n--)*d++=(unsigned char)c; return dst; }
void *memcpy(void *dst,const void *src,size_t n){ unsigned char*d=(unsigned char*)dst; const unsigned char*s=(const unsigned char*)src; while(n--)*d++=*s++; return dst; }
