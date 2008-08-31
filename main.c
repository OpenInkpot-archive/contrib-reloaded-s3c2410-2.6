#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/syscalls.h>

#include <asm/cacheflush.h>
#include <asm/uaccess.h>
#include <asm/setup.h>

extern void setup_mm_for_reboot(char mode);


/* module parameters */

static char kernel[256] = "zImage";
module_param_string(kernel, kernel, sizeof(kernel), 0);
MODULE_PARM_DESC(kernel, "Kernel image file");

static char initrd[256] = "initrd.gz";
module_param_string(initrd, initrd, sizeof(initrd), 0);
MODULE_PARM_DESC(initrd, "initrd file");

static char cmdline[1024] = "root=/dev/ram console=ttyS0,115200 :::DB88FXX81:egiga0:none";
module_param_string(cmdline, cmdline, sizeof(cmdline), 0);
MODULE_PARM_DESC(cmdline, "Kernel command line");

// 193    = smdk2410 / lbook_v3
// 526    = MV88fxx81 in d-link firmware 1.02/1.03
// 275    = qemu integrator (cintegrator)
// 387    = qemu versatile pb
// 859    = marvell MV88FXX81 board in recent kernels

static int machtype = 193;
module_param(machtype, int, 0);
MODULE_PARM_DESC(machtype, "Machine type (see linux/arch/arm/tools/mach-types)");



/* reboot.S */
/* current array sizes:
   200 for kernel
   280 for initrd

   mean segment size is 32k,
   two longs needed per segment

   kernel < 200/2 * 32k = 3.2M
   initrd < 280/2 * 32k = 4.3M
*/
   
extern unsigned long dns323_kernel_segments[];
extern unsigned long dns323_initrd_segments[];
extern unsigned long dns323_taglist;
extern unsigned long dns323_machtype;

extern void dns323_reboot(void);
extern unsigned char dns323_do_reboot[];
extern unsigned long dns323_reboot_size;
extern unsigned long dns323_reboot_start;


/* reserve low 16MB */
#define MIN_ADDR 0x31000000lu

/* */
#define SEGMENT_SIZE (32*PAGE_SIZE)
#define MIN_SEGMENT_SIZE PAGE_SIZE
static unsigned int segment_size = SEGMENT_SIZE;

static void load_file(char *filename, int count, unsigned long *segments)
{
        mm_segment_t old_fs = get_fs();
        int fd, i;

        void *ptr;
        unsigned long c = 0;

        int segc = 0;

        set_fs(KERNEL_DS);
        if ((fd = sys_open(filename, O_RDONLY, 0)) < 0) 
                panic("%s: open failed (%d)\n", filename, fd);
        while (c < count) {

        retry:
                while (!(ptr = kmalloc(segment_size, GFP_KERNEL))) {
                        if (segment_size == MIN_SEGMENT_SIZE)
                                panic("load_file: Out of memory\n");
                        segment_size >>= 1;
                }
                if (virt_to_phys(ptr) < MIN_ADDR) {
                        printk("not using %d bytes reserved memory at %p / %p\n", segment_size, ptr, __virt_to_phys(ptr));
                        goto retry;
                }

                i = sys_read(fd, ptr, segment_size);
                printk("loaded %d of %d bytes at %p / %p\n", i, count, ptr, __virt_to_phys(ptr));
                if (i > 0) {
                        *segments++ = virt_to_phys(ptr);
                        *segments++ = (unsigned long)i;
                        c += i;
                        segc++;
                }
                if (i < 0)
                        panic("load_file: read error\n");
        }
        *segments = 0;
        sys_close(fd);
        printk("%s: OK (%d segments)\n", filename, segc);
        set_fs(old_fs);
}


