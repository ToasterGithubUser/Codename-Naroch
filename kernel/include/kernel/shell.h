#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

// Добавляет символ в буфер командной строки
void shell_input_char(char c);

// Вызывается при нажатии Enter для обработки накопленной строки
void shell_execute(void);

// Выводит приглашение к вводу (например, "naroch> ")
void shell_prompt(void);

#endif
