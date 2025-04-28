#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/dl.h>
#include <grub/i18n.h>
#include <grub/types.h>

GRUB_MOD_LICENSE("GPLv3+");

/* Android Boot Image Magic */
#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8

struct boot_img_hdr_v0
{
  grub_uint8_t magic[BOOT_MAGIC_SIZE];

  grub_uint32_t kernel_size;
  grub_uint32_t kernel_addr;

  grub_uint32_t ramdisk_size;
  grub_uint32_t ramdisk_addr;

  grub_uint32_t second_size;
  grub_uint32_t second_addr;

  grub_uint32_t tags_addr;
  grub_uint32_t page_size;

  grub_uint32_t header_version;
  grub_uint32_t os_version;

  grub_uint8_t name[16];
  grub_uint8_t cmdline[512];

  grub_uint32_t id[8];

  grub_uint8_t extra_cmdline[1024];
} __attribute__((packed));

struct boot_img_hdr_v1
{
  struct boot_img_hdr_v0 v0;
  grub_uint32_t recovery_dtbo_size;
  grub_uint64_t recovery_dtbo_offset;
  grub_uint32_t header_size;
} __attribute__((packed));

struct boot_img_hdr_v2
{
  struct boot_img_hdr_v1 v1;
  grub_uint32_t dtb_size;
  grub_uint64_t dtb_addr;
} __attribute__((packed));

enum android_boot_img_component
{
  COMPONENT_KERNEL = 0,
  COMPONENT_RAMDISK,
  COMPONENT_SECOND,
  COMPONENT_RECOVERY_DTBO,
  COMPONENT_DTB,
  COMPONENT_COUNT
};

struct component_info
{
  const char *name;
  grub_uint64_t offset;
  grub_uint32_t size;
};

struct android_boot_img_data
{
  grub_disk_t disk;
  grub_uint32_t page_size;
  struct component_info components[COMPONENT_COUNT];
};

/* Utility: align up to page boundary */
static grub_uint64_t
align_to_page(grub_uint64_t val, grub_uint32_t page_size)
{
  return ((val + page_size - 1) / page_size) * page_size;
}

/* Mount (parse header) */
static struct android_boot_img_data *
android_boot_mount(grub_disk_t disk)
{
  struct android_boot_img_data *data;
  struct boot_img_hdr_v2 hdr;

  if (grub_disk_read(disk, 0, 0, sizeof(hdr), &hdr))
    return NULL;

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0)
    {
      grub_error(GRUB_ERR_BAD_FS, "Invalid Android boot image magic");
      return NULL;
    }

  data = grub_zalloc(sizeof(*data));
  if (!data)
    return NULL;

  data->disk = disk;
  data->page_size = hdr.v0.page_size;

  grub_uint64_t offset = data->page_size;

  /* Kernel */
  data->components[COMPONENT_KERNEL].name = "kernel";
  data->components[COMPONENT_KERNEL].offset = offset;
  data->components[COMPONENT_KERNEL].size = hdr.v0.kernel_size;
  offset += align_to_page(hdr.v0.kernel_size, hdr.v0.page_size);

  /* Ramdisk */
  data->components[COMPONENT_RAMDISK].name = "ramdisk";
  data->components[COMPONENT_RAMDISK].offset = offset;
  data->components[COMPONENT_RAMDISK].size = hdr.v0.ramdisk_size;
  offset += align_to_page(hdr.v0.ramdisk_size, hdr.v0.page_size);

  /* Second stage (optional) */
  data->components[COMPONENT_SECOND].name = "second";
  data->components[COMPONENT_SECOND].offset = offset;
  data->components[COMPONENT_SECOND].size = hdr.v0.second_size;
  offset += align_to_page(hdr.v0.second_size, hdr.v0.page_size);

  /* Recovery DTBO (only if v1+) */
  if (hdr.v0.header_version >= 1 && hdr.v1.recovery_dtbo_size)
    {
      data->components[COMPONENT_RECOVERY_DTBO].name = "recovery_dtbo";
      data->components[COMPONENT_RECOVERY_DTBO].offset = hdr.v1.recovery_dtbo_offset;
      data->components[COMPONENT_RECOVERY_DTBO].size = hdr.v1.recovery_dtbo_size;
    }

  /* DTB (only if v2+) */
  if (hdr.v0.header_version >= 2 && hdr.v2.dtb_size)
    {
      data->components[COMPONENT_DTB].name = "dtb";
      data->components[COMPONENT_DTB].offset = offset;
      data->components[COMPONENT_DTB].size = hdr.v2.dtb_size;
    }

  return data;
}

/* Directory listing */
static grub_err_t
android_boot_dir(grub_device_t device, const char *path,
                 grub_fs_dir_hook_t hook, void *hook_data)
{
  struct android_boot_img_data *data;
  grub_err_t err = GRUB_ERR_NONE;
  unsigned i;

  data = android_boot_mount(device->disk);
  if (!data)
    return grub_errno;

  for (i = 0; i < COMPONENT_COUNT; i++)
    {
      struct grub_dirhook_info info;
      grub_memset(&info, 0, sizeof(info));
      if (data->components[i].size > 0)
        hook(data->components[i].name, &info, hook_data);
    }

  grub_free(data);
  return err;
}

/* Open a file */
static grub_err_t
android_boot_open(grub_file_t file, const char *path)
{
  struct android_boot_img_data *data;
  unsigned i;

  data = android_boot_mount(file->device->disk);
  if (!data)
    return grub_errno;

  for (i = 0; i < COMPONENT_COUNT; i++)
    {
      if (data->components[i].size > 0 &&
          grub_strcmp(path, data->components[i].name) == 0)
        {
          file->size = data->components[i].size;
          file->offset = 0;
          file->data = data;
          file->not_easily_seekable = 0;
          return GRUB_ERR_NONE;
        }
    }

  grub_free(data);
  return grub_error(GRUB_ERR_FILE_NOT_FOUND, "File not found in boot image");
}

/* Read */
static grub_ssize_t
android_boot_read(grub_file_t file, char *buf, grub_size_t len)
{
  struct android_boot_img_data *data = file->data;
  unsigned i;

  for (i = 0; i < COMPONENT_COUNT; i++)
    {
      if (grub_strcmp(file->path, data->components[i].name) == 0)
        {
          grub_uint64_t file_offset = data->components[i].offset + file->offset;
          data->disk->read_hook = file->read_hook;
          data->disk->read_hook_data = file->read_hook_data;

          if (grub_disk_read(data->disk, 0, file_offset, len, buf))
            {
              data->disk->read_hook = 0;
              return -1;
            }

          data->disk->read_hook = 0;
          return (grub_ssize_t) len;
        }
    }

  return -1;
}

/* Close */
static grub_err_t
android_boot_close(grub_file_t file)
{
  grub_free(file->data);
  return GRUB_ERR_NONE;
}

/* Register FS */
static struct grub_fs android_boot_fs =
  {
    .name = "android_bootimg",
    .fs_dir = android_boot_dir,
    .fs_open = android_boot_open,
    .fs_read = android_boot_read,
    .fs_close = android_boot_close,
#ifdef GRUB_UTIL
    .reserved_first_sector = 0,
    .blocklist_install = 0,
#endif
  };

GRUB_MOD_INIT(android_bootimg)
{
  grub_fs_register(&android_boot_fs);
}

GRUB_MOD_FINI(android_bootimg)
{
  grub_fs_unregister(&android_boot_fs);
}
