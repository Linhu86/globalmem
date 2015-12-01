#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/poll.h>

#define GLOBALFIFO_SIZE 0x1000  
#define FIFO_CLEAR 0x1 
#define GLOBALFIFO_MAJOR 0

static int globalfifo_major=GLOBALFIFO_MAJOR;

struct globalfifo_dev
{
  struct cdev cdev;
  unsigned int current_len;
  unsigned char mem[GLOBALFIFO_SIZE];
  struct semaphore sem; /* semaphore for concorrent control */
  wait_queue_head_t r_wait; /* wait blocking read header */
  wait_queue_head_t w_wait; /* wait blocking write header */
  struct fasync_struct *async_queue;
};

struct globalfifo_dev *globalfifo_devp; 

//support for globalfifo device fasync function.
static int globalfifo_fasync(int fd, struct file *filp, int mode)
{
  struct globalfifo_dev *dev = filp->private_data;
  return fasync_helper(fd, filp, mode, &dev->async_queue); /* setup a async notify queue. */
} 

int globalfifo_open(struct inode *inode, struct file *filp)
{
  filp->private_data = globalfifo_devp;
  return 0;
}

int globalfifo_release(struct inode *inode, struct file *filp)
{
  globalfifo_fasync(-1, filp, 0); /* Delete from async notify queue. */
  return 0;
}

static int globalfifo_ioctl(struct inode *inodep, struct file *filp,
                            unsigned int cmd, unsigned long arg)
{
  struct globalfifo_dev *dev = filp->private_data;
  switch (cmd)
  {
    case FIFO_CLEAR:
      down(&dev->sem);
      dev->current_len  = 0;
      memset(dev->mem, 0, GLOBALFIFO_SIZE);
      up(&dev->sem);
      printk(KERN_INFO "global fifo is set to zero.\n");
      break;
    default:
      return - EINVAL;
  }
  return 0;
}

static unsigned int globalfifo_poll(struct file *filp, poll_table *wait)
{
  unsigned int mask = 0;
  struct globalfifo_dev *dev = filp->private_data;

  down(&dev->sem);
  poll_wait(filp, &dev->r_wait, wait);
  poll_wait(filp, &dev->w_wait, wait);

  if(dev->current_len != 0)
  {
    mask |= POLLIN | POLLRDNORM;
  }

  if(dev->current_len != GLOBALFIFO_SIZE)
  {
    mask |= POLLOUT | POLLWRNORM;
  }

  up(&dev->sem);
  return mask;
}

static ssize_t globalfifo_read(struct file *filp, char __user *buf,size_t count,
                               loff_t *ppos)
{
  int ret = 0;
  struct globalfifo_dev *dev = filp->private_data; 
  DECLARE_WAITQUEUE(wait, current);

  down(&dev->sem);  /* mutex lock */
    
  add_wait_queue(&dev->r_wait, &wait); /* enter to wait queue. */

  if(dev->current_len == 0)
  {
    if(filp->f_flags & O_NONBLOCK)
    {
      ret = -EAGAIN;
      goto out;
    }
    __set_current_state(TASK_INTERRUPTIBLE);
    
    up(&dev->sem); /* unlock mutex */

    /*Current read blocking, let other process run. */
    schedule();
    
    if(signal_pending(current))
    {
      ret = -ERESTARTSYS;
      goto out2;
    }

    down(&dev->sem);
  }

  if(count > dev->current_len)
  {
    count = dev->current_len;
  }

  if (copy_to_user(buf, dev->mem, count))
  {
    ret = - EFAULT;
    goto out;
  }
  else
  {
    memcpy(dev->mem, dev->mem + count, dev->current_len - count);
    dev->current_len -= count;
    printk(KERN_INFO "read %lu bytes(s) from %d\n", count, dev->current_len);
    wake_up_interruptible(&dev->w_wait);
    ret = count;
  }

  out:
    up(&dev->sem);

  out2:
    remove_wait_queue(&dev->w_wait, &wait);
    set_current_state(TASK_RUNNING);
    return ret;
}


