#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/dl.h>
#include <grub/i18n.h>
#include <grub/types.h>

GRUB_MOD_LICENSE("GPLv3+");

#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024

#define VENDOR_BOOT_MAGIC "VNDRBOOT"
#define VENDOR_BOOT_MAGIC_SIZE 8
#define VENDOR_BOOT_ARGS_SIZE 2048
#define VENDOR_BOOT_NAME_SIZE 16

#define VENDOR_RAMDISK_TYPE_NONE 0
#define VENDOR_RAMDISK_TYPE_PLATFORM 1
#define VENDOR_RAMDISK_TYPE_RECOVERY 2
#define VENDOR_RAMDISK_TYPE_DLKM 3
#define VENDOR_RAMDISK_NAME_SIZE 32
#define VENDOR_RAMDISK_TABLE_ENTRY_BOARD_ID_SIZE 16

struct boot_img_hdr_v0 {
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

  grub_uint8_t name[BOOT_NAME_SIZE];
  grub_uint8_t cmdline[BOOT_ARGS_SIZE];

  grub_uint32_t id[8];

  grub_uint8_t extra_cmdline[BOOT_EXTRA_ARGS_SIZE];
} __attribute__((packed));

struct boot_img_hdr_v1 {
  struct boot_img_hdr_v0 v0;
  grub_uint32_t recovery_dtbo_size;
  grub_uint64_t recovery_dtbo_offset;
  grub_uint32_t header_size;
} __attribute__((packed));

struct boot_img_hdr_v2 {
  struct boot_img_hdr_v1 v1;
  grub_uint32_t dtb_size;
  grub_uint64_t dtb_addr;
} __attribute__((packed));

struct boot_img_hdr_v3 {
  grub_uint8_t magic[BOOT_MAGIC_SIZE];

  grub_uint32_t kernel_size;
  grub_uint32_t ramdisk_size;

  grub_uint32_t os_version;

  grub_uint32_t header_size;

  grub_uint32_t reserved[4];

  grub_uint32_t header_version;

  grub_uint8_t cmdline[BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE];
} __attribute__((packed));

struct boot_img_hdr_v4 {
  struct boot_img_hdr_v3 v3;
  grub_uint32_t signature_size;
} __attribute__((packed));

union boot_img_hdr_union {
  struct boot_img_hdr_v0 v0;
  struct boot_img_hdr_v1 v1;
  struct boot_img_hdr_v2 v2;
  struct boot_img_hdr_v3 v3;
  struct boot_img_hdr_v4 v4;
} __attribute__((packed));

enum android_boot_img_component {
  COMPONENT_KERNEL = 0,
  COMPONENT_RAMDISK,
  COMPONENT_SECOND,
  COMPONENT_RECOVERY_DTBO,
  COMPONENT_DTB,
  COMPONENT_COUNT
};

const char* android_boot_img_component_filename[COMPONENT_COUNT] = {
  [COMPONENT_KERNEL] = "kernel",
  [COMPONENT_RAMDISK] = "ramdisk.img",
  [COMPONENT_SECOND] = "second",
  [COMPONENT_RECOVERY_DTBO] = "recovery_dtbo",
  [COMPONENT_DTB] = "dtb",
};

struct component_info {
  grub_uint64_t offset;
  grub_uint32_t size;
};

struct android_boot_img_data {
  grub_disk_t disk;
  grub_uint32_t page_size;
  enum android_boot_img_component cur_component;
  struct component_info components[COMPONENT_COUNT];
};

/* Mount (parse header) */
static struct android_boot_img_data*
android_boot_mount(grub_disk_t disk) {
  struct android_boot_img_data* data;
  union boot_img_hdr_union hdr;

  if (grub_disk_read(disk, 0, 0, sizeof(hdr), &hdr))
    return NULL;

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0) {
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
  data->components[COMPONENT_KERNEL].offset = offset;
  data->components[COMPONENT_KERNEL].size = hdr.v0.kernel_size;
  offset += ALIGN_UP(hdr.v0.kernel_size, hdr.v0.page_size);

  /* Ramdisk */
  data->components[COMPONENT_RAMDISK].offset = offset;
  data->components[COMPONENT_RAMDISK].size = hdr.v0.ramdisk_size;
  offset += ALIGN_UP(hdr.v0.ramdisk_size, hdr.v0.page_size);

  /* Second stage (optional) */
  data->components[COMPONENT_SECOND].offset = offset;
  data->components[COMPONENT_SECOND].size = hdr.v0.second_size;
  offset += ALIGN_UP(hdr.v0.second_size, hdr.v0.page_size);

  /* Recovery DTBO (only if v1+) */
  if (hdr.v0.header_version >= 1 && hdr.v1.recovery_dtbo_size) {
    data->components[COMPONENT_RECOVERY_DTBO].offset
        = hdr.v1.recovery_dtbo_offset;
    data->components[COMPONENT_RECOVERY_DTBO].size = hdr.v1.recovery_dtbo_size;
  }

  /* DTB (only if v2+) */
  if (hdr.v0.header_version >= 2 && hdr.v2.dtb_size) {
    data->components[COMPONENT_DTB].offset = offset;
    data->components[COMPONENT_DTB].size = hdr.v2.dtb_size;
  }

  return data;
}

