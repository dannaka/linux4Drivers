/*  -*- C -*-
 * mmap.c -- memory mapping for the scullv char module
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
 * $Id: _mmap.c.in,v 1.13 2004/10/18 18:07:36 corbet Exp $
 */

#include <linux/module.h>

#include <linux/mm.h>		/* everything */
#include <linux/errno.h>	/* error codes */
#include <asm/pgtable.h>

#include "scullv.h"		/* local definitions */


/*
 * open and close: just keep track of how many times the device is
 * mapped, to avoid releasing it.
 */

void scullv_vma_open(struct vm_area_struct *vma)
{
	struct scullv_dev *dev = vma->vm_private_data;

	dev->vmas++;
}

void scullv_vma_close(struct vm_area_struct *vma)
{
	struct scullv_dev *dev = vma->vm_private_data;

	dev->vmas--;
}

/*
 * The fault method: the core of the file. It retrieves the
 * page required from the scullv device and returns it to the
 * user. The count for the page must be incremented, because
 * it is automatically decremented at page unmap.
 *
 * For this reason, "order" must be zero. Otherwise, only the first
 * page has its count incremented, and the allocating module must
 * release it as a whole block. Therefore, it isn't possible to map
 * pages from a multipage block: when they are unmapped, their count
 * is individually decreased, and would drop to 0.
 */
static int scullv_vma_fault(struct vm_area_struct *vma,
        struct vm_fault *vmf)
{
    struct scullv_dev *ptr, *dev = vma->vm_private_data;
    int result = VM_FAULT_SIGBUS;
    struct page *page;
    void * pageptr = NULL;
    pgoff_t pgoff = vmf->pgoff;  

    down(&dev->sem);
    printk (KERN_NOTICE "scullv_vma_fault: pgoff   = %lx\n", pgoff);
    if (pgoff >= dev->size) goto out;

    /*
     * Now retrieve the scullv device from the list, then the page.
     * If the device has holes, the process receives a SIGBUS when
     * accessing the hole.
     */
    for (ptr = dev; ptr && pgoff >= dev->qset;) {
        ptr = ptr->next;
        pgoff -= dev->qset;
    }
    if (ptr && ptr->data) pageptr = ptr->data[pgoff];
    if (!pageptr) goto out; /* hole or end-of-file */
    /* 
     * got it, now convert pointer to a struct page and increment the count.
	 * After scullv lookup, "page" is now the address of the page
	 * needed by the current process. Since it's a vmalloc address,
	 * turn it into a struct page.
	 */
	page = vmalloc_to_page(pageptr);
    get_page(page);
    vmf->page = page;
    result = 0;
out:
    up(&dev->sem);
    return result;
}


struct vm_operations_struct scullv_vm_ops = {
	.open =     scullv_vma_open,
	.close =    scullv_vma_close,
	.fault =   scullv_vma_fault,
};


int scullv_mmap(struct file *filp, struct vm_area_struct *vma)
{

	/* don't do anything here: "fault" will set up page table entries */
	vma->vm_ops = &scullv_vm_ops;
	vma->vm_flags |= VM_RESERVED;
	vma->vm_private_data = filp->private_data;
	scullv_vma_open(vma);
	return 0;
}