static ssize_t globalfifo_write(struct file *filp, const char __user
                                                *buf,size_t count, loff_t *ppos)
{
  struct globalfifo_dev *dev = filp->private_data; 
  int ret = 0;
  DECLARE_WAITQUEUE(wait, current);

  down(&dev->sem);

  add_wait_queue(&dev->w_wait, &wait);

  if(dev->current_len == GLOBALFIFO_SIZE)
  {
    if(filp->f_flags & O_NONBLOCK)
    {
        ret = -EAGAIN;
        goto out;
    }
    __set_current_state(TASK_INTERRUPTIBLE);
    
    up(&dev->sem);
    
    schedule();
    
    /*Current write blocking, let other process run. */
    if(signal_pending(current))
    {
      ret = -ERESTARTSYS;
      goto out2;
    }

    down(&dev->sem);
  }

  if(count > GLOBALFIFO_SIZE - dev->current_len)
  {
    count = GLOBALFIFO_SIZE - dev->current_len;
  }
 
  if (copy_from_user(dev->mem + dev->current_len, buf, count))
  {
    ret = - EFAULT;
    goto out;
  }
  else
  {
    dev->current_len += count;
    printk(KERN_INFO "written %lu bytes(s) from %u\n", count, dev->current_len);
    wake_up_interruptible(&dev->r_wait);

    if(dev->async_queue)
    {
      kill_fasync(&dev->async_queue, SIGIO, POLL_IN); // Send a notificaion to user space to read.
      printk(KERN_DEBUG "%s kill SIGIO.\n", __FUNCTION__);
    }

    ret = count;
  }

  out:
    up(&dev->sem);
  out2:
    remove_wait_queue(&dev->w_wait, &wait);
    set_current_state(TASK_RUNNING);
    return ret;
}

static const struct file_operations globalfifo_fops =
{
  .owner        = THIS_MODULE,
  .read         = globalfifo_read,
  .write        = globalfifo_write,
  .unlocked_ioctl = globalfifo_ioctl, 	/* ioctl */
  .poll         = globalfifo_poll,
  .fasync        = globalfifo_fasync,
  .open         = globalfifo_open,
  .release      = globalfifo_release,
};

static void globalfifo_setup_cdev(struct globalfifo_dev *dev, int
index)
{
  int err, devno = MKDEV(globalfifo_major, index);

  cdev_init(&dev->cdev, &globalfifo_fops);
  dev->cdev.owner = THIS_MODULE;
  dev->cdev.ops = &globalfifo_fops;
  err = cdev_add(&dev->cdev, devno, 1);
  if(err)
  {
    printk(KERN_NOTICE "Error %d adding LED%d", err, index);
  }
}

int globalfifo_init(void)
{
  int result;
  printk(KERN_INFO "start to init driver.\n");
  dev_t devno=MKDEV(globalfifo_major,0);

  if(globalfifo_major)
  {
    result=register_chrdev_region(devno,1,"globalfifo");
  }
  else
  {
    result=alloc_chrdev_region(&devno,0,1,"globalfifo");
    globalfifo_major=MAJOR(devno);
  }

  if (result < 0)
  {
    printk(KERN_ERR "device number allocation error.\n");
    return result;
  }

  globalfifo_devp=kmalloc(sizeof(struct globalfifo_dev),GFP_KERNEL);

  if(!globalfifo_devp) 
  {
    result= - ENOMEM;
    goto fail_malloc;
  }

  memset(globalfifo_devp, 0, sizeof(struct globalfifo_dev));
  globalfifo_setup_cdev(globalfifo_devp, 0);

  sema_init(&globalfifo_devp->sem, 1);

  init_waitqueue_head(&globalfifo_devp->r_wait);
  init_waitqueue_head(&globalfifo_devp->w_wait);
  
  return 0;

fail_malloc: 
  printk(KERN_ERR "device memory allocation error.\n");
  unregister_chrdev_region(devno, 1);
  return result;
}


void globalfifo_exit(void)
{
  cdev_del(&globalfifo_devp->cdev);   
  kfree(globalfifo_devp);
  unregister_chrdev_region(MKDEV(globalfifo_major,0),1); 
}

/* Module info */
MODULE_AUTHOR("Linhu Ying.");
MODULE_LICENSE("Dual BSD/GPL");
module_param(globalfifo_major,int,S_IRUGO);
module_init(globalfifo_init);
module_exit(globalfifo_exit);




