/* Directory listing */
#define CHECK_AND_ADD(check, component) \
  grub_memset(&info, 0, sizeof(info)); \
  if (hdr.check) \
    hook(android_boot_img_component_filename[component], &info, hook_data);
static grub_err_t
android_boot_dir(grub_device_t device,
                 const char* path __attribute__((unused)),
                 grub_fs_dir_hook_t hook, void* hook_data) {
  struct grub_dirhook_info info;
  union boot_img_hdr_union hdr;

  if (grub_disk_read(device->disk, 0, 0, sizeof(hdr), &hdr))
    return grub_error(GRUB_ERR_READ_ERROR, "Failed to load boot image header from disk");

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0) {
    switch (hdr.v0.header_version) {
      case 2:
        CHECK_AND_ADD(v2.dtb_size, COMPONENT_DTB)
        [[fallthrough]];
      case 1:
        CHECK_AND_ADD(v1.recovery_dtbo_size, COMPONENT_RECOVERY_DTBO)
        [[fallthrough]];
      case 0:
        CHECK_AND_ADD(v0.kernel_size, COMPONENT_KERNEL)
        CHECK_AND_ADD(v0.ramdisk_size, COMPONENT_RAMDISK)
        CHECK_AND_ADD(v0.second_size, COMPONENT_SECOND)
        break;
      case 4:
        [[fallthrough]];
      case 3:
        CHECK_AND_ADD(v3.kernel_size, COMPONENT_KERNEL)
        CHECK_AND_ADD(v3.ramdisk_size, COMPONENT_RAMDISK)
        break;
    default:
        return grub_error(GRUB_ERR_BAD_FS, "Unhandled Android boot header version");
    }
  } else if (grub_memcmp(hdr.v0.magic, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_SIZE) == 0) {
    // TODO
  } else {
    return grub_error(GRUB_ERR_UNKNOWN_FS, "Invalid Android boot image magic");
  }

  return GRUB_ERR_NONE;
}
#undef CHECK_AND_ADD

/* Open a file */
static grub_err_t
android_boot_open(grub_file_t file, const char* path) {
  struct android_boot_img_data* data;
  unsigned i;

  data = android_boot_mount(file->device->disk);
  if (!data)
    return grub_errno;

  for (i = 0; i < COMPONENT_COUNT; i++) {
    if (data->components[i].size > 0
        && grub_strcmp(path + 1, android_boot_img_component_filename[i]) == 0) {
      data->cur_component = i;
      file->size = data->components[i].size;
      file->offset = 0;
      file->data = data;
      file->not_easily_seekable = 0;
      return GRUB_ERR_NONE;
    }
  }

  grub_free(data);
  return grub_error(GRUB_ERR_FILE_NOT_FOUND, "Path %s not found in boot image",
                    path);
}

/* Read */
static grub_ssize_t
android_boot_read(grub_file_t file, char* buf, grub_size_t len) {
  struct android_boot_img_data* data = file->data;

  grub_uint64_t file_offset
      = data->components[data->cur_component].offset + file->offset;
  data->disk->read_hook = file->read_hook;
  data->disk->read_hook_data = file->read_hook_data;

  if (grub_disk_read(data->disk, 0, file_offset, len, buf)) {
    data->disk->read_hook = 0;
    return -1;
  }

  data->disk->read_hook = 0;
  return (grub_ssize_t)len;
}

/* Close */
static grub_err_t
android_boot_close(grub_file_t file) {
  grub_free(file->data);
  return GRUB_ERR_NONE;
}

/* Register FS */
static struct grub_fs android_boot_fs = {
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

GRUB_MOD_INIT(android_bootimg) {
  grub_fs_register(&android_boot_fs);
}

GRUB_MOD_FINI(android_bootimg) {
  grub_fs_unregister(&android_boot_fs);
}
