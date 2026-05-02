#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <linux/kvm.h>
#include <getopt.h>
#include <pthread.h>

#define GUEST_START_ADDR 0x8000 

#define PDE64_PRESENT (1u << 0)
#define PDE64_RW (1u << 1)
#define PDE64_USER (1u << 2)
#define PDE64_PS (1u << 7)

#define CR0_PE (1u << 0)
#define CR0_PG (1u << 31)
#define CR4_PAE (1u << 5)

#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

#define PORT_FILE 0x0278
#define REQ_ADDR  0x9000
#define BUF_ADDR  0x9100

#define CMD_OPEN   1
#define CMD_READ   2
#define CMD_WRITE  3
#define CMD_CLOSE  4

#define FILE_READ   0
#define FILE_WRITE  1
#define FILE_APPEND 2

#define MAX_SHARED 16

struct vm {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    char *mem;
    size_t mem_size;
    struct kvm_run *run;
    int run_mmap_size;
    size_t page_size;
    FILE *fd[32];
    char *local_shared[MAX_SHARED];
    int local_shared_count;
};

struct vm_args{
    size_t mem_size;
    size_t page_size;
    char *guest_path;
    int vm_id;
};

struct FileRequest {
    uint32_t cmd;
    uint32_t fd;
    uint64_t buf_addr;
    uint32_t size;
    uint32_t flags;
    int32_t result;
};

char* shared_files[MAX_SHARED];
int shared_count = 0;

