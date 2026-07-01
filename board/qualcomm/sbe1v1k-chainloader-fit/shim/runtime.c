#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;
	size_t i;

	for (i = 0; i < n; ++i)
		d[i] = s[i];

	return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;
	size_t i;

	if (d == s || !n)
		return dest;

	if (d < s) {
		for (i = 0; i < n; ++i)
			d[i] = s[i];
	} else {
		for (i = n; i != 0; --i)
			d[i - 1] = s[i - 1];
	}

	return dest;
}

void *memset(void *dest, int c, size_t n)
{
	unsigned char *d = dest;
	size_t i;

	for (i = 0; i < n; ++i)
		d[i] = (unsigned char)c;

	return dest;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *lhs = a;
	const unsigned char *rhs = b;
	size_t i;

	for (i = 0; i < n; ++i) {
		if (lhs[i] != rhs[i])
			return (int)lhs[i] - (int)rhs[i];
	}

	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	size_t i;

	for (i = 0; i < n; ++i) {
		if (p[i] == (unsigned char)c)
			return (void *)(uintptr_t)(p + i);
	}

	return 0;
}

size_t strlen(const char *s)
{
	size_t len = 0;

	while (s[len] != '\0')
		++len;

	return len;
}

size_t strnlen(const char *s, size_t maxlen)
{
	size_t len = 0;

	while (len < maxlen && s[len] != '\0')
		++len;

	return len;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		++a;
		++b;
	}

	return (unsigned char)*a - (unsigned char)*b;
}

char *strrchr(const char *s, int c)
{
	const char *last = 0;

	for (;;) {
		if (*s == (char)c)
			last = s;
		if (*s == '\0')
			return (char *)(uintptr_t)last;
		++s;
	}
}
