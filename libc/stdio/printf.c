#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static bool print(const char* data, size_t length) {
	const unsigned char* bytes = (const unsigned char*) data;
	for (size_t i = 0; i < length; i++)
		if (putchar(bytes[i]) == EOF)
			return false;
	return true;
}

static bool print_integer(unsigned int value, unsigned int base, int uppercase,
                          int negative, int pointer, int* written) {
	char digits[sizeof(value) * 8];
	size_t length = 0;
	do {
		unsigned int digit = value % base;
		digits[length++] = (char)(digit < 10
			? '0' + digit
			: (uppercase ? 'A' : 'a') + digit - 10);
		value /= base;
	} while (value);

	if (negative) {
		if (putchar('-') == EOF)
			return false;
		(*written)++;
	}
	if (pointer) {
		if (!print("0x", 2))
			return false;
		*written += 2;
	}
	while (length) {
		if (putchar(digits[--length]) == EOF)
			return false;
		(*written)++;
	}
	return true;
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;

	while (*format != '\0') {
		size_t maxrem = INT_MAX - written;

		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%')
				format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%')
				amount++;
			if (maxrem < amount) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(format, amount))
				return -1;
			format += amount;
			written += amount;
			continue;
		}

		const char* format_begun_at = format++;

		if (*format == 'c') {
			format++;
			char c = (char) va_arg(parameters, int /* char promotes to int */);
			if (!maxrem) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(&c, sizeof(c)))
				return -1;
			written++;
		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			if (!str)
				str = "(null)";
			size_t len = strlen(str);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(str, len))
				return -1;
			written += len;
		} else if (*format == 'd' || *format == 'i') {
			format++;
			int value = va_arg(parameters, int);
			unsigned int magnitude = value < 0
				? 0U - (unsigned int)value
				: (unsigned int)value;
			if (!print_integer(magnitude, 10, 0, value < 0, 0, &written))
				return -1;
		} else if (*format == 'u' || *format == 'x' ||
		           *format == 'X' || *format == 'p') {
			char conversion = *format++;
			unsigned int value = conversion == 'p'
				? (unsigned int)(uintptr_t)va_arg(parameters, void*)
				: va_arg(parameters, unsigned int);
			unsigned int base = conversion == 'u' ? 10 : 16;
			if (!print_integer(value, base, conversion == 'X', 0,
			                   conversion == 'p', &written))
				return -1;
		} else {
			format = format_begun_at;
			size_t len = strlen(format);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(format, len))
				return -1;
			written += len;
			format += len;
		}
	}

	va_end(parameters);
	return written;
}
