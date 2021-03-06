/*********************************************************************************
 *
 *       Copyright (C) 2015 Ichiro Kawazome
 *       All rights reserved.
 * 
 *       Redistribution and use in source and binary forms, with or without
 *       modification, are permitted provided that the following conditions
 *       are met:
 * 
 *         1. Redistributions of source code must retain the above copyright
 *            notice, this list of conditions and the following disclaimer.
 * 
 *         2. Redistributions in binary form must reproduce the above copyright
 *            notice, this list of conditions and the following disclaimer in
 *            the documentation and/or other materials provided with the
 *            distribution.
 * 
 *       THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *       "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *       LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *       A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 *       OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *       SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *       LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *       DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *       THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 *       (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *       OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 ********************************************************************************/
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/pagemap.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <asm/page.h>
#include <asm/byteorder.h>

#define VERSION_LATER_3_13

#define DRIVER_NAME        "udmabuf"
#define DEVICE_NAME_FORMAT "udmabuf%d"
#define UDMABUF_DEBUG       1
#define SYNC_ENABLE         1

#if     (UDMABUF_DEBUG == 1)
#define UDMABUF_DEBUG_CHECK(this,debug) (this->debug)
#else
#define UDMABUF_DEBUG_CHECK(this,debug) (0)
#endif

static struct class*  udmabuf_sys_class     = NULL;
static dev_t          udmabuf_device_number = 0;

/**
 * struct udmabuf_driver_data - Device driver structure
 */
struct udmabuf_driver_data {
    struct device*       device;
    struct cdev          cdev;
    dev_t                device_number;
    struct mutex         sem;
    bool                 is_open;
    int                  size;
    size_t               alloc_size;
    void*                virt_addr;
    dma_addr_t           phys_addr;
#if (SYNC_ENABLE == 1)
    int                  sync_mode;
#endif
#if ((UDMABUF_DEBUG == 1) && (SYNC_ENABLE == 1))
    bool                 debug_vma;
#endif   
};

/**
 *
 */
#define SYNC_INVALID       (0)
#define SYNC_NONCACHED     (1)
#define SYNC_WRITECOMBINE  (2)
#define SYNC_DMACOHERENT   (3)

#define DEF_ATTR_SHOW(__attr_name, __format, __value) \
static ssize_t udmabuf_show_ ## __attr_name(struct device *dev, struct device_attribute *attr, char *buf) \
{                                                            \
    ssize_t status;                                          \
    struct udmabuf_driver_data* this = dev_get_drvdata(dev); \
    if (mutex_lock_interruptible(&this->sem) != 0)           \
        return -ERESTARTSYS;                                 \
    status = sprintf(buf, __format, (__value));              \
    mutex_unlock(&this->sem);                                \
    return status;                                           \
}

#ifdef VERSION_LATER_3_13
#define STRTOUL(__buf, __size, __value) kstrtoul(__buf, __size, __value)
#else
#define STRTOUL(__buf, __size, __value) strict_strtoul(__buf, __size, __value)
#endif

#define DEF_ATTR_SET(__attr_name, __min, __max, __pre_action, __post_action) \
static ssize_t udmabuf_set_ ## __attr_name(struct device *dev, struct device_attribute *attr, const char *buf, size_t size) \
{ \
    ssize_t       status; \
    unsigned long value;  \
    struct udmabuf_driver_data* this = dev_get_drvdata(dev);                 \
    if (0 != mutex_lock_interruptible(&this->sem)){return -ERESTARTSYS;}     \
    if (0 != (status = STRTOUL(buf, 10, &value))) {     goto failed;} \
    if ((value < __min) || (__max < value)) {status = -EINVAL; goto failed;} \
    if (0 != (status = __pre_action )) {                       goto failed;} \
    this->__attr_name = value;                                               \
    if (0 != (status = __post_action)) {                       goto failed;} \
    status = size;                                                           \
  failed:                                                                    \
    mutex_unlock(&this->sem);                                                \
    return status;                                                           \
}

