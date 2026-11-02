#include <stdio.h>

#if defined(__is_libk)
#include <kernel/tty.h>
#else
#include <unistd.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
	terminal_write(&c, sizeof(c));
#else
	char c = (char) ic;
	if (write(1, &c, sizeof(c)) != sizeof(c))
		return EOF;
#endif
	return ic;
}
