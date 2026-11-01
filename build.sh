#!/bin/sh
set -e
. ./config.sh

# --- АВТОМАТИЧЕСКИЙ СЧЕТЧИК СБОРКИ ---
BUILD_FILE=".build_number"

# Если файла нет, начинаем с 0
if [ ! -f "$BUILD_FILE" ]; then
    echo "0" > "$BUILD_FILE"
fi

# Читаем старый номер и увеличиваем на 1
BUILD_NUM=$(cat "$BUILD_FILE")
BUILD_NUM=$((BUILD_NUM + 1))
echo "$BUILD_NUM" > "$BUILD_FILE"

# Генерируем заголовочный файл в папку инклюдов ядра
# В синтаксисе Meaty Skeleton путь обычно: kernel/include/kernel/build_no.h
cat << EOF > kernel/include/kernel/build_no.h
#ifndef _KERNEL_BUILD_NO_H
#define _KERNEL_BUILD_NO_H

#define KERNEL_BUILD_NUMBER $BUILD_NUM

#endif
EOF
# -------------------------------------

export MAKE=${MAKE:-make}
#!/bin/sh
set -e
. ./headers.sh

for PROJECT in $PROJECTS; do
  (cd $PROJECT && DESTDIR="$SYSROOT" $MAKE install)
done