DEF_ATTR_SHOW(size      , "%d\n"   , this->size      );
DEF_ATTR_SHOW(phys_addr , "0x%lx\n", (long unsigned int)this->phys_addr );
#if (SYNC_ENABLE == 1)
DEF_ATTR_SHOW(sync_mode , "%d\n"   , this->sync_mode );
DEF_ATTR_SET( sync_mode            , 0, 3, 0, 0      );
#endif
#if ((UDMABUF_DEBUG == 1) && (SYNC_ENABLE == 1))
DEF_ATTR_SHOW(debug_vma , "%d\n"   , this->debug_vma );
DEF_ATTR_SET( debug_vma            , 0, 1, 0, 0      );
#endif

#ifdef VERSION_LATER_3_13
static struct device_attribute udmabuf_device_attrs[] = {
#else
static const struct device_attribute udmabuf_device_attrs[] = {
#endif
  __ATTR(size      , 0644, udmabuf_show_size      , NULL),
  __ATTR(phys_addr , 0644, udmabuf_show_phys_addr , NULL),
#if (SYNC_ENABLE == 1)
  __ATTR(sync_mode , 0644, udmabuf_show_sync_mode , udmabuf_set_sync_mode),
#endif
#if ((UDMABUF_DEBUG == 1) && (SYNC_ENABLE == 1))
  __ATTR(debug_vma , 0644, udmabuf_show_debug_vma , udmabuf_set_debug_vma),
#endif
  __ATTR_NULL,
};

#ifdef VERSION_LATER_3_13
static struct attribute *udmabuf_attrs[] = {
  &(udmabuf_device_attrs[0].attr),
  &(udmabuf_device_attrs[1].attr),
#if (SYNC_ENABLE == 1)
  &(udmabuf_device_attrs[2].attr),
#endif
#if ((UDMABUF_DEBUG == 1) && (SYNC_ENABLE == 1))
  &(udmabuf_device_attrs[3].attr),
#endif
  NULL,
};

static struct attribute_group udmabuf_attr_group = {
  .attrs = udmabuf_attrs,
};

static const struct attribute_group *udmabuf_attr_groups[] = {
  &udmabuf_attr_group,
  NULL
};
#endif

#if (SYNC_ENABLE == 1)
/**
 * udmabuf_driver_vma_open() - The is the driver open function.
 * @vma:        Pointer to the vm area structure.
 * returns:	Success or error status.
 */
static void udmabuf_driver_vma_open(struct vm_area_struct* vma)
{
    struct udmabuf_driver_data* this = vma->vm_private_data;
    if (UDMABUF_DEBUG_CHECK(this, debug_vma))
        dev_info(this->device, "vma_open(virt_addr=0x%lx, offset=0x%lx)\n", vma->vm_start, vma->vm_pgoff<<PAGE_SHIFT);
}
#endif

#if (SYNC_ENABLE == 1)
/**
 * udmabuf_driver_vma_close() - The is the driver close function.
 * @vma:        Pointer to the vm area structure.
 * returns:	Success or error status.
 */
static void udmabuf_driver_vma_close(struct vm_area_struct* vma)
{
    struct udmabuf_driver_data* this = vma->vm_private_data;
    if (UDMABUF_DEBUG_CHECK(this, debug_vma))
        dev_info(this->device, "vma_close()\n");
}
#endif

#if (SYNC_ENABLE == 1)
/**
 * udmabuf_driver_vma_fault() - The is the driver nopage function.
 * @vma:        Pointer to the vm area structure.
 * @vfm:        Pointer to the vm fault structure.
 * returns:	Success or error status.
 */
static int udmabuf_driver_vma_fault(struct vm_area_struct* vma, struct vm_fault* vmf)
{
    struct udmabuf_driver_data* this = vma->vm_private_data;
    struct page*  page_ptr           = NULL;
    unsigned long offset             = vmf->pgoff << PAGE_SHIFT;
    unsigned long phys_addr          = this->phys_addr + offset;
    unsigned long page_frame_num     = phys_addr  >> PAGE_SHIFT;
    unsigned long request_size       = 1          << PAGE_SHIFT;
    unsigned long available_size     = this->alloc_size -offset;

    if (UDMABUF_DEBUG_CHECK(this, debug_vma))
        dev_info(this->device, "vma_fault(virt_addr=0x%lx, phys_addr=0x%lx)\n", (long unsigned int)vmf->virtual_address, phys_addr);

    if (request_size > available_size) 
        return VM_FAULT_SIGBUS;

    if (!pfn_valid(page_frame_num))
        return VM_FAULT_SIGBUS;

    page_ptr = pfn_to_page(page_frame_num);
    get_page(page_ptr);
    vmf->page = page_ptr;
    return 0;
}
#endif

#if (SYNC_ENABLE == 1)
/**
 *
 */
static const struct vm_operations_struct udmabuf_driver_vm_ops = {
    .open    = udmabuf_driver_vma_open ,
    .close   = udmabuf_driver_vma_close,
    .fault   = udmabuf_driver_vma_fault,
};
#endif

/**
 * udmabuf_driver_file_open() - The is the driver open function.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * returns:	Success or error status.
 */
static int udmabuf_driver_file_open(struct inode *inode, struct file *file)
{
    struct udmabuf_driver_data* this;
    int status = 0;

    this = container_of(inode->i_cdev, struct udmabuf_driver_data, cdev);
    file->private_data = this;
    this->is_open = 1;

    return status;
}

/**
 * udmabuf_driver_file_release() - The is the driver release function.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * returns:	Success.
 */
static int udmabuf_driver_file_release(struct inode *inode, struct file *file)
{
    struct udmabuf_driver_data* this = file->private_data;

    this->is_open = 0;

    return 0;
}

/**
 * udmabuf_driver_file_mmap() - The is the driver memory map function.
 * @file:	Pointer to the file structure.
 * @vma:        Pointer to the vm area structure.
 * returns:	Success.
 */
static int udmabuf_driver_file_mmap(struct file *file, struct vm_area_struct* vma)
{
    struct udmabuf_driver_data* this = file->private_data;

#if (SYNC_ENABLE == 1)
    vma->vm_ops           = &udmabuf_driver_vm_ops;
    vma->vm_private_data  = this;
 /* vma->vm_flags        |= VM_RESERVED; */
    if (file->f_flags & O_SYNC) {
        switch (this->sync_mode) {
            case SYNC_NONCACHED : 
                vma->vm_flags    |= VM_IO;
                vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
                break;
            case SYNC_WRITECOMBINE : 
                vma->vm_flags    |= VM_IO;
                vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
                break;
            case SYNC_DMACOHERENT : 
                vma->vm_flags    |= VM_IO;
                vma->vm_page_prot = pgprot_dmacoherent(vma->vm_page_prot);
                break;
            default :
                break;
        }
    }
    udmabuf_driver_vma_open(vma);
    return 0;
#else
    return dma_mmap_coherent(this->device, vma, this->virt_addr, this->phys_addr, this->alloc_size);
#endif
}

/**
 *
 */
static const struct file_operations udmabuf_driver_file_ops = {
    .owner   = THIS_MODULE,
    .open    = udmabuf_driver_file_open,
    .release = udmabuf_driver_file_release,
    .mmap    = udmabuf_driver_file_mmap,
};

/**
 * udmabuf_device_minor_number_bitmap
 */
static u32  udmabuf_device_minor_number_bitmap = 0;
static int  udmabuf_device_minor_number_check(int num)
{
    if (num >= 32) {
        return 0;
    } else {
        u32 mask = (1 << num);
        if (udmabuf_device_minor_number_bitmap & mask)
            return 0;
        else 
            return 1;
    }
}
static int  udmabuf_device_minor_number_allocate(int num)
{
    if (udmabuf_device_minor_number_check(num) == 0) {
        return -1;
    } else {
        u32 mask = (1 << num);
        udmabuf_device_minor_number_bitmap |= mask;
        return 0;
    }
}
static void udmabuf_device_minor_number_free(int num)
{
    u32 mask = (1 << num);
    udmabuf_device_minor_number_bitmap &= ~mask;
}

/**
 * udmabuf_driver_create() -  Create call for the device.
 *
 * @minor:	minor_number
 * @size:	buffer size
 * Returns device driver strcutre pointer
 *
 * It does all the memory allocation and registration for the device.
 */
static struct udmabuf_driver_data* udmabuf_driver_create(int minor, unsigned int size)
{
    struct udmabuf_driver_data* this     = NULL;
    unsigned int                done     = 0;
    const unsigned int          DONE_CHRDEV_ADD    = (1 << 0);
    const unsigned int          DONE_ALLOC_CMA     = (1 << 1);
    const unsigned int          DONE_DEVICE_CREATE = (1 << 2);
    /*
     * alloc device_minor_number
     */
    if (udmabuf_device_minor_number_allocate(minor) == -1) {
        goto failed;
    }
    /*
     * create (udmabuf_driver_data*) this.
     */
    {
        this = kzalloc(sizeof(*this), GFP_KERNEL);
        if (IS_ERR_OR_NULL(this)) {
            goto failed;
        }
    }
    /*
     * make this->device_number and this->size
     */
    {
        this->device_number = MKDEV(MAJOR(udmabuf_device_number), minor);
        this->size          = size;
        this->alloc_size    = ((size + ((1 << PAGE_SHIFT) - 1)) >> PAGE_SHIFT) << PAGE_SHIFT;
    }
#if (SYNC_ENABLE == 1)
    {
        this->sync_mode     = SYNC_NONCACHED;
    }
#endif
#if ((UDMABUF_DEBUG == 1) && (SYNC_ENABLE == 1))
    {
        this->debug_vma     = 0;
    }
#endif
    /*
     * register /sys/class/udmabuf/udmabuf[0..n]
     */
    {
        this->device = device_create(udmabuf_sys_class,
                                     NULL,
                                     this->device_number,
                                     (void *)this,
                                     DEVICE_NAME_FORMAT, MINOR(this->device_number));
        if (IS_ERR_OR_NULL(this->device)) {
            this->device = NULL;
            goto failed;
        }
        dma_set_coherent_mask(this->device, 0xFFFFFFFF);
        done |= DONE_DEVICE_CREATE;
    }
    /*
     * dma buffer allocation 
     */
    {
        this->virt_addr = dma_alloc_coherent(this->device, this->alloc_size, &this->phys_addr, GFP_KERNEL);
        if (IS_ERR_OR_NULL(this->virt_addr)) {
            dev_err(this->device, "dma_alloc_coherent() failed\n");
            this->virt_addr = NULL;
            goto failed;
        }
        done |= DONE_ALLOC_CMA;
    }
    /*
     * add chrdev.
     */
    {
        cdev_init(&this->cdev, &udmabuf_driver_file_ops);
        this->cdev.owner = THIS_MODULE;
        if (cdev_add(&this->cdev, this->device_number, 1) != 0) {
            dev_err(this->device, "cdev_add() failed\n");
            goto failed;
        }
        done |= DONE_CHRDEV_ADD;
    }
    /*
     *
     */
    mutex_init(&this->sem);
    /*
     *
     */
    dev_info(this->device, "driver installed\n");
    dev_info(this->device, "major number   = %d\n"    , MAJOR(this->device_number));
    dev_info(this->device, "minor number   = %d\n"    , MINOR(this->device_number));
    dev_info(this->device, "phys address   = 0x%lx\n" , (long unsigned int)this->phys_addr);
    dev_info(this->device, "buffer size    = %d\n"    , this->alloc_size);