static void *make_taglist(int initrd_size) 
{
        void *taglist;
        struct tag *t;
        int i;
        unsigned long mem_start = 0x30000000;
        unsigned long mem_size = 0x02000000;//(unsigned long)__virt_to_phys(high_memory);

        if (!(t = taglist = kmalloc(MIN_SEGMENT_SIZE, GFP_KERNEL)))
                panic("make_taglist: Out of memory\n");

        t->hdr.tag = ATAG_CORE;
        t->hdr.size = tag_size(tag_core);
        t->u.core.flags = 1;
        t->u.core.pagesize = PAGE_SIZE;
        t->u.core.rootdev = 0x0; //0x00010000lu;

        printk("CMDLINE: %s\n", cmdline);
        t = tag_next(t);
        i = strlen(cmdline)+1;
        t->hdr.size = (sizeof(struct tag_header) + i+3) >> 2;
        t->hdr.tag = ATAG_CMDLINE;
        strcpy(t->u.cmdline.cmdline, cmdline);

        printk("MEM: start %08lx size %luMB\n", mem_start, mem_size >> 20);
        t = tag_next(t);
        t->hdr.size = tag_size(tag_mem32);
        t->hdr.tag = ATAG_MEM;
        t->u.mem.size = mem_size;
        t->u.mem.start = mem_start;

        if (initrd_size > 0) {
                printk("INITRD: start %08lx size %d\n", 0x30800000lu, initrd_size);
                t = tag_next(t);
                t->hdr.size = tag_size(tag_initrd);
                t->hdr.tag = ATAG_INITRD2;
                t->u.initrd.start = 0x30800000;
                t->u.initrd.size = initrd_size;
        }

        t = tag_next(t);
        t->hdr.size = 0;
        t->hdr.tag = ATAG_NONE;

        return taglist;
}

/* main function */
static int __init reloaded_init(void)
{
        mm_segment_t old_fs;
        int err = 0;
        struct kstat kernel_st;
        struct kstat initrd_st;
        void *dns323_reboot_code;

        printk("reloaded for DNS-323, 2007 tp@fonz.de\n");

        /* load kernel and initrd */
        old_fs = get_fs();
        set_fs(KERNEL_DS);
        if ((err = vfs_stat(kernel, &kernel_st)) < 0) 
                printk("%s: stat failed (%d)\n", kernel, err);
        else if (*initrd && (err = vfs_stat(initrd, &initrd_st)) < 0)
                printk("%s: stat failed (%d)\n", initrd, err);
        set_fs(old_fs);
        if (err < 0)
                return -1;

        printk("%s: %lld bytes\n", kernel, kernel_st.size);
        load_file(kernel, kernel_st.size, dns323_kernel_segments);
        if (*initrd) {
                printk("%s: %lld bytes\n", initrd, initrd_st.size);
                load_file(initrd, initrd_st.size, dns323_initrd_segments);
        }

        /* */
        dns323_machtype = machtype;
        printk("dns323_machtype = %ld\n", dns323_machtype);
        dns323_taglist = virt_to_phys(make_taglist(*initrd ? initrd_st.size : 0));
        printk("dns323_taglist  = %lx (%p)\n", dns323_taglist, phys_to_virt(dns323_taglist));
        if (!(dns323_reboot_code = kmalloc(dns323_reboot_size, GFP_KERNEL))) 
                panic("reboot code: Out of memory\n");
        printk("copying %lu bytes reboot code from %p to %p\n",
               dns323_reboot_size, dns323_do_reboot, dns323_reboot_code);
        memcpy(dns323_reboot_code, dns323_do_reboot, dns323_reboot_size);
        dns323_reboot_start = virt_to_phys(dns323_reboot_code);
        printk("dns323_reboot_start  = %lx\n", dns323_reboot_start);

        /* go */
        printk(KERN_INFO "Reloading...\n");
        flush_icache_range((unsigned long)dns323_reboot_code, (unsigned long)dns323_reboot_code + dns323_reboot_size);
        cpu_proc_fin();
        setup_mm_for_reboot(0);
        dns323_reboot();

        panic("FAILED\n");
        return 0;
}

static void __exit reloaded_cleanup(void)
{
        printk("reloaded: bye\n");
}

module_init(reloaded_init);
module_exit(reloaded_cleanup);
MODULE_LICENSE("GPL");
