#include "types.h"
#include "globals.h"
#include "kernel.h"

#include "util/gdb.h"
#include "util/init.h"
#include "util/debug.h"
#include "util/string.h"
#include "util/printf.h"

#include "mm/mm.h"
#include "mm/page.h"
#include "mm/pagetable.h"
#include "mm/pframe.h"

#include "vm/vmmap.h"
#include "vm/shadow.h"
#include "vm/anon.h"

#include "main/acpi.h"
#include "main/apic.h"
#include "main/interrupt.h"
#include "main/cpuid.h"
#include "main/gdt.h"

#include "proc/sched.h"
#include "proc/proc.h"
#include "proc/kthread.h"

#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "drivers/tty/virtterm.h"

#include "api/exec.h"
#include "api/syscall.h"

#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/fcntl.h"
#include "fs/stat.h"

#include "test/kshell/kshell.h"

GDB_DEFINE_HOOK(boot)
GDB_DEFINE_HOOK(initialized)
GDB_DEFINE_HOOK(shutdown)

static void      *bootstrap(int arg1, void *arg2);
static void      *idleproc_run(int arg1, void *arg2);
static kthread_t *initproc_create(void);
static void      *initproc_run(int arg1, void *arg2);
static void       hard_shutdown(void);

static context_t bootstrap_context;

/**
 * This is the first real C function ever called. It performs a lot of
 * hardware-specific initialization, then creates a pseudo-context to
 * execute the bootstrap function in.
 */
void
kmain()
{
        GDB_CALL_HOOK(boot);

        dbg_init();
        dbgq(DBG_CORE, "Kernel binary:\n");
        dbgq(DBG_CORE, "  text: 0x%p-0x%p\n", &kernel_start_text, &kernel_end_text);
        dbgq(DBG_CORE, "  data: 0x%p-0x%p\n", &kernel_start_data, &kernel_end_data);
        dbgq(DBG_CORE, "  bss:  0x%p-0x%p\n", &kernel_start_bss, &kernel_end_bss);

        page_init();

        pt_init();
        slab_init();
        pframe_init();

        acpi_init();
        apic_init();
        intr_init();

        gdt_init();

        /* initialize slab allocators */
#ifdef __VM__
        anon_init();
        shadow_init();
#endif
        vmmap_init();
        proc_init();
        kthread_init();

#ifdef __DRIVERS__
        bytedev_init();
        blockdev_init();
#endif

        void *bstack = page_alloc();
        pagedir_t *bpdir = pt_get();
        KASSERT(NULL != bstack && "Ran out of memory while booting.");
	/* This little loop gives gdb a place to synch up with weenix.  In the
	 * past the weenix command started qemu was started with -S which
	 * allowed gdb to connect and start before the boot loader ran, but
	 * since then a bug has appeared where berakpoints fail if gdb connects
	 * before the boot loader runs.  See
	 *
	 * https://bugs.launchpad.net/qemu/+bug/526653
	 *
	 * This loop (along with an additional command in init.gdb setting
	 * gdb_wait to 0) sticks weenix at a known place so gdb can join a
	 * running weenix, set gdb_wait to zero  and catch the breakpoint in
	 * bootstrap below.  See Config.mk for how to set GDBWAIT correctly.
	 *
	 * DANGER: if GDBWAIT != 0, and gdb isn't run, this loop will never
	 * exit and weenix will not run.  Make SURE the GDBWAIT is set the way
	 * you expect.
	 */
	int gdb_wait = GDBWAIT;

	while (gdb_wait)
	    ;
        context_setup(&bootstrap_context, bootstrap, 0, NULL, bstack, PAGE_SIZE, bpdir);
        context_make_active(&bootstrap_context);

        panic("\nReturned to kmain()!!!\n");
}

/**
 * This function is called from kmain, however it is not running in a
 * thread context yet. It should create the idle process which will
 * start executing idleproc_run() in a real thread context.  To start
 * executing in the new process's context call context_make_active(),
 * passing in the appropriate context. This function should _NOT_
 * return.
 *
 * Note: Don't forget to set curproc and curthr appropriately.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *
bootstrap(int arg1, void *arg2)
{
        /* necessary to finalize page table information */
        pt_template_init();

        /* PROCS {{{ */
        /* Set up our initial process and jump into it */
        curproc = proc_create("idle");
        KASSERT(NULL != curproc);
        KASSERT(PID_IDLE == curproc->p_pid);

        curthr = kthread_create(curproc, idleproc_run, 0, NULL);
        KASSERT(NULL != curthr);

        dbg(DBG_INIT, "Starting idle proc\n");
        context_make_active(&curthr->kt_ctx);

        /* PROCS }}} */

        panic("weenix returned to bootstrap()!!! BAD!!!\n");
        return NULL;
}

