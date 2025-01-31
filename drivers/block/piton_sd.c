#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <asm/io.h>

#define DRV_NAME     "piton_sd"
#define DRV_VERSION  "1.0"
#define DRV_RELDATE  "Jan 31, 2019"

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OpenPiton FPGA SD card device driver");

static char version[] =
        DRV_NAME ":v" DRV_VERSION " " DRV_RELDATE " \n";

#ifndef PITON_SD_BASE_ADDR
#define PITON_SD_BASE_ADDR  0xf000000000L
#endif

#ifndef PITON_SD_LENGTH
#define PITON_SD_LENGTH     0xff0300000L
#endif

#define PITON_SD_NMINORS    1
#define PITON_SD_BLOCK_SIZE 512

// Partition Table Header (LBA 1)
typedef struct gpt_pth
{
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size; //! little endian, usually 0x5c = 92
    uint32_t crc_header;
    uint32_t reserved; //! must be 0
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t nr_partition_entries;
    uint32_t size_partition_entry; //! usually 0x80 = 128
    uint32_t crc_partition_entry;
} gpt_pth_t;

// Partition Entries (LBA 2-33)
typedef struct partition_entries
{
    uint8_t partition_type_guid[16];
    uint8_t partition_guid[16];
    uint64_t first_lba;
    uint64_t last_lba; //! inclusive
    uint64_t attributes;
    uint8_t name[72]; //! utf16 encoded
} partition_entries_t;

static int piton_sd_major = 0;
static char * piton_sd_name = "piton_sd";
static struct gendisk  *piton_sd_gendisk;
static struct block_device_operations piton_sd_bdev_ops = {};

static partition_entries_t partition = {};

// helpers
#define PITON_SD_READ 0
#define PITON_SD_WRITE 1
static void piton_sd_rw(int rw, void *dst, uint32_t lba, unsigned long size) {
    uint64_t raw_addr = PITON_SD_BASE_ADDR;
    raw_addr += ((uint64_t)lba) << 9;

    volatile uint64_t * p = (uint64_t *)dst;
    for (unsigned long i = 0; i < size; i += 8, raw_addr += 8) {
        if (rw == PITON_SD_READ) {
            *(p++) = readq(raw_addr);
        } else {
            writeq(*(p++), raw_addr);
        }
    }
}

static blk_qc_t piton_sd_make_request(struct request_queue *queue, struct bit *bio) {
    struct bio_vec bvec;
    struct bvec_iter iter;
    int direction = bio_data_dir(bio);

    bio_for_each_segment(bvec, bio, iter) {
        unsigned long real_addr = (unsigned long) page_to_phys(bvec.bv_page);
        real_addr += bvec.bv_offset;
        piton_sd_rw(direction, (void *)real_addr, iter.bi_sector, bvec.bv_len);
    }

    bio_endio(bio);
    return BLK_QC_T_NONE;
}

static int piton_sd_init(void)
{
    static unsigned int version_printed = 0;
    struct request_queue *queue = NULL;
    int result;
    uint8_t lba_buf[PITON_SD_BLOCK_SIZE];
    gpt_pth_t * pth;

    if (version_printed++ == 0) {
	    printk(KERN_INFO  "%s", version);
    }

    // load the first partition on the disk
    piton_sd_rw(PITON_SD_READ, lba_buf, 1, PITON_SD_BLOCK_SIZE);
    pth = (gpt_pth_t *)lba_buf;

    piton_sd_rw(PITON_SD_READ, lba_buf, pth->partition_entries_lba, PITON_SD_BLOCK_SIZE);
    partition = ((partition_entries_t *)lba_buf)[1]; // read partition 2

    // register device
    result = register_blkdev(piton_sd_major, piton_sd_name);
    if (result <= 0) {
        printk(KERN_ERR "%s: register_blkdev returned error %d \n", DRV_NAME, result);
        return -EIO;
    }
    if (piton_sd_major == 0) {
        piton_sd_major = result;
    }

    queue = blk_alloc_queue(GFP_KERNEL);
    if (queue == NULL) {
        printk(KERN_ERR "%s: blk_alloc_queue() returned NULL. \n", DRV_NAME);
        goto fail;
    }

    piton_sd_gendisk = alloc_disk(PITON_SD_NMINORS);
    if (piton_sd_gendisk == NULL) {
        printk(KERN_ERR "%s: alloc_disk() returned NULL. \n", DRV_NAME);
        goto fail;
    }

    blk_queue_make_request(queue, piton_sd_make_request);

    piton_sd_gendisk->queue = queue;
    piton_sd_gendisk->major = piton_sd_major;
    piton_sd_gendisk->first_minor = 0;
    snprintf(piton_sd_gendisk->disk_name, 32, "%s", piton_sd_name);
    piton_sd_gendisk->fops = &piton_sd_bdev_ops;
    set_capacity(piton_sd_gendisk, partition.last_lba - partition.first_lba + 1);
    add_disk(piton_sd_gendisk);

    return 0;


fail:
    unregister_blkdev(piton_sd_major, piton_sd_name);

    if (queue) {
	    blk_cleanup_queue(queue);
    }

    return -EIO;
}


static void piton_sd_exit(void)
{
    del_gendisk(piton_sd_gendisk);
    put_disk(piton_sd_gendisk);
    blk_cleanup_queue(piton_sd_gendisk->queue);

    unregister_blkdev(piton_sd_major, piton_sd_name);

    return;
}


module_init(piton_sd_init);
module_exit(piton_sd_exit);
