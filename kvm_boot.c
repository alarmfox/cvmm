#include <asm/bootparam.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <signal.h>
#include <termios.h>

#include "serial.h"

static struct termios g_orig_term;
static int g_term_saved = 0;
static volatile int g_shouldexit = 0;

static void term_restore(void) {
    if (g_term_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

static void on_signal(int sig) {
    term_restore();
    __atomic_store_n(&g_shouldexit, 1, __ATOMIC_RELAXED);
    _exit(128 + sig);
}

/*
 * Minimal kernel config
 *
 * CONFIG_64BIT=y
 * CONFIG_PRINTK=y
 * CONFIG_EARLY_PRINTK=y
 * CONFIG_X86_VERBOSE_BOOTUP=y
 * CONFIG_TTY=y
 * CONFIG_SERIAL_8250=y
 * CONFIG_SERIAL_8250_CONSOLE=y
 * CONFIG_BLK_DEV_INITRD=y
 * CONFIG_BINFMT_ELF=y
 * CONFIG_BINFMT_SCRIPT=y
 * CONFIG_DEVTMPFS=y
 *
 * make -C /path/to/kernel/src O=$(pwd)/kbuild-minimal/ KCONFIG_ALLCONFIG=$(pwd)/minimal.config allnoconfig
 * make -C /path/to/kernel/src O=$(pwd)/kbuild-minimal/ KCONFIG_ALLCONFIG=$(pwd)/minimal.config -j`nproc` bzImage
 * cp $(pwd)/kbuild-minimal/arch/x86/boot/bzImage .
 * gcc -Wall -Wextra kvm_boot.c
 * ./kvm_boot bzImage initramfs.cpio.gz
 *
 */

static void dump_guest_state(int vcpufd) {
    struct kvm_regs regs;
    struct kvm_sregs sregs;

    printf("**** START DUMP *****\n");

    ioctl(vcpufd, KVM_GET_REGS, &regs);
    ioctl(vcpufd, KVM_GET_SREGS, &sregs);

    printf("RIP=0x%llx\n", regs.rip);
    printf("RSP=0x%llx\n", regs.rsp);
    printf("RFLAGS=0x%llx\n", regs.rflags);

    printf("CR0=0x%llx\n", sregs.cr0);
    printf("CR3=0x%llx\n", sregs.cr3);
    printf("CR4=0x%llx\n", sregs.cr4);
    printf("EFER=0x%llx\n", sregs.efer);
    printf("GDT=0x%llx\n", sregs.gdt.base);

    printf("CS selector=%x base=%llx type=%x present=%d\n",
            sregs.cs.selector,
            sregs.cs.base,
            sregs.cs.type,
            sregs.cs.present);

    printf("**** END DUMP *****\n");
}

#define GUEST_MEM (1 << 27)

#define ZERO_PAGE_START      0x00010000
#define CMDLINE_START        0x00020000
#define KERNEL_LOAD_ADDR     0x00100000

#define INITRD_ALIGN         0x00001000

#define BOOT_STACK           0x00080000

#define GDT_START            0x00001000

/* Setup the guest VM. Init IRQ chip, memory mapping and TSS. Returns the guestfd */
static int setup_guest_vm(int kvmfd, void **mem) {
    int guestfd;
    int ret;
    __u64 map_addr = 0xffffc000;

    guestfd = ioctl(kvmfd, KVM_CREATE_VM, 0);

    if(guestfd< 0) {
        fprintf(stderr, "kvm cannot create vm: %s", strerror(errno));
        return 1;
    }

    ret = ioctl(guestfd, KVM_SET_TSS_ADDR, 0xffffd000);

    if (ret < 0) {
        fprintf(stderr, "failed to set tss addr");
        return 1;
    }

    ret = ioctl(guestfd, KVM_SET_IDENTITY_MAP_ADDR, &map_addr);

    if (ret < 0) {
        fprintf(stderr, "failed to set identity map addr");
        return 1;
    }

    /* Add IRQCHIP */
    ret = ioctl(guestfd, KVM_CREATE_IRQCHIP, 0);

    if(ret < 0) {
        fprintf(stderr, "kvm cannot add IRQ chip to vm: %s", strerror(errno));
        return 1;
    }

    /* Create PIT for timer interrupts */
    struct kvm_pit_config pit = {
        .flags = 0,
    };

    ret = ioctl(guestfd, KVM_CREATE_PIT2, &pit);
    if ( ret < 0) {
        fprintf(stderr, "kvm cannot add PIT2 chip to vm: %s", strerror(errno));
        return 1;
    }

    /* Map guest memory to userspace memory */
    *mem = mmap(NULL, GUEST_MEM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if (mem == NULL) {
        fprintf(stderr, "cannot map guest memory to userspace: %s", strerror(errno));
        return 1;
    }

    /* Configure and add memory region to the VM */
    struct kvm_userspace_memory_region region;
    memset(&region, 0, sizeof(region));
    region.slot = 0;
    region.guest_phys_addr = 0;
    region.memory_size = GUEST_MEM;
    region.userspace_addr = (uintptr_t)*mem;

    ret = ioctl(guestfd, KVM_SET_USER_MEMORY_REGION, &region);

    if(ret < 0) {
        fprintf(stderr, "kvm cannot add memory region to vm: %s", strerror(errno));
        return 1;
    }

    return guestfd;
}

/* Create the vCPU and setup registers to boot a Linux kernel. Returns the vcpufd */
static int setup_guest_vcpu(int kvmfd, int guestfd, void *mem) {
    int vcpufd;
    int ret;

    vcpufd = ioctl(guestfd, KVM_CREATE_VCPU, 0);

    if(vcpufd< 0) {
        fprintf(stderr, "kvm cannot create vcpu: %s\n", strerror(errno));
        return 1;
    }

    /* Setup CPUID */
    struct {
        uint32_t nent;
        uint32_t padding;
        struct kvm_cpuid_entry2 entries[100];
    } kvm_cpuid;
    kvm_cpuid.nent = sizeof(kvm_cpuid.entries) / sizeof(kvm_cpuid.entries[0]);
    ioctl(kvmfd, KVM_GET_SUPPORTED_CPUID, &kvm_cpuid);

    for (unsigned int i = 0; i < kvm_cpuid.nent; i++) {
        struct kvm_cpuid_entry2 *entry = &kvm_cpuid.entries[i];
        if (entry->function == KVM_CPUID_SIGNATURE) {
            entry->eax = KVM_CPUID_FEATURES;
            entry->ebx = 0x4b4d564b; // KVMK
            entry->ecx = 0x564b4d56; // VMKV
            entry->edx = 0x4d;       // M
        }
    }

    /* Setup GDT */
    uint64_t *gdt = (uint64_t *)((uint8_t *)mem + GDT_START);

    /* Null descriptor */
    gdt[0] = 0x0000000000000000ULL;

    /* 32-bit flat kernel code segment */
    gdt[1] = 0x00CF9A000000FFFFULL;

    /* 32-bit flat kernel data segment */
    gdt[2] = 0x00CF92000000FFFFULL;

    ret = ioctl(vcpufd, KVM_SET_CPUID2, &kvm_cpuid);

    if(ret < 0) {
        fprintf(stderr, "cannot set cpuid: %s", strerror(errno));
        return 1;
    }

    /* Setup vCPU special registers. For a Linux kernel we need real mode */
    struct kvm_sregs sregs;

    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    if (ret < 0) {
        fprintf(stderr, "failed to get special registers");
        return 1;
    }

    sregs.gdt.base  = GDT_START;
    sregs.gdt.limit = 3 * 8 - 1;

    /* Code segment (selector 0x08) */
    sregs.cs.base     = 0;
    sregs.cs.limit    = 0xffffffff;
    sregs.cs.selector = 0x08;
    sregs.cs.type     = 0x0b;      /* execute/read/accessed */
    sregs.cs.present  = 1;
    sregs.cs.dpl      = 0;
    sregs.cs.db       = 1;
    sregs.cs.s        = 1;
    sregs.cs.l        = 0;
    sregs.cs.g        = 1;
    sregs.cs.avl      = 0;

    /* Data segments (selector 0x10) */
    sregs.ds.base     = 0;
    sregs.ds.limit    = 0xffffffff;
    sregs.ds.selector = 0x10;
    sregs.ds.type     = 0x03;      /* read/write/accessed */
    sregs.ds.present  = 1;
    sregs.ds.dpl      = 0;
    sregs.ds.db       = 1;
    sregs.ds.s        = 1;
    sregs.ds.l        = 0;
    sregs.ds.g        = 1;
    sregs.ds.avl      = 0;

    sregs.es = sregs.ds;
    sregs.fs = sregs.ds;
    sregs.gs = sregs.ds;
    sregs.ss = sregs.ds;

    sregs.cr0 |= 0x11;   /* Enable protected mode */
    sregs.cr4 = 0;
    sregs.efer = 0;

    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if (ret < 0) {
        fprintf(stderr, "failed to set special registers");
        return 1;
    }

    /* Setup vCPU regular registers.*/
    struct kvm_regs regs;

    ret = ioctl(vcpufd, KVM_GET_REGS, &regs);
    if (ret < 0) {
        fprintf(stderr, "failed to get regular registers");
        return 1;
    }

    memset(&regs, 0, sizeof(regs));

    /* Linux boot protocol */
    regs.rip    = KERNEL_LOAD_ADDR;      /* Protected-mode entry */
    regs.rsi    = ZERO_PAGE_START;       /* struct boot_params */
    regs.rsp    = BOOT_STACK;
    regs.rbp    = BOOT_STACK;
    regs.rflags = 0x2;                   /* Reserved bit must be set */

    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if (ret < 0) {
        fprintf(stderr, "failed to get regular registers");
        return 1;
    }

    return vcpufd;
}

static int load_guest_image(const void *mem, const char *image_path, const char *initrd_image) {

    if (mem == NULL || image_path == NULL || initrd_image == NULL) return -1;

    void *data= NULL;
    struct boot_params *boot = (struct boot_params *)((uint8_t *)mem + ZERO_PAGE_START);

    /* Load the kernel image */
    int fd = open(image_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cannot open guest image at %s: %s", image_path, strerror(errno));
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    data = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);

    memset(boot, 0, sizeof(struct boot_params));
    memcpy(boot, data, 4096);

    close(fd); fd = 1;

    /*
     * Linux 32-bit mode boot protocol. It allows to boot directly in protected mode skipping a bootloader
     */
    /* Check header validity */
    if (boot->hdr.boot_flag != 0xaa55) return -1;
    if (boot->hdr.header != 0x53726448 /* "HdrS" */) return -1;

    unsigned setup_sects = boot->hdr.setup_sects;

    if (setup_sects == 0) setup_sects = 4;

    size_t setup_size = (setup_sects + 1) * 512;

    memcpy((uint8_t *)mem + KERNEL_LOAD_ADDR, (uint8_t *)data + setup_size, st.st_size - setup_size);

    strcpy((char *)mem + CMDLINE_START, "earlyprintk=serial,ttyS0,115200 console=ttyS0 acpi=off pci=off panic=-1 rebooto=t");

    /* Load flags */
    boot->hdr.type_of_loader    = 0xFF;
    boot->hdr.cmd_line_ptr      = CMDLINE_START;
    boot->hdr.cmdline_size      = 4096;

    boot->hdr.heap_end_ptr      = 0xfe00;

    boot->e820_entries = 3;

    boot->e820_table[0].addr    = 0x00000000;
    boot->e820_table[0].size    = 0x0009fc00;
    boot->e820_table[0].type    = 1;

    boot->e820_table[1].addr    = 0x000f0000;
    boot->e820_table[1].size    = 0x00010000;
    boot->e820_table[1].type    = 2;

    boot->e820_table[2].addr    = 0x00100000;
    boot->e820_table[2].size    = GUEST_MEM - 0x00100000;
    boot->e820_table[2].type    = 1;

    printf("version              0x%x\n", boot->hdr.version);
    printf("loadflags            0x%x\n", boot->hdr.loadflags);
    printf("relocatable          0x%d\n", boot->hdr.relocatable_kernel);
    if (boot->hdr.relocatable_kernel) printf("pref_address         0x%llx\n", boot->hdr.pref_address);
    printf("kernel_alignment     0x%x\n", boot->hdr.kernel_alignment);
    printf("code32_start         0x%x\n", boot->hdr.code32_start);

    munmap(data, st.st_size);

    /* Load initrd */

    fd = open(initrd_image, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cannot open initrd image at %s: %s", initrd_image, strerror(errno));
        return -1;
    }
    fstat(fd, &st);

    data = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);

    uint32_t initrd_addr = (GUEST_MEM - st.st_size) & ~(INITRD_ALIGN - 1);

    /*
     * Keep it above the kernel.
     */
    if (initrd_addr < KERNEL_LOAD_ADDR + st.st_size) {
        fprintf(stderr, "initrd does not fit in guest memory\n");
        return -1;
    }

    memcpy((uint8_t *)mem + initrd_addr,data, st.st_size);

    boot->hdr.ramdisk_image = initrd_addr;
    boot->hdr.ramdisk_size  = st.st_size;

    munmap(data, st.st_size);

    printf("initrd loaded at %#x (%zu bytes)\n",  initrd_addr, (size_t)st.st_size);

    return 0;
}

int main(int argc, const char **argv) {
    int ret = 1;
    int kvmfd = -1;
    int guestfd = -1;
    int vcpufd = -1;
    void *mem = NULL;

    if (argc < 3) {
        fprintf(stderr, "Usage: ./%s <kernel path> <initramfs path>\n", argv[0]);
        goto exit;
    }

    kvmfd = open("/dev/kvm", O_RDWR);
    if(kvmfd < 0) {
        fprintf(stderr, "cannot open /dev/kvm: %s\n", strerror(errno));
        goto exit;
    }

    guestfd = setup_guest_vm(kvmfd, &mem);
    if(guestfd < 0) goto exit;

    printf("[GUEST] memory address mapped at: %lx\n", (uintptr_t)mem);

    ret = load_guest_image(mem, argv[1], argv[2]);
    if(ret < 0) goto exit;

    vcpufd = setup_guest_vcpu(kvmfd, guestfd, mem);
    if(vcpufd < 0) goto exit;

    /* Map serial */
    ret = serial8250__init(guestfd);
    if(ret) {
        fprintf(stderr, "cannot attach serial\n");
        goto exit;
    }

    /* Run the KVM vCPU */
    int run_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run = mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);

    dump_guest_state(vcpufd);

    ret =  tcgetattr(STDIN_FILENO, &g_orig_term);
    if (ret == 0) {
        struct termios raw = g_orig_term;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(ICRNL | INLCR | IXON | IXOFF | ISTRIP);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        g_term_saved = 1;
        atexit(term_restore);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);


    /* Main loop */
    while(!__atomic_load_n(&g_shouldexit, __ATOMIC_RELAXED)) {
        /* Execute the VCPU */
        ret = ioctl(vcpufd, KVM_RUN, 0);

        if (ret < 0) {
            fprintf(stderr, "kvm run failed: %s\n", strerror(errno));
            __atomic_store_n(&g_shouldexit, 1, __ATOMIC_RELAXED);
            continue;
        }

        switch (run->exit_reason) {
        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr, "guest exited\n");
            __atomic_store_n(&g_shouldexit, 1, __ATOMIC_RELAXED);
            break;

        case KVM_EXIT_IO: {
          uint8_t *data = (uint8_t *)run + run->io.data_offset;
          int is_write = run->io.direction == KVM_EXIT_IO_OUT;

          for (uint32_t i = 0; i < run->io.count; i++) {
              uint8_t *p = data + (size_t) i * run->io.size;
              if (!serial8250__io(run->io.port, p, is_write) && !is_write)
                  memset(data, 0xff, (size_t)run->io.size);
          }
          break;
        }

        case KVM_EXIT_MMIO:
          printf("MMIO %s addr=0x%llx len=%u\n",
                  run->mmio.is_write ? "WRITE" : "READ",
                  (unsigned long long)run->mmio.phys_addr,
                  run->mmio.len);

          if (!run->mmio.is_write)
              memset(run->mmio.data, 0xFF, run->mmio.len);

          // dump_guest_state(vcpufd);

          break;

        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR\n");
            fprintf(stderr, "suberror = %u\n", run->internal.suberror);
            fprintf(stderr, "ndata = %u\n", run->internal.ndata);

            for (unsigned i = 0; i < run->internal.ndata; i++)
                fprintf(stderr, "data[%u] = 0x%llx\n", i, (unsigned long long)run->internal.data[i]);
            __atomic_store_n(&g_shouldexit, 1, __ATOMIC_RELAXED);
            break;

        case KVM_EXIT_HLT:
            fprintf(stderr, "guest halted\n");
            __atomic_store_n(&g_shouldexit, 1, __ATOMIC_RELAXED);
            break;

        default:
            fprintf(stderr, "unknown exit_reason: %d\n", run->exit_reason);
            __atomic_store_n(&g_shouldexit, 1, __ATOMIC_RELAXED);
            break;
        }
    }

    ret = 0;
exit:
    /* Guest exit */
    if(kvmfd > 0) close(kvmfd);
    if(vcpufd > 0) close(vcpufd);
    if(guestfd > 0) close(guestfd);
    if(mem) munmap(mem, GUEST_MEM);

    return ret;
}
