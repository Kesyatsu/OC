#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <aio.h>
#include <signal.h>
#include <sys/stat.h>
#include <cstring>
#include <atomic>
#include <chrono>

// Структура для управления одной асинхронной операцией
struct aio_operation {
    struct aiocb aio;
    char* buffer;
    bool is_writing; // Флаг: false - чтение, true - запись
};

// Атомарный счетчик для безопасного подсчета завершенных блоков из разных потоков
std::atomic<int> completed_blocks(0);

// Обработчик завершения асинхронных операций
void aio_completion_handler(sigval_t sigval) {
    struct aio_operation* op = (struct aio_operation*)sigval.sival_ptr;
    
    int err = aio_error(&op->aio);
    if (err == 0) {
        ssize_t ret = aio_return(&op->aio);
        if (ret > 0) {
            if (op->is_writing) {
                // Если успешно завершилась запись — блок полностью скопирован
                completed_blocks++;
            }
        }
    } else if (err != EINPROGRESS) {
        std::cerr << "AIO Error: " << strerror(err) << std::endl;
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

    // Открываем файлы. 
    // ПРИМЕЧАНИЕ: Если программа запускается на файловой системе без поддержки O_DIRECT (например, WSL),
    // можно убрать O_DIRECT из флагов open.
    int src_fd = open(src_path, O_RDONLY | O_DIRECT);
    int dst_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0666);

    if (src_fd < 0 || dst_fd < 0) {
        perror("Ошибка открытия файлов. Попробуйте запустить без O_DIRECT в коде, если запускаете в WSL/Docker");
        return 1;
    }

    // Узнаем точный размер исходного файла
    struct stat64 st;
    if (fstat64(src_fd, &st) < 0) {
        perror("Ошибка fstat");
        return 1;
    }
    long long file_size = st.st_size;
    int total_blocks = (file_size + block_size - 1) / block_size;

    // Выделяем пулы операций и выравниваем буферы под O_DIRECT (кратно 4096 байт)
    std::vector<aio_operation*> ops(n_ops);
    for (int i = 0; i < n_ops; ++i) {
        ops[i] = new aio_operation();
        ops[i]->is_writing = false;
        if (posix_memalign((void**)&ops[i]->buffer, 4096, block_size) != 0) {
            perror("Ошибка выделения памяти posix_memalign");
            return 1;
        }
        memset(&ops[i]->aio, 0, sizeof(struct aiocb));
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    long long read_offset = 0;

    // Главный цикл координации задач
    while (completed_blocks.load() < total_blocks) {
        for (int i = 0; i < n_ops; ++i) {
            int err = aio_error(&ops[i]->aio);

            if (err == EINPROGRESS) {
                // Операция все еще выполняется в фоне, пропускаем ее
                continue;
            }

            // Сценарий 1: Слот свободен, и нам нужно прочитать следующую порцию данных
            if (!ops[i]->is_writing && read_offset < file_size) {
                memset(&ops[i]->aio, 0, sizeof(struct aiocb));
                ops[i]->aio.aio_fildes = src_fd;
                ops[i]->aio.aio_buf = ops[i]->buffer;
                ops[i]->aio.aio_nbytes = block_size;
                ops[i]->aio.aio_offset = read_offset;

                ops[i]->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
                ops[i]->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
                ops[i]->aio.aio_sigevent.sigev_value.sival_ptr = ops[i];

                ops[i]->is_writing = false;
                
                if (aio_read(&ops[i]->aio) == 0) {
                    read_offset += block_size;
                }
            }
            // Сценарий 2: Операция чтения только что успешно завершилась — запускаем запись прочитанного
            else if (!ops[i]->is_writing && err == 0 && ops[i]->aio.aio_fildes == src_fd) {
                ssize_t bytes_read = aio_return(&ops[i]->aio);
                if (bytes_read > 0) {
                    long long current_offset = ops[i]->aio.aio_offset;

                    memset(&ops[i]->aio, 0, sizeof(struct aiocb));
                    ops[i]->aio.aio_fildes = dst_fd;
                    ops[i]->aio.aio_buf = ops[i]->buffer;
                    // Для O_DIRECT пишем полный выровненный блок, лишнее обрежем в конце файла
                    ops[i]->aio.aio_nbytes = block_size; 
                    ops[i]->aio.aio_offset = current_offset;

                    ops[i]->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
                    ops[i]->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
                    ops[i]->aio.aio_sigevent.sigev_value.sival_ptr = ops[i];

                    ops[i]->is_writing = true; // Переключаем режим на запись

                    aio_write(&ops[i]->aio);
                }
            }
            // Сценарий 3: Запись завершена, переводим слот обратно в режим чтения
            else if (ops[i]->is_writing && err == 0 && ops[i]->aio.aio_fildes == dst_fd) {
                ops[i]->is_writing = false;
                // Зануляем дескриптор, показывая, что слот готов к новой работе
                ops[i]->aio.aio_fildes = 0; 
            }
        }
        
        // Микропауза для предотвращения 100%-й нагрузки на одно ядро процессора
        usleep(500); 
    }

    // Обрезаем скопированный файл до оригинального размера (убирает мусор в конце из-за O_DIRECT)
    if (ftruncate64(dst_fd, file_size) < 0) {
        perror("Ошибка ftruncate");
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    std::cout << "Копирование завершено успешно за: " << diff.count() << " сек." << std::endl;
    std::cout << "Скорость: " << (file_size / (1024.0 * 1024.0)) / diff.count() << " MB/s" << std::endl;

    // Освобождение ресурсов
    for (int i = 0; i < n_ops; ++i) {
        free(ops[i]->buffer);
        delete ops[i];
    }
    close(src_fd);
    close(dst_fd);

    return 0;
}