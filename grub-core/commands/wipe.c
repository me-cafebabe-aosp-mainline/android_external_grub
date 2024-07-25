#include <grub/command.h>
#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/partition.h>
#include <grub/types.h>

GRUB_MOD_LICENSE("GPLv3+");

// Define the module entry point
static grub_err_t
grub_cmd_wipe(grub_command_t cmd __attribute__ ((unused)), int argc, char **argv)
{
    char *device_name;

    if (argc != 1)
    {
        return grub_error(GRUB_ERR_BAD_ARGUMENT, "Usage: wipe (device)");
    }

    device_name = grub_file_get_device_name(argv[0]);
    grub_device_t dev = grub_device_open(device_name);
    grub_free(device_name);
    if (!dev)
    {
        return grub_error(GRUB_ERR_UNKNOWN_DEVICE, "Cannot open device");
    }

    if (!dev->disk || !dev->disk->partition)
    {
        grub_device_close(dev);
        return grub_error(GRUB_ERR_FILE_NOT_FOUND, "Invalid partition");
    }

    grub_disk_t disk = dev->disk;
    grub_partition_t part = disk->partition;

    grub_uint64_t part_start = part->start;
    grub_uint64_t part_size = part->len;

    grub_disk_cache_invalidate_all();

    grub_uint64_t zero_buf_size = 4096;
    char *zero_buf = grub_malloc(zero_buf_size);
    if (!zero_buf)
    {
        grub_device_close(dev);
        return grub_error(GRUB_ERR_OUT_OF_MEMORY, "Cannot allocate buffer");
    }

    grub_memset(zero_buf, 0, zero_buf_size);

    grub_uint64_t remaining_size = part_size;
    grub_uint64_t offset = part_start;

    while (remaining_size > 0)
    {
        grub_printf("remaining_size = %lu\n", remaining_size);
        grub_size_t write_size = remaining_size > zero_buf_size ? zero_buf_size : remaining_size;

        if (grub_disk_write(disk, offset, 0, write_size, zero_buf) != GRUB_ERR_NONE)
        {
            grub_free(zero_buf);
            grub_device_close(dev);
            return grub_error(GRUB_ERR_WRITE_ERROR, "Failed to write zeroes");
        }

        offset += write_size;
        remaining_size -= write_size;
    }

    grub_free(zero_buf);
    grub_device_close(dev);

    return GRUB_ERR_NONE;
}

// Register the module command
static grub_command_t cmd;

GRUB_MOD_INIT(wipe)
{
    cmd = grub_register_command("wipe", grub_cmd_wipe, "wipe (device)", "Wipe specified partition with zeroes");
}

GRUB_MOD_FINI(wipe)
{
    grub_unregister_command(cmd);
}
