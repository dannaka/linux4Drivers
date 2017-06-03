/*
 * jiq.c -- the just-in-queue module
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
 * $Id: jiq.c,v 1.7 2004/09/26 07:02:43 gregkh Exp $
 */
 
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>     /* everything... */
#include <linux/proc_fs.h>
#include <linux/errno.h>  /* error codes */
#include <linux/workqueue.h>
#include <linux/preempt.h>
#include <linux/interrupt.h> /* tasklets */
#include <linux/slab.h>

MODULE_AUTHOR("Dan Nakahara");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * The delay for the delayed workqueue timer file.
 */
static long delay = 1;
int max_timer_nr = 20;
int p_cnt = 0;

module_param(delay, long, 0);
module_param(max_timer_nr, int, 0);


/*
 * This module is a silly one: it only embeds short code fragments
 * that show how enqueued tasks `feel' the environment
 */
#define LIMIT   (PAGE_SIZE-128)	/* don't print any more after this size */

/*
 * Print information about the current environment. This is called from
 * within the task queues. If the limit is reached, awake the reading
 * process.
 */
static DECLARE_WAIT_QUEUE_HEAD (jiq_wait);


/*
 * Keep track of info we need between task queue runs.
 */
static struct clientdata {
    struct seq_file *sq_file;
	unsigned long jiffies;
    unsigned int flag;
} jiq_data;

#define SCHEDULER_QUEUE ((task_queue *) 1)

static void jiq_print_tasklet(unsigned long);
static DECLARE_TASKLET(jiq_tasklet, jiq_print_tasklet, (unsigned long)&jiq_data);

/*
 * Do the printing; return non-zero if the task should be rescheduled.
 */
static int jiq_print(void *ptr)
{
	struct clientdata *data = (struct clientdata*)ptr;
	struct seq_file *file = data->sq_file;
    int count = file->count;
	unsigned long j = jiffies;

	if (count > LIMIT) { 
        data->flag = 1;
		wake_up_interruptible(&jiq_wait);
		return 0;
	}

	if (count == 0)
		seq_printf(file, "    time  delta preempt   pid cpu command\n");

  	/* intr_count is only exported since 1.3.5, but 1.99.4 is needed anyways */
	seq_printf(file, "%9li  %4li     %3i %5i %3i %s\n",
			j, j - data->jiffies,
			preempt_count(), current->pid, smp_processor_id(),
			current->comm);

	data->jiffies = j;
	return 1;
}
static void jiq_print_wq(struct work_struct*);
static void jiq_print_wqdelayed(struct work_struct*);

static DECLARE_WORK(jiq_work, jiq_print_wq);
static DECLARE_DELAYED_WORK(jiq_delayed_work, jiq_print_wqdelayed);

/*
 * Call jiq_print from a work queue
 */
static void jiq_print_wq(struct work_struct *w)
{
	if (! jiq_print ((void *)&jiq_data))
		return;
    
    schedule_work(&jiq_work);
}

static void jiq_print_wqdelayed(struct work_struct *w)
{
    struct delayed_work *dwork = to_delayed_work(w);
    
	if (! jiq_print ((void *)&jiq_data))
		return;
    
    schedule_delayed_work(dwork, delay);
}


static int jiqwq_show(struct seq_file *file, void *v)
{
	DEFINE_WAIT(wait);
	
	jiq_data.jiffies = jiffies;      /* initial time */
    jiq_data.sq_file = file;
    
	prepare_to_wait(&jiq_wait, &wait, TASK_INTERRUPTIBLE);
	schedule_work(&jiq_work);
	schedule();
	finish_wait(&jiq_wait, &wait);

	return 0;
}


static int jiqwqdelayed_show(struct seq_file *file, void *v)
{
	DEFINE_WAIT(wait);
	
	jiq_data.jiffies = jiffies;      /* initial time */
    jiq_data.sq_file = file;
    
	prepare_to_wait(&jiq_wait, &wait, TASK_INTERRUPTIBLE);
	schedule_delayed_work(&jiq_delayed_work, delay);
	schedule();
	finish_wait(&jiq_wait, &wait);

	return 0;
}


