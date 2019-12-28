#include <stdint.h>
#include <stddef.h>
#include <lib/cio.h>
#include <lib/klib.h>
#include <devices/display/vbe/vbe.h>
#include <devices/term/tty/tty.h>
#include <sys/e820.h>
#include <mm/mm.h>
#include <sys/idt.h>
#include <sys/pic.h>
#include <acpi/acpi.h>
#include <lib/cmdline.h>
#include <sys/hpet.h>
#include <sys/smp.h>
#include <proc/task.h>
#include <devices/dev.h>
#include <fd/vfs/vfs.h>
#include <proc/elf.h>
#include <sys/pci.h>
#include <lib/time.h>
#include <sys/irq.h>
#include <sys/panic.h>
#include <fs/fs.h>
#include <fd/fd.h>
#include <devices/dev.h>
#include <sys/vga_font.h>
#include <lib/rand.h>
#include <sys/urm.h>
#include <net/hostname.h>
#include <net/e1000.h>
#include <lib/cstring.h>

#include <lai/core.h>
#include <lai/helpers/sci.h>

void kmain_thread(void *arg) {
    (void)arg;

    /* Launch the urm */
    task_tcreate(0, tcreate_fn_call, tcreate_fn_call_data(0, userspace_request_monitor, 0));

    /* Initialise file descriptor handlers */
    init_fd();

    /* Initialise filesystem drivers */
    init_fs();

    /* Mount /dev */
    mount("devfs", "/dev", "devfs", 0, 0);

    /* Initialise device drivers */
    init_dev();

    /* Initialise E1000 based cards. */
    init_e1000();

    int tty = open("/dev/tty0", O_RDWR);

    char root[64];
    if (!cmdline_get_value(root, 64, "root")) {
        kprint(KPRN_WARN, "kmain: Command line argument \"root\" not specified.");
        readline(tty, "Select root device: ", root, 64);
    }
    kprint(KPRN_INFO, "kmain: root=%s", root);

    char rootfs[64];
    if (!cmdline_get_value(rootfs, 64, "rootfs")) {
        kprint(KPRN_WARN, "kmain: Command line argument \"rootfs\" not specified.");
        readline(tty, "Root filesystem to use: ", rootfs, 64);
    }
    kprint(KPRN_INFO, "kmain: rootfs=%s", rootfs);

    char init[64];
    if (!cmdline_get_value(init, 64, "init")) {
        kprint(KPRN_WARN, "kmain: Command line argument \"init\" not specified.");
        readline(tty, "Location of init: ", init, 64);
    }
    kprint(KPRN_INFO, "kmain: init=%s", init);

    close(tty);

    /* Mount root partition */
    if (mount(root, "/", rootfs, 0, 0)) {
        panic("Unable to mount root", 0, 0, NULL);
    }

    /* Set hostname */
    init_hostname();

    /* Execute init process */
    kprint(KPRN_INFO, "kmain: Starting init");
    const char *args[] = { init, NULL };
    const char *environ[] = { NULL };
    if (kexec(init, args, environ, "/dev/tty0", "/dev/tty0", "/dev/tty0") == -1) {
        panic("Unable to launch init", 0, 0, NULL);
    }
    if (kexec(init, args, environ, "/dev/tty1", "/dev/tty1", "/dev/tty1") == -1) {
        panic("Unable to launch init", 0, 0, NULL);
    }
    if (kexec(init, args, environ, "/dev/tty2", "/dev/tty2", "/dev/tty2") == -1) {
        panic("Unable to launch init", 0, 0, NULL);
    }
    if (kexec(init, args, environ, "/dev/tty3", "/dev/tty3", "/dev/tty3") == -1) {
        panic("Unable to launch init", 0, 0, NULL);
    }
    if (kexec(init, args, environ, "/dev/tty4", "/dev/tty4", "/dev/tty4") == -1) {
        panic("Unable to launch init", 0, 0, NULL);
    }
    if (kexec(init, args, environ, "/dev/tty5", "/dev/tty5", "/dev/tty5") == -1) {
        panic("Unable to launch init", 0, 0, NULL);
    }

    kprint(KPRN_INFO, "kmain: End of kmain");

    /* kill kmain now */
    task_tkill(CURRENT_PROCESS, CURRENT_THREAD);

    for (;;) asm volatile ("hlt;");
}

/* Main kernel entry point, only initialise essential services and scheduler */
void kmain(void) {
    char cmdline_val[64];

    kprint(KPRN_INFO, "Kernel booted");
    kprint(KPRN_INFO, "Build time: %s", BUILD_TIME);
    kprint(KPRN_INFO, "Command line: %s", cmdline);

    init_idt();
    init_cpu_features();

    /* Memory-related stuff */
    init_e820();
    init_pmm();
    init_rand();
    init_vmm();

    init_vbe();
    init_tty();

    /* Time stuff */
    struct s_time_t s_time;
    bios_get_time(&s_time);
    kprint(KPRN_INFO, "Current date & time: %u/%u/%u %u:%u:%u",
           s_time.years, s_time.months, s_time.days,
           s_time.hours, s_time.minutes, s_time.seconds);
    unix_epoch = get_unix_epoch(s_time.seconds, s_time.minutes, s_time.hours,
                                s_time.days, s_time.months, s_time.years);
    kprint(KPRN_INFO, "Current unix epoch: %U", unix_epoch);

    /*** NO MORE REAL MODE CALLS AFTER THIS POINT ***/
    flush_irqs();
    init_acpi();
    init_pic();

    /* Init the HPET */
    init_hpet();

    /* Initialise PCI */
    init_pci();

    /* Init Symmetric Multiprocessing */
    asm volatile ("sti");
    init_smp();

    /* LAI */
    if (!cmdline_get_value(cmdline_val, 64, "acpi") || !strcmp(cmdline_val, "enabled")) {
        lai_set_acpi_revision(rsdp->rev);
        lai_create_namespace();
        // lai_enable_acpi(1);
    }
    asm volatile ("cli");

    /* Initialise scheduler */
    init_sched();

    /* Unlock the scheduler for the first time */
    spinlock_release(&scheduler_lock);

    /* Start a main kernel thread which will take over when the scheduler is running */
    task_tcreate(0, tcreate_fn_call, tcreate_fn_call_data(0, kmain_thread, 0));

    /*** DO NOT ADD ANYTHING TO THIS FUNCTION AFTER THIS POINT, ADD TO kmain_thread
         INSTEAD! ***/

    /* Pre-scheduler init done. Wait for the main kernel thread to be scheduled. */
    asm volatile ("sti");
    for (;;) asm volatile ("hlt");
}