    return this;

 failed:
    if (done & DONE_DEVICE_CREATE) { device_destroy(udmabuf_sys_class, this->device_number);}
    if (done & DONE_ALLOC_CMA    ) { dma_free_coherent(this->device, this->alloc_size, this->virt_addr, this->phys_addr);}
    if (done & DONE_CHRDEV_ADD   ) { cdev_del(&this->cdev); }
    if (this != NULL)              { kfree(this); }
    return NULL;
}

/**
 * udmabuf_driver_destroy() -  Remove call for the device.
 *
 * @this: udmabuf driver data strcutre pointer
 * Returns 0 or error status.
 *
 * Unregister the device after releasing the resources.
 */
static int udmabuf_driver_destroy(struct udmabuf_driver_data* this)
{
    if (!this)
        return -ENODEV;

    udmabuf_device_minor_number_free(MINOR(this->device_number));
    dev_info(this->device, "driver uninstalled\n");
    dma_free_coherent(this->device, this->alloc_size, this->virt_addr, this->phys_addr);
    device_destroy(udmabuf_sys_class, this->device_number);
    cdev_del(&this->cdev);
    kfree(this);
    return 0;
}

/**
 * udmabuf_platform_driver_probe() -  Probe call for the device.
 *
 * @pdev:	handle to the platform device structure.
 * Returns 0 on success, negative error otherwise.
 *
 * It does all the memory allocation and registration for the device.
 */
static int udmabuf_platform_driver_probe(struct platform_device *pdev)
{
    int          retval = 0;
    unsigned int size   = 0;
    unsigned int minor_number = 0;

    dev_info(&pdev->dev, "driver probe start.\n");
    /*
     * get buffer size
     */
    {
        int status;
        status = of_property_read_u32(pdev->dev.of_node, "size", &size);
        if (status != 0) {
            dev_err(&pdev->dev, "invalid property size.\n");
            retval = -ENODEV;
            goto failed;
        }
    }
    /*
     * get device number
     */
    {
        int status;
        status = of_property_read_u32(pdev->dev.of_node, "minor-number", &minor_number);
        if (status != 0) {
            dev_err(&pdev->dev, "invalid property minor number.\n");
            retval = -ENODEV;
            goto failed;
        }
        if (udmabuf_device_minor_number_check(minor_number) == 0) {
            dev_err(&pdev->dev, "invalid or conflict minor number %d.\n", minor_number);
            retval = -ENODEV;
            goto failed;
        }
    }
    /*
     * create (udmabuf_driver_data*)this.
     */
    {
        struct udmabuf_driver_data* driver_data = udmabuf_driver_create(minor_number, size);
        if (IS_ERR_OR_NULL(driver_data)) {
            dev_err(&pdev->dev, "driver create fail.\n");
            retval = PTR_ERR(driver_data);
            goto failed;
        }
        dev_set_drvdata(&pdev->dev, driver_data);
    }
    dev_info(&pdev->dev, "driver installed.\n");
    return 0;
 failed:
    dev_info(&pdev->dev, "driver install failed.\n");
    return retval;
}

/**
 * udmabuf_platform_driver_remove() -  Remove call for the device.
 *
 * @pdev:	handle to the platform device structure.
 * Returns 0 or error status.
 *
 * Unregister the device after releasing the resources.
 */
static int udmabuf_platform_driver_remove(struct platform_device *pdev)
{
    struct udmabuf_driver_data* this   = dev_get_drvdata(&pdev->dev);
    int                        retval = 0;

    if ((retval = udmabuf_driver_destroy(this)) != 0)
        return retval;
    dev_set_drvdata(&pdev->dev, NULL);
    dev_info(&pdev->dev, "driver unloaded\n");
    return 0;
}

/**
 * Open Firmware Device Identifier Matching Table
 */
static struct of_device_id udmabuf_of_match[] = {
    { .compatible = "ikwzm,udmabuf-0.10.a", },
    { /* end of table */}
};
MODULE_DEVICE_TABLE(of, udmabuf_of_match);

/**
 * Platform Driver Structure
 */
static struct platform_driver udmabuf_platform_driver = {
    .probe  = udmabuf_platform_driver_probe,
    .remove = udmabuf_platform_driver_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name  = DRIVER_NAME,
        .of_match_table = udmabuf_of_match,
    },
};
static bool udmabuf_platform_driver_done = 0;