/*
 * Call jiq_print from a tasklet
 */
static void jiq_print_tasklet(unsigned long ptr)
{
	if (jiq_print ((void *) ptr))
		tasklet_schedule (&jiq_tasklet);
}


static int jiqtasklet_show(struct seq_file *file, void *v)
{
	jiq_data.jiffies = jiffies;      /* initial time */
    jiq_data.sq_file = file;

	tasklet_schedule(&jiq_tasklet);
	wait_event_interruptible(jiq_wait, jiq_data.flag != 0);    /* sleep till completion */
    jiq_data.flag = 0;

	return 0;
}


/*
 * This one, instead, tests out the timers.
 */
static struct timer_list jiq_timer;

static void jiq_timedout(unsigned long ptr)
{
    struct clientdata *data = (struct clientdata*)ptr;
	jiq_print((void *)data);            /* print a line */
    data->flag = 1;
	wake_up_interruptible(&jiq_wait);  /* awake the process */
}


static int jiqruntimer_show(struct seq_file *file, void *v)
{
	jiq_data.jiffies = jiffies;
    jiq_data.sq_file = file;

	init_timer(&jiq_timer);              /* init the timer structure */
	jiq_timer.function = jiq_timedout;
	jiq_timer.data = (unsigned long)&jiq_data;
	jiq_timer.expires = jiffies + HZ; /* one second */

	jiq_print((void *)&jiq_data);   /* print and go to sleep */
	add_timer(&jiq_timer);
	wait_event_interruptible(jiq_wait, jiq_data.flag != 0);  /* RACE */
    jiq_data.flag = 0;
	del_timer_sync(&jiq_timer);  /* in case a signal woke us up */
    
	return 0;
}


#define BUILD_JIQ_PROC_SINGLE_OPEN(type)    \
    static int type##_proc_single_open(struct inode *inode, struct file *file)   \
    {   \
        return single_open(file, &type##_show, NULL);       \
    }

/*
 * Now to implement the /proc file we need only make an open
 * method which sets up the sequence operators.
 */
BUILD_JIQ_PROC_SINGLE_OPEN(jiqruntimer)
BUILD_JIQ_PROC_SINGLE_OPEN(jiqtasklet)
BUILD_JIQ_PROC_SINGLE_OPEN(jiqwq)
BUILD_JIQ_PROC_SINGLE_OPEN(jiqwqdelayed)

#define BUILD_JIQ_PROC_SINGLE_OPS(type)     \
    static struct file_operations type##_proc_single_ops = {        \
        .owner   = THIS_MODULE,     \
        .open    = type##_proc_single_open,     \
        .read    = seq_read,        \
        .llseek  = seq_lseek,       \
        .release = single_release,      \
    };

/*
 * Create a set of file operations for our proc file.
 */
BUILD_JIQ_PROC_SINGLE_OPS(jiqruntimer)
BUILD_JIQ_PROC_SINGLE_OPS(jiqtasklet)
BUILD_JIQ_PROC_SINGLE_OPS(jiqwq)
BUILD_JIQ_PROC_SINGLE_OPS(jiqwqdelayed)

/*
 * the init/clean material
 */
static int jiq_init(void)
{
	/* this line is in jiq_init() */
    jiq_data.flag = 0;

	proc_create("jiqwq", 0, NULL, &jiqwq_proc_single_ops);
	proc_create("jiqwqdelay", 0, NULL, &jiqwqdelayed_proc_single_ops);
	proc_create("jiqruntimer", 0, NULL, &jiqruntimer_proc_single_ops);
	proc_create("jiqtasklet", 0, NULL, &jiqtasklet_proc_single_ops);

	return 0; /* succeed */
}

static void jiq_cleanup(void)
{
	remove_proc_entry("jiqwq", NULL);
	remove_proc_entry("jiqwqdelay", NULL);
	remove_proc_entry("jiqruntimer", NULL);
	remove_proc_entry("jiqtasklet", NULL);
}


module_init(jiq_init);
module_exit(jiq_cleanup);