/**
 * Once we're inside of idleproc_run(), we are executing in the context of the
 * first process-- a real context, so we can finally begin running
 * meaningful code.
 *
 * This is the body of process 0. It should initialize all that we didn't
 * already initialize in kmain(), launch the init process (initproc_run),
 * wait for the init process to exit, then halt the machine.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *
idleproc_run(int arg1, void *arg2)
{
        int status;
        pid_t child;

        /* create init proc */
        kthread_t *initthr = initproc_create();

        init_call_all();
        GDB_CALL_HOOK(initialized);

        /* Create other kernel threads (in order) */
        /* PROCS BLANK {{{ */
#ifdef __SHADOWD__
        /* TODO port this - alvin */
#endif
        /* PROCS BLANK }}} */

#ifdef __VFS__
        /* Once you have VFS remember to set the current working directory
         * of the idle and init processes */
        /* PROCS BLANK {{{ */
        proc_t *idle = proc_lookup(PID_IDLE);
        proc_t *init = proc_lookup(PID_INIT);
        KASSERT(NULL != idle);
        KASSERT(NULL != init);
        idle->p_cwd = vfs_root_vn;
        init->p_cwd = vfs_root_vn;
        vref(vfs_root_vn);
        vref(vfs_root_vn);
        /* PROCS BLANK }}} */

        /* Here you need to make the null, zero, and tty devices using mknod */
        /* You can't do this until you have VFS, check the include/drivers/dev.h
         * file for macros with the device ID's you will need to pass to mknod */
        /* PROCS BLANK {{{ */
        int fd, ii;
        char path[32];

        struct stat statbuf;
        if (do_stat("/dev", &statbuf) < 0) {
                KASSERT(!(status = do_mkdir("/dev")));
        }
        if ((fd = do_open("/dev/null", O_RDONLY)) < 0) {
                KASSERT(!(status = do_mknod("/dev/null", S_IFCHR, MEM_NULL_DEVID)));
        } else {
                do_close(fd);
        }
        if ((fd = do_open("/dev/zero", O_RDONLY)) < 0) {
                KASSERT(!(status = do_mknod("/dev/zero", S_IFCHR, MEM_ZERO_DEVID)));
        } else {
                do_close(fd);
        }

        memset(path, '\0', 32);
        for (ii = 0; ii < __NTERMS__; ii++) {
                sprintf(path, "/dev/tty%d", ii);
                dbg(DBG_INIT, "Creating tty mknod with path %s\n", path);
                if ((fd = do_open(path, O_RDONLY)) < 0) {
                        KASSERT(!do_mknod(path, S_IFCHR, MKDEVID(2, ii)));
                } else {
                        do_close(fd);
                }
        }

        for (ii = 0; ii < __NDISKS__; ii++) {
                sprintf(path, "/dev/hda%d", ii);
                dbg(DBG_INIT, "Creating disk mknod with path %s\n", path);
                if ((fd = do_open(path, O_RDONLY)) < 0) {
                        KASSERT(!do_mknod(path, S_IFBLK, MKDEVID(1, ii)));
                } else {
                        do_close(fd);
                }
        }
        /* PROCS BLANK }}} */
#endif

        /* Finally, enable interrupts (we want to make sure interrupts
         * are enabled AFTER all drivers are initialized) */
        intr_enable();

        /* Run initproc */
        sched_make_runnable(initthr);
        /* Now wait for it */
        child = do_waitpid(-1, 0, &status);
        KASSERT(PID_INIT == child);

#ifdef __MTP__
        kthread_reapd_shutdown();
#endif


        /* PROCS BLANK {{{ */
#ifdef __SHADOWD__
        /* TODO port this - alvin */
#endif
        /* PROCS BLANK }}} */
#ifdef __VFS__
        /* Shutdown the vfs: */
        dbg_print("weenix: vfs shutdown...\n");
        vput(curproc->p_cwd);
        if (vfs_shutdown())
                panic("vfs shutdown FAILED!!\n");

#endif

        /* Shutdown the pframe system */
#ifdef __S5FS__
        pframe_shutdown();
#endif

        dbg_print("\nweenix: halted cleanly!\n");
        GDB_CALL_HOOK(shutdown);
        hard_shutdown();
        return NULL;
}

/**
 * This function, called by the idle process (within 'idleproc_run'), creates the
 * process commonly refered to as the "init" process, which should have PID 1.
 *
 * The init process should contain a thread which begins execution in
 * initproc_run().
 *
 * @return a pointer to a newly created thread which will execute
 * initproc_run when it begins executing
 */
static kthread_t *
initproc_create(void)
{
        /* PROCS {{{ */
        dbg(DBG_INIT, "Creating init proc\n");

        proc_t *p = proc_create("init");
        KASSERT(NULL != p);
        KASSERT(PID_INIT == p->p_pid);

        kthread_t *thr = kthread_create(p, initproc_run, 0, NULL);
        KASSERT(NULL != thr);
        return thr;
        /* PROCS }}} */
        return NULL;
}

/**
 * The init thread's function changes depending on how far along your Weenix is
 * developed. Before VM/FI, you'll probably just want to have this run whatever
 * tests you've written (possibly in a new process). After VM/FI, you'll just
 * exec "/bin/init".
 *
 * Both arguments are unused.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *
initproc_run(int arg1, void *arg2)
{
        /* PROCS {{{ */
