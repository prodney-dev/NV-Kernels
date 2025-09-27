// SPDX-License-Identifier: GPL-2.0-only */
/*
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/memblock.h>
#include <linux/types.h>
#include <linux/boot_time_profiler.h>
#include <asm/page.h>
#include <asm/arch_timer.h>


#define CUS_FORMAT		0

#define BOOT_PROF_PROCFS_FILE "boot_time_prof_entries"

static const char *module_name = "boot_time_profiler";

static char *boot_prof_entry_data;
static spinlock_t boot_time_prof_lock;
static void *mapped_prof_start;
static void *mapped_prof_ro_start;
static bool is_privileged_driver;
static struct proc_dir_entry *prof_procfs_entry;
static phys_addr_t boot_prof_start;
static phys_addr_t boot_prof_size;
static phys_addr_t boot_prof_ro_start;
static phys_addr_t boot_prof_ro_size;


#define MAX_PROFILE_STRLEN	55

size_t add_boot_time_prof_entry(const char *buf);
size_t insert_boot_prof_entry(const char *buf, size_t len);

static u64 arch_timer_get_us(void)
{
    u64 cnt = arch_timer_read_counter();
    u32 freq = arch_timer_get_cntfrq();
    return cnt * 1000000ULL / freq;
}

struct profiler_record {
    char str[MAX_PROFILE_STRLEN + 1];
    uint64_t timestamp;
} __packed;

/**
 * boot_time_prof_show_entries - show the profiler entries
 * as a seq file in procfs
 *
 * Return: 0 on success or error code in case of failure.
 */
static void boot_time_prof_show_entries(struct seq_file *m, void *addr, int size)
{
    struct profiler_record *profiler_data;
    int count = 0;
    int i = 0;
    int prof_data_section_valid = 0;
    uint64_t last_valid_ts = 0;

    profiler_data = (struct profiler_record *)addr;
    count = size / sizeof(struct profiler_record);
    i = -1;
    seq_printf(m, "Showing data: \n");
    seq_printf(m, "%-54s\t%16s\t%16s\n", "Profile marker", "Timestamp (us)", "Time diff (us)");
    seq_printf(m, "------------------------------------------------");
    seq_printf(m, "------------------------------------------------\n");
    while (count--) {
        i++;
#if CUS_FORMAT
        char buf[MAX_PROFILE_STRLEN];
        memset(buf,0,MAX_PROFILE_STRLEN);
        strncpy(buf,profiler_data[i].str,MAX_PROFILE_STRLEN);
        buf[MAX_PROFILE_STRLEN-1]='\0';
        if (!profiler_data[i].timestamp || (strstr(buf,":") == NULL && strstr(buf,"[") == NULL)) {
            if (prof_data_section_valid) {
                seq_printf(m, "\n");
                prof_data_section_valid = 0;
            }
            continue;
        }
        seq_printf(m, "%-54s\t%16lld\n",buf,profiler_data[i].timestamp);
#else
        if (!profiler_data[i].timestamp && profiler_data[i].str[0] == '\0') {
            if (prof_data_section_valid) {
                seq_printf(m, "\n");
                prof_data_section_valid = 0;
            }
            continue;
        }

        seq_printf(m, "%-54s\t%16lld",
            profiler_data[i].str, profiler_data[i].timestamp);
        if (i > 0 && last_valid_ts <= profiler_data[i].timestamp) {
            seq_printf(m, "\t%16lld\n",
            profiler_data[i].timestamp - last_valid_ts);
        } else {
            seq_printf(m, "\n");
        }
#endif
        last_valid_ts = profiler_data[i].timestamp;
        prof_data_section_valid = 1;
    }
}

/**
 * boot_time_prof_show - Callback function to show the profiler
 * entries as a seq file in procfs
 *
 * Return: 0 on success or error code in case of failure.
 */
static int boot_time_prof_show(struct seq_file *m, void *p) {
    if (is_privileged_driver) {
        if (!mapped_prof_ro_start) {
            pr_err("%s: Error mapping RO profiling data\n", module_name);
            return -EINVAL;
        }

        boot_time_prof_show_entries(m, mapped_prof_ro_start, boot_prof_ro_size);
    } else {
        if (!mapped_prof_start) {
            pr_err("%s: Error mapping RW profiling data\n", module_name);
            return -EINVAL;
        }

        boot_time_prof_show_entries(m, mapped_prof_start, boot_prof_size);
    }

    return 0;
}

