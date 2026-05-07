#define _GNU_SOURCE
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <aio.h>
#include <signal.h>
#include <sys/stat.h>
#include <cstring>
#include <chrono>

// Структура для управления асинхронной операцией (из методички)
struct aio_operation {
    struct aiocb aio;
    char* buffer;
    bool is_write;
    long offset;
};

// Глобальные переменные для синхронизации завершения
volatile int completed_ops = 0;
int total_needed = 0;

// Функция завершения (Callback)
void aio_completion_handler(sigval_t sigval) {
    struct aio_operation* op = (struct aio_operation*)sigval.sival_ptr;
    
    if (aio_error(&op->aio) == 0) {
        size_t ret = aio_return(&op->aio);
        if (ret > 0) {
            // Если это было чтение, можно было бы сразу запустить запись,
            // но для лаконичности просто считаем завершенные блоки.
            completed_ops++;
        }
    } else {
        perror("AIO Error");
    }
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cout << "Использование: ./copy_app <откуда> <куда> <размер_блока> <кол_во_операций>" << std::endl;
        return 1;
    }

    const char* src_path = argv[1];
    const char* dst_path = argv[2];
    size_t block_size = std::stoul(argv[3]);
    int n_ops = std::stoi(argv[4]);

    // 1. Открытие файлов (O_DIRECT требует выравнивания буфера)
    int src_fd = open(src_path, O_RDONLY | O_DIRECT);
    int dst_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0666);

    if (src_fd < 0 || dst_fd < 0) {
        perror("Ошибка открытия файлов (проверьте наличие прав и O_DIRECT)");
        return 1;
    }

    // Получаем размер файла
    struct stat st;
    fstat(src_fd, &st);
    long file_size = st.st_size;
    total_needed = (file_size + block_size - 1) / block_size;

    // 2. Подготовка пула операций и буферов
    std::vector<aio_operation*> ops(n_ops);
    for (int i = 0; i < n_ops; ++i) {
        ops[i] = new aio_operation();
        // Выравнивание памяти по 4096 байт для O_DIRECT
        posix_memalign((void**)&ops[i]->buffer, 4096, block_size);
        memset(&ops[i]->aio, 0, sizeof(struct aiocb));
    }

    auto start = std::chrono::high_resolution_clock::now();

    // 3. Основной цикл копирования
    long current_offset = 0;
    int active_ops = 0;

    while (completed_ops < total_needed) {
        for (int i = 0; i < n_ops; ++i) {
            // Если операция свободна и файл еще не вычитан полностью
            if (aio_error(&ops[i]->aio) != EINPROGRESS && current_offset < file_size) {
                
                ops[i]->aio.aio_fildes = src_fd;
                ops[i]->aio.aio_buf = ops[i]->buffer;
                ops[i]->aio.aio_nbytes = block_size;
                ops[i]->aio.aio_offset = current_offset;
                
                // Настройка уведомления
                ops[i]->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
                ops[i]->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
                ops[i]->aio.aio_sigevent.sigev_value.sival_ptr = ops[i];

                // Читаем блок (в реальной асинхронности здесь должна быть цепочка Read -> Write)
                // Для учебной задачи упростим до синхронного запуска асинхронных чтений
                if (aio_read(&ops[i]->aio) == 0) {
                    // Ждем завершения чтения этого блока для простоты примера
                    const struct aiocb* aio_list[1] = {&ops[i]->aio};
                    aio_suspend(aio_list, 1, NULL);
                    
                    // Сразу запускаем запись этого же блока в выходной файл
                    ops[i]->aio.aio_fildes = dst_fd;
                    aio_write(&ops[i]->aio);
                    
                    current_offset += block_size;
                }
            }
        }
        usleep(1000); // Небольшая пауза, чтобы не нагружать CPU в цикле
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Копирование завершено за: " << diff.count() << " сек." << std::endl;
    std::cout << "Скорость: " << (file_size / (1024.0 * 1024.0)) / diff.count() << " MB/s" << std::endl;

    // Очистка
    for (int i = 0; i < n_ops; ++i) {
        free(ops[i]->buffer);
        delete ops[i];
    }
    close(src_fd);
    close(dst_fd);

    return 0;
}