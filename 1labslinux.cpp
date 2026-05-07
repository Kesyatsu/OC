#define _GNU_SOURCE
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <aio.h>
#include <signal.h>
#include <sys/stat.h>
#include <cstring>

// Структура из методички
struct aio_operation {
    struct aiocb aio;
    char* buffer;
    int write_operation;
    void* next_operation;
};

// Обработчик завершения
void aio_completion_handler(sigval_t sigval) {
    struct aio_operation* aio_op = (struct aio_operation*)sigval.sival_ptr;
    
    if (aio_error(&aio_op->aio) == 0) {
        size_t ret = aio_return(&aio_op->aio);
        // Здесь логика переключения между чтением и записью
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: ./copy <src> <dest>" << std::endl;
        return 1;
    }

    int src_fd = open(argv[1], O_RDONLY | O_DIRECT);
    int dest_fd = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0666);

    if (src_fd < 0 || dest_fd < 0) {
        perror("Error opening files");
        return 1;
    }

    struct stat st;
    fstat(src_fd, &st);
    
    size_t block_size = 4096; // Размер кластера
    int n_ops = 4; // Количество перекрывающихся операций
    
    std::vector<aio_operation> ops(n_ops);
    
    // Пример настройки одной операции чтения
    for(int i = 0; i < n_ops; ++i) {
        // Выравнивание памяти необходимо для O_DIRECT
        posix_memalign((void**)&ops[i].buffer, 4096, block_size);
        memset(&ops[i].aio, 0, sizeof(struct aiocb));
        
        ops[i].aio.aio_fildes = src_fd;
        ops[i].aio.aio_buf = ops[i].buffer;
        ops[i].aio.aio_nbytes = block_size;
        ops[i].aio.aio_offset = i * block_size;
        
        // Настройка уведомления через сигнал/функцию
        ops[i].aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
        ops[i].aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
        ops[i].aio.aio_sigevent.sigev_value.sival_ptr = &ops[i];

        aio_read(&ops[i].aio);
    }

    std::cout << "Copying in progress..." << std::endl;
    // В реальной лабораторной тут должен быть цикл ожидания через aio_suspend
    sleep(2); 

    close(src_fd);
    close(dest_fd);
    return 0;
}