static ssize_t add_boot_time_prof_entry_from_file(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    ssize_t err;
    // Validate length before copying from userspace
    if (count >= MAX_PROFILE_STRLEN) {
        pr_err("%s: Failed to add record, invalid length: %ld. It should be less than %d\n",
            module_name, count, MAX_PROFILE_STRLEN);
        return -EINVAL;
    }
    if (!boot_prof_entry_data) {
        pr_err("%s: Failed to add record. Buffer not allocated\n",
            module_name);
        return -ENOMEM;
    }

    if (copy_from_user(boot_prof_entry_data, buf, count)) {
        return -EFAULT;
    }
    boot_prof_entry_data[count] = '\0';

    err = insert_boot_prof_entry(boot_prof_entry_data, count);
    if (err) {
        pr_err("%s: Error: Inserting profiler entry failed\n", module_name);
        return err;
    }
    return count;
}

static int boot_time_prof_file_open(struct inode *inode, struct file *file) {
    return single_open(file, boot_time_prof_show, NULL);
}

static const struct proc_ops prof_fops = {
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_open = boot_time_prof_file_open,
    .proc_write = add_boot_time_prof_entry_from_file,
    .proc_release = single_release,
};

/**
 * boot_time_prof_seq_file_init - Initialize the seq file
 * in procfs to show the profiler entries
 *
 * Return: 0 on success or error code in case of failure.
 */
static int boot_time_prof_seq_file_init(void) {
    prof_procfs_entry = proc_create(BOOT_PROF_PROCFS_FILE, 0444, NULL, &prof_fops);
    if (!prof_procfs_entry) {
        pr_err("%s: Failed to create file /proc/%s\n", module_name, BOOT_PROF_PROCFS_FILE);
        return -ENOMEM;
    }
    return 0;
}

static void boot_time_prof_seq_file_exit(void) {
    if (prof_procfs_entry)
        remove_proc_entry(BOOT_PROF_PROCFS_FILE, NULL);
}

/**
 * insert_boot_prof_entry - insert a new profiling point
 * @buf: string to insert as a profiling marker.
 * @len: length of the string without terminating NULL character
 *
 * Return: 0 on success or error code in case of failure.
 */
size_t insert_boot_prof_entry(const char *buf, size_t len)
{
    int count = 0;
#ifdef CUS_FORMAT
    int i = 1;
#else
    int i = 0;
#endif
    struct profiler_record *profiler_data;

    if (!mapped_prof_start || !boot_prof_size) {
        /*
         * This situation is possible if the kernel command line does not
         * have the arguments for the profiler so do not spam with error logs
         */
        pr_debug("%s: Error mapping profiling data\n", module_name);
        return -EINVAL;
    }

    if (len >= MAX_PROFILE_STRLEN) {
        pr_err("%s: Failed to insert record, invalid length: %ld. It should be less than %d\n",
            module_name, len, MAX_PROFILE_STRLEN);
        return -EINVAL;
    }

    if ((strstr(buf,":") == NULL || strstr(buf,":") == buf+len-1) && strstr(buf,"[") != buf) {
        pr_err("%s: Invalid profiling marker. Should be of format"
            "\"[Component name] <string marker>\" or \"Component name: <string marker>\"\n", module_name);
        return -EINVAL;
    }

    pr_info("%s: Inserting profiler data with len, %lu\n",
        __func__, len);

    spin_lock(&boot_time_prof_lock);

    profiler_data = (struct profiler_record *)mapped_prof_start;
    count = boot_prof_size / sizeof(struct profiler_record);
    while (i < count) {
        if (!profiler_data[i].timestamp)
            break;
        i++;
    }

    if (i == count) {
        pr_err("%s: Error profiling data buffer full\n", module_name);
        spin_unlock(&boot_time_prof_lock);
        return -ENOMEM;
    }

    profiler_data[i].timestamp = arch_timer_get_us();

    strncpy(profiler_data[i].str, buf, len);
    profiler_data[i].str[len]='\0';
    spin_unlock(&boot_time_prof_lock);

    pr_info("%s: Inserted profiler data at index %d, profiler_data(%s, %llu)\n",
            __func__, i, profiler_data[i].str, profiler_data[i].timestamp);

    return 0;
}

/**
 * add_boot_time_prof_entry - add a new profiling point
 * @buf: string to add as a profiling marker. Should follow
 * 		one of these formats:
 * 		[Component name] <String marker>
 * 		Component name: <String marker>
 *
 * Return: 0 on success or error code in case of failure.
 */
size_t add_boot_time_prof_entry(const char *buf)
{
    return insert_boot_prof_entry(buf, strlen(buf));
}
EXPORT_SYMBOL(add_boot_time_prof_entry);

/**
 * boot_time_prof_init - Initialize the proffs entries
 * and map the boot profile buffers.
 *
 * Return: 0 on success or error code in case of failure.
 */
