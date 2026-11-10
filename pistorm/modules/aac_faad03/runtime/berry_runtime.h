#ifndef BERRY_RUNTIME_H
#define BERRY_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
void berry_heap_bind(void *base, size_t bytes);
size_t berry_heap_used(void);
size_t berry_heap_capacity(void);
int berry_heap_failed(void);
uint32_t berry_fnv1a32(const void *data, size_t n);
#endif