/**
 * 
 */
static int        udmabuf0 = 0;
static int        udmabuf1 = 0;
static int        udmabuf2 = 0;
static int        udmabuf3 = 0;
module_param(     udmabuf0, int, S_IRUGO);
module_param(     udmabuf1, int, S_IRUGO);
module_param(     udmabuf2, int, S_IRUGO);
module_param(     udmabuf3, int, S_IRUGO);
MODULE_PARM_DESC( udmabuf0, "udmabuf0 buffer size");
MODULE_PARM_DESC( udmabuf1, "udmabuf1 buffer size");
MODULE_PARM_DESC( udmabuf2, "udmabuf2 buffer size");
MODULE_PARM_DESC( udmabuf3, "udmabuf3 buffer size");

struct udmabuf_driver_data* udmabuf_driver[4] = {NULL,NULL,NULL,NULL};

#define CREATE_UDMABUF_DRIVER(__num)                                                      \
    if (udmabuf ## __num > 0) {                                                           \
        udmabuf_driver[__num] = udmabuf_driver_create(__num, udmabuf ## __num);           \
        if (IS_ERR_OR_NULL(udmabuf_driver[__num])) {                                      \
            udmabuf_driver[__num] = NULL;                                                 \
            printk(KERN_ERR "%s: couldn't create udmabuf%d driver\n", DRIVER_NAME, __num);\
        }                                                                                 \
    }

/**
 * udmabuf_module_exit()
 */
static void __exit udmabuf_module_exit(void)
{
    if (udmabuf_driver[3]     != NULL){udmabuf_driver_destroy(udmabuf_driver[3]);}
    if (udmabuf_driver[2]     != NULL){udmabuf_driver_destroy(udmabuf_driver[2]);}
    if (udmabuf_driver[1]     != NULL){udmabuf_driver_destroy(udmabuf_driver[1]);}
    if (udmabuf_driver[0]     != NULL){udmabuf_driver_destroy(udmabuf_driver[0]);}
    if (udmabuf_platform_driver_done ){platform_driver_unregister(&udmabuf_platform_driver);}
    if (udmabuf_sys_class     != NULL){class_destroy(udmabuf_sys_class);}
    if (udmabuf_device_number != 0   ){unregister_chrdev_region(udmabuf_device_number, 0);}
}

/**
 * udmabuf_module_init()
 */
static int __init udmabuf_module_init(void)
{
    int retval = 0;

    retval = alloc_chrdev_region(&udmabuf_device_number, 0, 0, DRIVER_NAME);
    if (retval != 0) {
        printk(KERN_ERR "%s: couldn't allocate device major number\n", DRIVER_NAME);
        udmabuf_device_number = 0;
        goto failed;
    }

    udmabuf_sys_class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR_OR_NULL(udmabuf_sys_class)) {
        printk(KERN_ERR "%s: couldn't create sys class\n", DRIVER_NAME);
        retval = PTR_ERR(udmabuf_sys_class);
        udmabuf_sys_class = NULL;
        goto failed;
    }
#ifdef VERSION_LATER_3_13
    udmabuf_sys_class->dev_groups = udmabuf_attr_groups;
#else
    udmabuf_sys_class->dev_attrs = udmabuf_device_attrs;
#endif

    CREATE_UDMABUF_DRIVER(0);
    CREATE_UDMABUF_DRIVER(1);
    CREATE_UDMABUF_DRIVER(2);
    CREATE_UDMABUF_DRIVER(3);

    retval = platform_driver_register(&udmabuf_platform_driver);
    if (retval) {
        printk(KERN_ERR "%s: couldn't register platform driver\n", DRIVER_NAME);
    } else {
        udmabuf_platform_driver_done = 1;
    }
    return 0;

 failed:
    udmabuf_module_exit();
    return retval;
}

module_init(udmabuf_module_init);
module_exit(udmabuf_module_exit);

MODULE_AUTHOR("ikwzm");
MODULE_DESCRIPTION("User space mappable DMA buffer device driver");
MODULE_LICENSE("GPL");
