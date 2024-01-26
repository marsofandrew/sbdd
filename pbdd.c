#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>

#define DEVICE_SECTOR_SHIFT         9
#define DEVICE_SECTOR_SIZE        (1 << DEVICE_SECTOR_SHIFT)
#define MIB_SECTORS                (1 << (20 - DEVICE_SECTOR_SHIFT))
#define DEVICE_NAME                "pbdd"

struct pbdd {
	wait_queue_head_t exitwait;
	atomic_t deleting;
	atomic_t refs_cnt;
	sector_t capacity;
	struct gendisk *gd;
	struct request_queue *q;
};

static struct pbdd __pbdd;
static int __pbdd_major = 0;
static unsigned long __pbdd_capacity_mib = 100;
static struct block_device *bdev;
static char *__device_path = "";
static fmode_t device_mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;

static void end_bio_request(struct bio *proxy_bio)
{
	struct bio *original_bio = proxy_bio->bi_private;
	bio_put(proxy_bio);
	bio_endio(original_bio);
	pr_debug("end io of proxy bio is handled\n");
}

static blk_qc_t forward_bio(struct bio *bio)
{
	int rc = BLK_STS_OK;

	struct bio *proxy_bio;
	proxy_bio = bio_clone_fast(bio, GFP_KERNEL, NULL);
	bio_set_dev(proxy_bio, bdev);
	proxy_bio->bi_private = bio;
	proxy_bio->bi_end_io = end_bio_request;
	pr_debug("sending proxy bio to %s\n", __device_path);

	rc = submit_bio(proxy_bio);
	return rc;
}

static blk_qc_t pbdd_make_request(struct request_queue *q, struct bio *bio)
{
	int rc = BLK_STS_OK;
	if (atomic_read(&__pbdd.deleting)) {
		pr_err("unable to process bio while deleting\n");
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	atomic_inc(&__pbdd.refs_cnt);

	rc = forward_bio(bio);

	if (atomic_dec_and_test(&__pbdd.refs_cnt)) {
		wake_up(&__pbdd.exitwait);
	}

	return rc;
}

/*
There are no read or write operations. These operations are performed by
the request() function associated with the request queue of the disk.
*/
static struct block_device_operations const __pbdd_bdev_ops = {
	.owner = THIS_MODULE,
};

static void free_blk_dev(void)
{
	if (bdev && !IS_ERR(bdev)) {
		blkdev_put(bdev, device_mode);
		pr_info("free block dev\n");
	}
}

static void delete_disk(void)
{
	/* gd will be removed only after the last reference put */
	if (__pbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__pbdd.gd);
	}
}

static void clean_queue(void)
{
	if (__pbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__pbdd.q);
	}
}

static void unregister_block_dev(void)
{
	if (__pbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__pbdd_major, DEVICE_NAME);
		__pbdd_major = 0;
		pr_info("blkdev is unregistered\n");
	}
}

/*
 * Called to clean everything
 */
static void pbdd_delete(void)
{
	atomic_set(&__pbdd.deleting, 1);

	wait_event(__pbdd.exitwait, !atomic_read(&__pbdd.refs_cnt));

	delete_disk();
	clean_queue();

	if (__pbdd.gd) {
		put_disk(__pbdd.gd);
	}

	memset(&__pbdd, 0, sizeof(struct pbdd));
	free_blk_dev();
	unregister_block_dev();
}

static int pbdd_create(void)
{
	int ret = 0;

	if (!strcmp(__device_path, "")) {
		pr_err("EMPTY device_path is invalid. Please provide correct device path");
		return -EINVAL;
	}

	bdev = blkdev_get_by_path(__device_path, device_mode, THIS_MODULE);
	pr_info("Pointer to bdev is get %d", IS_ERR(bdev));

	if (IS_ERR(bdev)) {
		pr_err("unable to get block device %s\n", __device_path);
		return -ENODEV;
	}

	/*
	This call is somewhat redundant, but used anyways by tradition.
	The number is to be displayed in /proc/devices (0 for auto).
	*/
	pr_info("registering blkdev\n");
	__pbdd_major = register_blkdev(0, DEVICE_NAME);
	if (__pbdd_major < 0) {
		pr_err("call register_blkdev() failed with %d\n", __pbdd_major);
		return -EBUSY;
	}

	memset(&__pbdd, 0, sizeof(struct pbdd));
	__pbdd.capacity = (sector_t) __pbdd_capacity_mib * MIB_SECTORS;

	sector_t capacity = get_capacity(bdev->bd_disk);
	if ((__pbdd.capacity > 0) && (capacity < __pbdd.capacity)) {
		pr_err("not enough capacity. Need %d; Actual: %d\n",__pbdd.capacity, capacity);
		return -EINVAL;
	} else if ((__pbdd.capacity == 0) && (capacity > 0)) {
		__pbdd.capacity = capacity;
		pr_info("$s capacity is set to %d\n", DEVICE_NAME, capacity);
	}

	init_waitqueue_head(&__pbdd.exitwait);

	pr_info("allocating queue\n");
	__pbdd.q = blk_alloc_queue(GFP_KERNEL);
	if (!__pbdd.q) {
		pr_err("call blk_alloc_queue() failed\n");
		return -ENOMEM;
	}
	blk_queue_make_request(__pbdd.q, pbdd_make_request);

	/* Configure queue */
	blk_queue_logical_block_size(__pbdd.q, DEVICE_SECTOR_SIZE);

	/* A disk must have at least one minor */
	pr_info("allocating disk\n");
	__pbdd.gd = alloc_disk(1);

	if (!__pbdd.gd) {
		return -ENOMEM;
	}

	/* Configure gendisk */
	__pbdd.gd->queue = __pbdd.q;
	__pbdd.gd->major = __pbdd_major;
	__pbdd.gd->first_minor = 0;
	__pbdd.gd->fops = &__pbdd_bdev_ops;
	/* Represents name in /proc/partitions and /sys/block */
	scnprintf(__pbdd.gd->disk_name, DISK_NAME_LEN, DEVICE_NAME);
	set_capacity(__pbdd.gd, __pbdd.capacity);

	/*
	Allocating gd does not make it available, add_disk() required.
	After this call, gd methods can be called at any time. Should not be
	called before the driver is fully initialized and ready to process reqs.
	*/
	pr_info("adding disk\n");
	add_disk(__pbdd.gd);

	return ret;
}

/*
Note __init is for the kernel to drop this function after
initialization complete making its memory available for other uses.
There is also __initdata note, same but used for variables.
*/
static int __init pbdd_init(void)
{
	int ret = 0;

	pr_info("starting initialization...\n");
	ret = pbdd_create();

	if (ret) {
		pr_warn("initialization failed\n");
		pbdd_delete();
	} else {
		pr_info("initialization complete\n");
	}

	return ret;
}

/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit pbdd_exit(void)
{
	pr_info("exiting...\n");
	pbdd_delete();
	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(pbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(pbdd_exit);

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __pbdd_capacity_mib, ulong, S_IRUGO
);

/* Set desired capacity with insmod*/
module_param_named(device_path, __device_path, charp, S_IRUSR
);
MODULE_PARM_DESC(__device_path,
"Device to which IO will be forwarded");

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
