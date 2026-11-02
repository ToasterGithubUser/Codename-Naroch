#include <stddef.h>

typedef void (*init_function_t)(void);

extern init_function_t __init_array_start[];
extern init_function_t __init_array_end[];
extern init_function_t __preinit_array_start[];
extern init_function_t __preinit_array_end[];
extern init_function_t __fini_array_start[];
extern init_function_t __fini_array_end[];

void __libc_init_array(void) {
    for (init_function_t* function = __preinit_array_start;
         function < __preinit_array_end; function++) {
        (*function)();
    }
    for (init_function_t* function = __init_array_start;
         function < __init_array_end; function++) {
        (*function)();
    }
}

void __libc_fini_array(void) {
    init_function_t* function = __fini_array_end;
    while (function != __fini_array_start) {
        (*--function)();
    }
}
