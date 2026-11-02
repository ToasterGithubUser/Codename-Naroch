Naroch user executables

This runtime supports freestanding ELF32/i386 ET_EXEC programs, standard C
main(argc, argv, envp), and a small libc with basic memory, file, and output
functions. C++17 programs can use global constructors/destructors and basic
new/delete. Exceptions, RTTI, dynamic linking, and libstdc++ are not provided.
stdout is file descriptor 1.

The repository FAT16 image includes HELLO.ELF, built from hello.c. Run it
from the OS shell with:

  exec hello.elf

For build instructions and the supported C/C++ syntax and runtime, see
ELF_TUTORIAL.txt in the repository root.

Boot the OS and launch the example directly:

  ./hello.elf
