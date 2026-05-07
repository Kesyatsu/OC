#!/bin/bash

# Настройки
EXE="./copy_app"                # Имя вашего скомпилированного файла
TEST_FILE="source_big.dat"      # Имя исходного файла
DEST_FILE="dest_test.dat"       # Имя целевого файла
LOG_FILE="results.csv"          # Файл с результатами
FILE_SIZE_MB=512                # Размер тестового файла в МБ

# 1. Создание тестового файла (заполнен случайными данными)
echo "Creating test file ($FILE_SIZE_MB MB)..."
dd if=/dev/urandom of=$TEST_FILE bs=1M count=$FILE_SIZE_MB status=progress

# Подготовка CSV заголовка
echo "BlockSize,NumOps,TimeSec,SpeedMBs" > $LOG_FILE

# Массивы параметров для тестов
block_sizes=(4096 8192 16384 32768 65536 131072) # Размеры блока (в байтах)
ops_counts=(1 2 4 8 12 16)                        # Кол-во перекрывающихся операций

echo "Starting benchmarks..."

for bs in "${block_sizes[@]}"; do
    for ops in "${ops_counts[@]}"; do
        echo "Testing: BlockSize=$bs, Ops=$ops"
        
        # Запуск программы и замер времени (внутренним time или встроенным в приложение)
        # Предположим, программа сама выводит время в секундах в консоль
        # Если нет, используем /usr/bin/time
        
        START_TIME=$(date +%s.%N)
        
        # Запуск вашей программы (передаем параметры через аргументы командной строки)
        $EXE $TEST_FILE $DEST_FILE $bs $ops
        
        END_TIME=$(date +%s.%N)
        
        # Расчет времени выполнения через bc
        DURATION=$(echo "$END_TIME - $START_TIME" | bc)
        
        # Расчет скорости (МБ/с)
        SPEED=$(echo "scale=2; $FILE_SIZE_MB / $DURATION" | bc)
        
        # Запись в лог
        echo "$bs,$ops,$DURATION,$SPEED" >> $LOG_FILE
        
        # Проверка целостности через fc (в Linux это команда cmp или diff)
        if ! cmp -s "$TEST_FILE" "$DEST_FILE"; then
            echo "Error: Files are not identical for BS=$bs, Ops=$ops"
        fi
        
        # Очистка кэша записи (опционально, требует sudo)
        # sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
    done
done

echo "Done. Results saved to $LOG_FILE"