#ifdef __VM__
        int status;

        dbg(DBG_INIT, "do_init!\n");

        do_open("/dev/tty0", O_RDONLY);
        do_open("/dev/tty0", O_WRONLY);
        do_open("/dev/tty0", O_WRONLY);

        char *const argvec[] = { "foo", NULL };
        char *const envvec[] = { "bar", NULL };
        kernel_execve("/sbin/init", argvec, envvec);

        while (!do_waitpid(-1, 0, &status));
        do_exit(0);
#endif

#ifdef __DRIVERS__

        /* If we do not have VM yet, run the kernel shell */
        kshell_t *kshell;

        /* Create kernel shell on TTY 0 */
        kshell = kshell_create(0);
        if (NULL == kshell)
                panic("init: Couldn't create kernel shell\n");

        /*dbg_print("Going to test kshell ls\n");
        kshell_test(kshell,"ls\n");
        dbg_print("Going to test kshell cat README\n");
        kshell_test(kshell,"cat hamlet\n");
        dbg_print("Going to test stat README\n");
        kshell_test(kshell, "stat README\n");
        dbg_print("Going to test stat hamlet\n");
        kshell_test(kshell, "stat hamlet\n");*/
        /*kshell_test(kshell, "stat hamlet\n"); */
        /*dbg_print("Going to test 'thread_test 2'\n");
        kshell_test(kshell, "thread_test 2\n");
        kshell_test(kshell, "ls dir000\n");
        kshell_test(kshell, "ls dir001\n");*/
        /*dbg_print("Going to test 'directory_test 2'\n");
        kshell_test(kshell, "directory_test 2\n");
        kshell_test(kshell, "ls dir000\n");
        kshell_test(kshell, "ls dir001\n");*/
        /*dbg_print("Going to test 'thread_test 5'\n");
        kshell_test(kshell, "thread_test 5\n");
        kshell_test(kshell, "ls dir000\n");
        kshell_test(kshell, "cat hamlet > dir000/test000\n");
        kshell_test(kshell, "echo test > newfile\n");*/
        dbg_print("Going to test 'space_test'\n");
        kshell_test(kshell, "space_test\n");
        /*kshell_test(kshell, "ls\n");*/
        kshell_test(kshell, "echo data > newfile\n");
        /*kshell_test(kshell, "cat newfile\n");
        kshell_test(kshell, "stat newfile\n");
        kshell_test(kshell, "ls\n");*/
        /*kshell_test(kshell, "stat space\n");*/
        /*kshell_test(kshell, "cat space\n");*/
        kshell_test(kshell, "rm space\n");
        /*kshell_test(kshell, "echo data > newfile\n");
        kshell_test(kshell, "cat newfile\n");
        kshell_test(kshell, "stat newfile\n");*/
        kshell_test(kshell, "echo data > newfile2\n");
        kshell_test(kshell, "cat newfile2\n");
        kshell_test(kshell, "stat newfile2\n");
        /*dbg_print("Going to test 'rm test'\n");
        kshell_test(kshell, "rm test\n");
        kshell_test(kshell, "ls\n");
        dbg_print("Going to test 'mkdir newdir'\n");
        kshell_test(kshell, "mkdir newdir\n");
        kshell_test(kshell, "ls\n");
        kshell_test(kshell, "ls newdir\n");
        dbg_print("Going to test 'rmdir newdir'\n");
        kshell_test(kshell, "rmdir newdir\n");
        kshell_test(kshell, "ls\n");*/
        /*dbg_print("Going to test 'echo test > file'\n");
        kshell_test(kshell, "echo test > file\n");*/
        /*kshell_test(kshell, "ls\n");
        kshell_test(kshell, "cat file\n");*/
        /*dbg_print("Going to test 'rm short_file'\n");
        kshell_test(kshell, "rm short_file\n");
        kshell_test(kshell, "ls\n");
        dbg_print("Going to test 'rm test'\n");
        kshell_test(kshell, "rm test\n");*/
        /*dbg_print("Going to test 'link file file2'\n");
        kshell_test(kshell, "link file file2\n");*/
        /*kshell_test(kshell, "ls\n");
        kshell_test(kshell, "cat file2\n");*/
        /*dbg_print("Going to test 'cat file README > file3'\n");
        kshell_test(kshell, "cat file README > file3\n");*/
        /*kshell_test(kshell, "ls\n");*/
        /*kshell_test(kshell, "cat file3 README > file2\n");*/
        /*kshell_test(kshell, "ls\n");*/
        /*kshell_test(kshell, "cat file3\n");
        kshell_test(kshell, "cat file2\n");
        kshell_test(kshell, "cat file\n");*/
        /*while (kshell_execute_next(kshell));*/
        kshell_destroy(kshell);
#endif
        /* PROCS }}} */

        return NULL;
}

/**
 * Clears all interrupts and halts, meaning that we will never run
 * again.
 */
static void
hard_shutdown()
{
#ifdef __DRIVERS__
        vt_print_shutdown();
#endif
        __asm__ volatile("cli; hlt");
}
