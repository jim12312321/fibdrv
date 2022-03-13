#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.11");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 500

/* swap string x */
#define _reverse(x)                                \
    do {                                           \
        for (int i = 0; i < strlen(x) >> 1; i++) { \
            if (x[i] == x[strlen(x) - i - 1])      \
                continue;                          \
            x[i] ^= x[strlen(x) - i - 1];          \
            x[strlen(x) - i - 1] ^= x[i];          \
            x[i] ^= x[strlen(x) - i - 1];          \
        }                                          \
    } while (0)

#define _max(a, b) (a ^ ((a ^ b) & -(a < b)))

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);

static ktime_t kt;

typedef struct fib_node {
    char data[256];
} fib_node;

/*
 * add string a and b mathematically.
 * add the char in a and b backward,and record carry if necessary.
 * reverse the answer in the end.
 */
static void string_add(char *a, char *b, char *out)
{
    short tmp = 0, carry = 0;
    long len_a = strlen(a), len_b = strlen(b);
    long len_max = _max(len_a, len_b);
    for (int i = 0; i < len_max; i++) {
        // cppcheck-suppress shiftTooManyBitsSigned
        tmp = ((len_a - i - 1) >> 63) ? 0 : (a[len_a - i - 1] - '0');
        // cppcheck-suppress shiftTooManyBitsSigned
        tmp += ((len_b - i - 1) >> 63) ? 0 : (b[len_b - i - 1] - '0');
        if (carry)
            tmp += carry;
        if (tmp >= 10) {
            tmp -= 10;
            carry = 1;
        } else {
            carry = 0;
        }
        out[i] = tmp + '0';
    }
    if (carry) {
        out[len_max] = carry + '0';
        len_max++;
    }
    out[len_max + 1] = '\0';
    _reverse(out);
}

/*
 * Init node with string.
 */
static struct fib_node *node_init(struct fib_node *_node, char *str)
{
    _node = kmalloc(sizeof(fib_node), GFP_KERNEL);
    strncpy(_node->data, str, strlen(str) + 1);
    _node->data[strlen(str) + 1] = '\0';
    return _node;
}

static int fib_seq_str(long long k, char *out)
{
    /* FIXME: use clz/ctz and fast algorithms to speed up */
    struct fib_node *f[k + 2];
    int n;
    f[0] = node_init(f[0], "0");
    f[1] = node_init(f[1], "1");

    for (int i = 2; i <= k; i++) {
        f[i] = node_init(f[i], "0");
        string_add(f[i - 2]->data, f[i - 1]->data, f[i]->data);
    }

    n = strlen(f[k]->data);
    if (copy_to_user(out, f[k]->data, n))
        return -EFAULT;

    for (int i = 0; i <= k; i++)
        kfree(f[i]);

    return n;
}

static int fib_time_proxy_t(long long k, char *out)
{
    kt = ktime_get();
    int n = fib_seq_str(k, out);
    kt = ktime_sub(ktime_get(), kt);
    return n;
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    return (ssize_t) fib_time_proxy_t(*offset, buf);
}

/* return cost time (ns) */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return ktime_to_ns(kt);
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