static int __init boot_time_prof_init(void)
{
    void __iomem *ptr_prof_ro_carveout = NULL;
    void __iomem *ptr_prof_carveout = NULL;
    int err;

    if (!boot_prof_start || !boot_prof_size) {
        pr_err("%s: command line parameter boot_prof_dataptr not initialized\n",
            module_name);
        return -ENODEV;
    }

    ptr_prof_carveout = ioremap(boot_prof_start, boot_prof_size);
    if (!ptr_prof_carveout) {
        pr_err("%s: failed to map boot_prof_start\n", module_name);
        goto out_err;
    }

    pr_info("Remapped boot_prof_start(0x%llx) "
        "to address 0x%llx, size(0x%llx)\n",
        (u64)boot_prof_start,
        (__force u64)ptr_prof_carveout,
        (u64)boot_prof_size);

    mapped_prof_start = (__force void *)ptr_prof_carveout;

    if (boot_prof_ro_start != 0 && boot_prof_ro_size != 0) {
        ptr_prof_ro_carveout = ioremap(boot_prof_ro_start, boot_prof_ro_size);
        if (!ptr_prof_ro_carveout) {
            pr_err("%s: failed to map boot_prof_ro_start\n", module_name);
            goto out_err;
        }

        pr_info("Remapped boot_prof_ro_start(0x%llx) "
            "to address 0x%llx, size(0x%llx)\n",
            (u64)boot_prof_ro_start,
            (__force u64)ptr_prof_ro_carveout,
            (u64)boot_prof_ro_size);

        mapped_prof_ro_start = (__force void *)ptr_prof_ro_carveout;
        is_privileged_driver = true;
    } else {
        is_privileged_driver = false;
    }

    boot_prof_entry_data = kmalloc(MAX_PROFILE_STRLEN, GFP_KERNEL);
    if (!boot_prof_entry_data) {
        pr_err("%s: failed to allocate buffer for profile string\n", module_name);
        goto out_err;
    }

    err = boot_time_prof_seq_file_init();
    if (err != 0) {
        pr_err("%s: failed to initialize seq file\n", module_name);
        goto out_err;
    }

    spin_lock_init(&boot_time_prof_lock);

    return 0;

out_err:
    boot_time_prof_seq_file_exit();
    if (boot_prof_entry_data)
        kfree(boot_prof_entry_data);
    if (ptr_prof_carveout)
        iounmap(ptr_prof_carveout);
    if (ptr_prof_ro_carveout)
        iounmap(ptr_prof_ro_carveout);

    return -ENODEV;
}

/**
 * boot_time_prof_args - Parse the memory address args
 *
 * Return: 0 on success or error code in case of failure.
 */
static int __init boot_time_prof_args(char *options, phys_addr_t *boot_time_prof_arg_size,
            phys_addr_t *boot_time_prof_arg_start)
{
    char *p = options;

    *boot_time_prof_arg_size = memparse(p, &p);

    if (!p)
        return -EINVAL;
    if (*p != '@')
        return -EINVAL;

    *boot_time_prof_arg_start = memparse(p + 1, &p);

    if (!(*boot_time_prof_arg_size) || !(*boot_time_prof_arg_start)) {
        *boot_time_prof_arg_size = 0;
        *boot_time_prof_arg_start = 0;
        return 0;
    }

    return 0;
}

int __init boot_time_prof_module_init(void)
{
    if (!boot_prof_start || !boot_prof_size) {
        pr_debug("%s: Profiler args not initialized\n", __func__);
        return 0;
    }

    return boot_time_prof_init();
}

static void __exit boot_time_prof_module_exit(void)
{
    boot_time_prof_seq_file_exit();
    if (boot_prof_entry_data)
        kfree(boot_prof_entry_data);

    if (mapped_prof_ro_start)
        iounmap((void __iomem *)mapped_prof_ro_start);

    if (mapped_prof_start) {
        iounmap((void __iomem *)mapped_prof_start);
    }
}

static int __init handle_boot_time_prof_dataptr(char *str)
{
    int err = 0;
    err = boot_time_prof_args(str,
            &boot_prof_size,
            &boot_prof_start);
    if (err) {
        pr_err("%s: Failed to parse RW buffer size and addr\n", __func__);
        boot_prof_size = 0;
        boot_prof_start = 0;
    }
    return 0;
}

static int __init handle_boot_time_prof_ro_dataptr(char *str)
{
    int err = 0;
    err = boot_time_prof_args(str,
            &boot_prof_ro_size,
            &boot_prof_ro_start);
    if (err) {
        pr_err("%s: Failed to parse RO buffer size and add\n", __func__);
        boot_prof_ro_size = 0;
        boot_prof_ro_start = 0;
    }
    return 0;
}

early_param("boot_time_prof_dataptr", handle_boot_time_prof_dataptr);
early_param("boot_time_prof_ro_dataptr", handle_boot_time_prof_ro_dataptr);