int vm_init(struct vm *v, size_t mem_size,size_t page_size){
    struct kvm_userspace_memory_region region;    

    memset(v, 0, sizeof(*v));
    v->kvm_fd = v->vm_fd = v->vcpu_fd = -1;
    v->mem = MAP_FAILED;
    v->run = MAP_FAILED;
    v->run_mmap_size = 0;
    v->mem_size = mem_size;
    v->page_size = page_size;

    v->kvm_fd = open("/dev/kvm", O_RDWR);
    if (v->kvm_fd < 0) {
        perror("open /dev/kvm");
        return -1;
    }

    int api = ioctl(v->kvm_fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) {
        printf("KVM API mismatch: kernel=%d headers=%d\n", api, KVM_API_VERSION);
        return -1;
    }

    v->vm_fd = ioctl(v->kvm_fd, KVM_CREATE_VM, 0);
    if (v->vm_fd < 0) {
        perror("KVM_CREATE_VM");
        return -1;
    }

    v->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (v->mem == MAP_FAILED) {
        perror("mmap mem");
        return -1;
    }

    region.slot = 0;
    region.flags = 0;
    region.guest_phys_addr = 0;
    region.memory_size = v->mem_size;
    region.userspace_addr = (uintptr_t)v->mem;
    if (ioctl(v->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return -1;
    }

    v->vcpu_fd = ioctl(v->vm_fd, KVM_CREATE_VCPU, 0);
    if (v->vcpu_fd < 0) {
        perror("KVM_CREATE_VCPU");
        return -1;
    }

    v->run_mmap_size = ioctl(v->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (v->run_mmap_size <= 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        return -1;
    }

    v->run = mmap(NULL, v->run_mmap_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED, v->vcpu_fd, 0);
    if (v->run == MAP_FAILED) {
        perror("mmap kvm_run");
        return -1;
    }

    return 0;
}

void vm_destroy(struct vm *v) {
    for (int i = 0; i < v->local_shared_count; i++) {
        free(v->local_shared[i]);
    }
    v->local_shared_count = 0;

    if (v->run && v->run != MAP_FAILED) {
        munmap(v->run, (size_t)v->run_mmap_size);
        v->run = MAP_FAILED;
    }

    if(v->mem && v->mem != MAP_FAILED) {
        munmap(v->mem, v->mem_size);
        v->mem = MAP_FAILED;
    }

    if (v->vcpu_fd >= 0) {
        close(v->vcpu_fd);
        v->vcpu_fd = -1;
    }

    if (v->vm_fd >= 0) {
        close(v->vm_fd);
        v->vm_fd = -1;
    }

    if (v->kvm_fd >= 0) {
        close(v->kvm_fd);
        v->kvm_fd = -1;
    }
}

static void setup_segments_64(struct kvm_sregs *sregs){
    struct kvm_segment code = {
        .base = 0,
        .limit = 0xffffffff,
        .present = 1,
        .type = 11,
        .dpl = 0,
        .db = 0,
        .s = 1,
        .l = 1,
        .g = 1,
    };
    struct kvm_segment data = code;
    data.type = 3;
    data.l = 0;

    sregs->cs = code;
    sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = data;
}

static void setup_long_mode(struct vm *v, struct kvm_sregs *sregs){
    uint64_t pt4_addr = 0x1000;
    uint64_t pt3_addr = 0x2000;
    uint64_t pt2_addr = 0x3000;
    uint64_t pt1_base = 0x4000;

    uint64_t *pt4 = (void *)(v->mem + pt4_addr);
    uint64_t *pt3 = (void *)(v->mem + pt3_addr);
    uint64_t *pt2 = (void *)(v->mem + pt2_addr);

    memset(pt4, 0, 0x1000);
    memset(pt3, 0, 0x1000);
    memset(pt2, 0, 0x1000);

    pt4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pt3_addr;
    pt3[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pt2_addr;

    if (v->page_size == 0x200000) {
        size_t num_entries = v->mem_size / (2 * 1024 * 1024);
        for (size_t i = 0; i < num_entries; i++) {
            pt2[i] = PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS | (i * 0x200000ULL);
        }
    } else if (v->page_size == 0x1000) {
        size_t num_pt1 = v->mem_size / (2 * 1024 * 1024);

        for (size_t j = 0; j < num_pt1; j++) {
            uint64_t pt1_addr = pt1_base + j * 0x1000;
            uint64_t *pt1 = (void *)(v->mem + pt1_addr);
            memset(pt1, 0, 0x1000);

            pt2[j] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pt1_addr;

            for (size_t i = 0; i < 512; i++) {
                uint64_t phys = (j * 512 + i) * 0x1000ULL;
                pt1[i] = phys | PDE64_PRESENT | PDE64_RW | PDE64_USER;
            }
        }
    } else {
        fprintf(stderr, "Nepodržana veličina stranice: %zu\n", v->page_size);
        exit(1);
    }

    sregs->cr3  = pt4_addr;
    sregs->cr4  = CR4_PAE;
    sregs->cr0  = CR0_PE | CR0_PG;
    sregs->efer = EFER_LME | EFER_LMA;

    setup_segments_64(sregs);
}

int load_guest_image(struct vm *v, const char *image_path, uint64_t load_addr) {
    FILE *f = fopen(image_path, "rb");
    if (!f) {
        perror("Failed to open guest image");
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    if((uint64_t)fsz > v->mem_size - load_addr) {
        printf("Guest image is too large for the VM memory\n");
        fclose(f);
        return -1;
    }

    if (fread((uint8_t*)v->mem + load_addr, 1, (size_t)fsz, f) != (size_t)fsz) {
        perror("Failed to read guest image");
        fclose(f);
        return -1;
    }
    fclose(f);

    return 0;
}

static void make_local_name(int vm_id, const char *guest_path, char *out, size_t outsz) {
    snprintf(out, outsz, "vm%d_%s", vm_id, guest_path);
}

static int copy_file_contents(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int alloc_guest_fd(struct vm *v, FILE *fp) {
    for (int i = 0; i < 32; i++) {
        if (v->fd[i] == NULL) {
            v->fd[i] = fp;
            return i;
        }
    }
    return -1;
}

static int vm_is_shared(struct vm *v, const char *path) {
    for (int i = 0; i < v->local_shared_count; i++) {
        if (strcmp(v->local_shared[i], path) == 0) return 1;
    }
    return 0;
}

static void vm_remove_shared(struct vm *v, const char *path) {
    for (int i = 0; i < v->local_shared_count; i++) {
        if (strcmp(v->local_shared[i], path) == 0) {
            free(v->local_shared[i]);
            for (int j = i; j < v->local_shared_count - 1; j++) {
                v->local_shared[j] = v->local_shared[j+1];
            }
            v->local_shared_count--;
            break;
        }
    }
}

void *run_vm(void *arg){
    struct vm_args *args = (struct vm_args *)arg;

    struct vm v;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    int stop = 0, ret;

    if (vm_init(&v, args->mem_size, args->page_size)) {
        fprintf(stderr, "Failed to init VM for %s\n", args->guest_path);
        return NULL;
    }

    v.local_shared_count = shared_count;
    for (int i = 0; i < shared_count; i++) {
        v.local_shared[i] = strdup(shared_files[i]);
    }

    ioctl(v.vcpu_fd, KVM_GET_SREGS, &sregs);
    setup_long_mode(&v, &sregs);
    ioctl(v.vcpu_fd, KVM_SET_SREGS, &sregs);

    if (load_guest_image(&v, args->guest_path, GUEST_START_ADDR) < 0) {
        vm_destroy(&v);
        return NULL;
    }

    memset(&regs, 0, sizeof(regs));
    regs.rflags = 0x2;
    regs.rip = GUEST_START_ADDR;
    regs.rsp = v.mem_size;
    ioctl(v.vcpu_fd, KVM_SET_REGS, &regs);

    while (!stop) {
        ret = ioctl(v.vcpu_fd, KVM_RUN, 0);
        if (ret == -1) break;

        switch (v.run->exit_reason) {
            case KVM_EXIT_IO:
                if (v.run->io.direction == KVM_EXIT_IO_OUT && v.run->io.port == PORT_FILE) {
                    struct FileRequest* req = (struct FileRequest*)(v.mem+REQ_ADDR);
                    char *buf = (char *)(v.mem + BUF_ADDR);

                    switch(req->cmd){
                        case CMD_OPEN: {
                            char *guest_path = buf;
                            int shared = vm_is_shared(&v, guest_path);
                            char target_path[256];
                            FILE *fp = NULL;

                            if (shared) {
                                if (req->flags == FILE_READ) {
                                    strncpy(target_path, guest_path, sizeof(target_path)-1);
                                    target_path[sizeof(target_path)-1] = '\0';
                                    fp = fopen(target_path, "rb");
                                } else {
                                    make_local_name(args->vm_id, guest_path, target_path, sizeof(target_path));
                                    if (req->flags == FILE_APPEND) {
                                        if (copy_file_contents(guest_path, target_path) != 0) {
                                            req->result = -1;
                                            break;
                                        }
                                        fp = fopen(target_path, "ab+");
                                    } else {
                                        if (copy_file_contents(guest_path, target_path) != 0) {
                                            fp = fopen(target_path, "wb+");
                                        } else {
                                            fp = fopen(target_path, "wb+");
                                        }
                                    }
                                    vm_remove_shared(&v, guest_path);
                                }
                            } else {
                                make_local_name(args->vm_id, guest_path, target_path, sizeof(target_path));
                                if (req->flags == FILE_READ)      fp = fopen(target_path, "rb");
                                else if (req->flags == FILE_WRITE) fp = fopen(target_path, "wb+");
                                else                               fp = fopen(target_path, "ab+");
                            }

                            if (!fp) { req->result = -1; break; }
                            int guest_fd = alloc_guest_fd(&v, fp);
                            if (guest_fd < 0) { fclose(fp); req->result = -1; break; }
                            req->result = guest_fd;
                            break;
                        }
                        case CMD_WRITE: {
                            FILE *fp = v.fd[req->fd];
                            if (fp) {
                                char *data = buf;
                                size_t n = fwrite(data, 1, req->size, fp);
                                fflush(fp);
                                req->result = (int32_t)n;
                            } else req->result = -1;
                            break;
                        }
                        case CMD_READ: {
                            FILE *fp = v.fd[req->fd];
                            if (fp) {
                                char *dest = buf;
                                size_t n = fread(dest,sizeof(char), req->size, fp);
                                req->result = (int32_t)n;
                            } else req->result = -1;
                            break;
                        }
                        case CMD_CLOSE: {
                            FILE *fp = v.fd[req->fd];
                            if (fp) {
                                fclose(fp);
                                v.fd[req->fd] = NULL;
                                req->result = 0;
                            } else req->result = -1;
                            break;
                        }
                        default:
                            req->result = -1;
                            break;
                    }
                } else if (v.run->io.direction == KVM_EXIT_IO_OUT && v.run->io.port == 0xE9) {
                    char *p = (char *)v.run;
                    char ch = *(p + v.run->io.data_offset);
                    printf("%c", ch);
                    fflush(stdout);
                }
                continue;

            case KVM_EXIT_HLT:
                printf("\nHLT\n");
                stop = 1;
                break;

            default:
                printf("\nExit reason %d\n",v.run->exit_reason);
                stop = 1;
                break;
        }
    }

    vm_destroy(&v);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 7) {
        fprintf(stderr, "Usage: %s --memory <MB> --page <2|4> --guest <img1> [img2 ...] [--file f1 f2...]\n", argv[0]);
        return 1;
    }

    size_t mem_size = (size_t)atoi(argv[2]) * 1024 * 1024;
    int val = atoi(argv[4]);
    size_t page_size;
    if (val == 2) page_size = 0x200000;
    else if (val == 4) page_size = 0x1000;
    else {
        fprintf(stderr, "Unsupported page size: %d\n", val);
        return 1;
    }

    int file_flag_idx = -1;
    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            file_flag_idx = i;
            break;
        }
    }

    if (file_flag_idx != -1) {
        for (int j = file_flag_idx + 1; j < argc && shared_count < MAX_SHARED; j++) {
            shared_files[shared_count++] = argv[j];
        }
    }

    int num_guests = (file_flag_idx == -1 ? argc : file_flag_idx) - 6;
    if (num_guests <= 0) {
        fprintf(stderr, "No guests provided.\n");
        return 1;
    }

    pthread_t threads[num_guests];
    struct vm_args args[num_guests];

    for (int i = 0; i < num_guests; i++) {
        args[i].mem_size = mem_size;
        args[i].page_size = page_size;
        args[i].guest_path = argv[6 + i];
        args[i].vm_id = i+1;
        pthread_create(&threads[i], NULL, run_vm, &args[i]);
    }

    for (int i = 0; i < num_guests; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}
