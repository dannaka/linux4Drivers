/*
 * Simple - REALLY Simple memory mapping demonstration.
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: Simple.c,v 1.12 2005/01/31 16:15:31 rubini Exp $
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>   /* printk() */
#include <linux/slab.h>   /* kmalloc() */
#include <linux/fs.h>       /* everything... */
#include <linux/errno.h>    /* error codes */
#include <linux/types.h>    /* size_t */
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <asm/page.h>
#include <linux/cdev.h>

#include <linux/device.h>

static int Simple_major = 0;
module_param(Simple_major, int, 0);
MODULE_AUTHOR("Dan Nakahara");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * Open the device; in fact, there's nothing to do here.
 */
static int Simple_open (struct inode *inode, struct file *filp)
{
	return 0;
}


/*
 * Closing is just as Simpler.
 */
static int Simple_release(struct inode *inode, struct file *filp)
{
	return 0;
}



/*
 * Common VMA ops.
 */

void Simple_vma_open(struct vm_area_struct *vma)
{
	printk(KERN_NOTICE "Simple VMA open, virt %lx, phys %lx\n",
			vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

void Simple_vma_close(struct vm_area_struct *vma)
{
	printk(KERN_NOTICE "Simple VMA close.\n");
}


/*
 * The remap_pfn_range version of mmap.  This one is heavily borrowed
 * from drivers/char/mem.c.
 */

static struct vm_operations_struct Simple_remap_vm_ops = {
	.open =  Simple_vma_open,
	.close = Simple_vma_close,
};

static int Simple_remap_mmap(struct file *filp, struct vm_area_struct *vma)
{
	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			    vma->vm_end - vma->vm_start,
			    vma->vm_page_prot))
		return -EAGAIN;

	vma->vm_ops = &Simple_remap_vm_ops;
	Simple_vma_open(vma);
	return 0;
}



/*
 * The fault version.
 */
static int Simple_vma_fault(struct vm_area_struct *vma,
        struct vm_fault *vmf)
{
    struct page *pageptr;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    unsigned long address = (unsigned long) vmf->address;
    unsigned long physaddr = address - vma->vm_start + offset;
    unsigned long pageframe = physaddr >> PAGE_SHIFT;
    if(!pfn_valid(pageframe))
        return VM_FAULT_SIGBUS;
    pageptr = pfn_to_page(pageframe);
    printk(KERN_NOTICE "---- Fault, off %lx pageframe %lx\n", offset, pageframe);
    printk(KERN_NOTICE "page->index = %ld mapping %p\n", pageptr->index, pageptr->mapping);
    get_page(pageptr);
    vmf->page = pageptr;
    return 0;
}


static struct vm_operations_struct Simple_fault_vm_ops = {
	.open =   Simple_vma_open,
	.close =  Simple_vma_close,
    .fault = Simple_vma_fault,
};

static int Simple_fault_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset >= __pa(high_memory) || (filp->f_flags & O_SYNC))
		vma->vm_flags |= VM_IO;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

	vma->vm_ops = &Simple_fault_vm_ops;
	Simple_vma_open(vma);
	return 0;
}


/*
 * Set up the cdev structure for a device.
 */
static void Simple_setup_cdev(struct cdev *dev, int minor,
		struct file_operations *fops)
{
	int err, devno = MKDEV(Simple_major, minor);
    
	cdev_init(dev, fops);
	dev->owner = THIS_MODULE;
	dev->ops = fops;
	err = cdev_add (dev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk (KERN_NOTICE "Error %d adding Simple%d", err, minor);
}


/*
 * Our various sub-devices.
 */
/* Device 0 uses remap_pfn_range */
static struct file_operations Simple_remap_ops = {
	.owner   = THIS_MODULE,
	.open    = Simple_open,
	.release = Simple_release,
	.mmap    = Simple_remap_mmap,
};

/* Device 1 uses nopage */
static struct file_operations Simple_fault_ops = {
	.owner   = THIS_MODULE,
	.open    = Simple_open,
	.release = Simple_release,
	.mmap    = Simple_fault_mmap,
};

#define MAX_Simple_DEV 2

#if 0
static struct file_operations *Simple_fops[MAX_Simple_DEV] = {
	&Simple_remap_ops,
	&Simple_fault_ops,
};
#endif

/*
 * We export two Simple devices.  There's no need for us to maintain any
 * special housekeeping info, so we just deal with raw cdevs.
 */
static struct cdev SimpleDevs[MAX_Simple_DEV];

/*
 * Module housekeeping.
 */
static int Simple_init(void)
{
	int result;
	dev_t dev = MKDEV(Simple_major, 0);

	/* Figure out our device number. */
	if (Simple_major)
		result = register_chrdev_region(dev, 2, "Simple");
	else {
		result = alloc_chrdev_region(&dev, 0, 2, "Simple");
		Simple_major = MAJOR(dev);
	}
	if (result < 0) {
		printk(KERN_WARNING "Simple: unable to get major %d\n", Simple_major);
		return result;
	}
	if (Simple_major == 0)
		Simple_major = result;

	/* Now set up two cdevs. */
	Simple_setup_cdev(SimpleDevs, 0, &Simple_remap_ops);
	Simple_setup_cdev(SimpleDevs + 1, 1, &Simple_fault_ops);
	return 0;
}


static void Simple_cleanup(void)
{
	cdev_del(SimpleDevs);
	cdev_del(SimpleDevs + 1);
	unregister_chrdev_region(MKDEV(Simple_major, 0), 2);
}


module_init(Simple_init);
module_exit(Simple_cleanup);
