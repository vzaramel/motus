#ifndef MOT_FREESTANDING_STDLIB_H
#define MOT_FREESTANDING_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t new_size);

double strtod(const char *nptr, char **endptr);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

int abs(int x);

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

#endif
