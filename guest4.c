#include <stdint.h>

#define CMD_OPEN   1
#define CMD_READ   2
#define CMD_WRITE  3
#define CMD_CLOSE  4

#define FILE_READ   0
#define FILE_WRITE  1
#define FILE_APPEND 2

#define PORT_FILE 0x0278
#define REQ_ADR 0x9000
#define BUF_ADR 0x9100

struct FileRequest {
    uint32_t cmd;
    uint32_t fd;
    uint64_t buf_addr;
    uint32_t size;
    uint32_t flags;
    int32_t result;
};

static int my_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static void my_memcpy(void *dst, const void *src, int n) {
    char *d = (char*)dst;
    const char *s = (const char*)src;
    for (int i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void outb(uint16_t port, uint8_t value) {
    asm("outb %0,%1" : : "a"(value), "Nd"(port) : "memory");
}

static void outl(uint16_t port, uint32_t value) {
    asm("outl %0,%1" : : "a"(value), "Nd"(port) : "memory");
}

int _open(const char *path, int flags) {
    struct FileRequest *req = (struct FileRequest*)REQ_ADR;

    char *buf = (char *)BUF_ADR;
    my_strcpy(buf, path);

    req->cmd = CMD_OPEN;
    req->fd = 0;
    req->buf_addr = BUF_ADR;
    req->size = my_strlen(path) + 1;
    req->flags = flags;

    outl(PORT_FILE, REQ_ADR);

    return req->result;
}

int _write(int fd, const char *data, int size) {
    struct FileRequest *req = (struct FileRequest*)REQ_ADR;

    char *buf = (char *)BUF_ADR;
    my_memcpy(buf, data, size);

    req->cmd = CMD_WRITE;
    req->fd = fd;
    req->buf_addr = BUF_ADR;
    req->size = size;

    outl(PORT_FILE, REQ_ADR);

    return req->result;
}

int _read(int fd, char *dest, int size) {
    struct FileRequest *req = (struct FileRequest*)REQ_ADR;

    req->cmd = CMD_READ;
    req->fd = fd;
    req->buf_addr = BUF_ADR;
    req->size = size;

    outl(PORT_FILE, REQ_ADR);

    my_memcpy(dest, (char*)BUF_ADR, req->result);

    return req->result;
}

int _close(int fd) {
    struct FileRequest *req = (struct FileRequest*)REQ_ADR;

    req->cmd = CMD_CLOSE;
    req->fd = fd;

    outl(PORT_FILE, REQ_ADR);

    return req->result;
}

void __attribute__((noreturn)) __attribute__((section(".start")))
_start(void) {
    int fd=_open("shared.txt",FILE_APPEND);
    if(fd>=0){
        _write(fd,"Dodatak iz gosta 3!",21);
        _close(fd);
    }
    fd = _open("shared.txt",FILE_APPEND);
    if(fd>=0){
        char buf[64];
        int n = _read(fd, buf, sizeof(buf)-1);
        buf[n] = '\0';

        for (char *p = buf; *p; p++) {
            outb(0xE9, *p);
        }
        _close(fd);
    }
    for(;;) asm("hlt");
}
