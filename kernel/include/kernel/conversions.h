#ifndef _KERNEL_CONVERSIONS_H
#define _KERNEL_CONVERSIONS_H
#include <stdint.h>
void inttostr(int num, char* str);
void inttohex(uint32_t num, char* str);
#endif