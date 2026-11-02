#include <string.h>

char* strcpy(char* restrict destination, const char* restrict source) {
    char* result = destination;
    while ((*destination++ = *source++) != '\0') {
    }
    return result;
}
