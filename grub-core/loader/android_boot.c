#include <grub/types.h>
#include <grub/err.h>
#include <grub/mm.h>
#include <grub/file.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/command.h>
#include <grub/misc.h>
#include <grub/lib/cmdline.h>

#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024

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

union boot_img_hdr_v0_v1_v2 {
  struct boot_img_hdr_v0 v0;
  struct boot_img_hdr_v1 v1;
  struct boot_img_hdr_v2 v2;
} __attribute__((packed));

/* Load a file or a raw partition into memory */
static grub_err_t
android_boot_load_image(const char* path, void** buf, grub_size_t* size) {
  if (grub_strchr(path, '/')) {
    /* It's a file. */
    grub_file_t file = grub_file_open(path, GRUB_FILE_TYPE_NONE);
    if (!file)
      return grub_error(GRUB_ERR_FILE_NOT_FOUND, "cannot open file");

    *size = grub_file_size(file);
    *buf = grub_malloc(*size);
    if (!*buf) {
      grub_file_close(file);
      return grub_error(GRUB_ERR_OUT_OF_MEMORY, "cannot allocate memory");
    }

    if (grub_file_read(file, *buf, *size) != (grub_ssize_t)*size) {
      grub_file_close(file);
      grub_free(*buf);
      return grub_error(GRUB_ERR_READ_ERROR, "failed to read file");
    }

    grub_file_close(file);
  } else {
    /* It's a raw device. */
    grub_device_t dev = grub_device_open(path);
    if (!dev)
      return grub_error(GRUB_ERR_UNKNOWN_DEVICE, "cannot open device");

    /* Read first page (boot header). */
    union boot_img_hdr_v0_v1_v2 hdr;
    if (grub_disk_read(dev->disk, 0, 0, sizeof(hdr), &hdr)) {
      grub_device_close(dev);
      return grub_error(GRUB_ERR_READ_ERROR, "failed to read boot header");
    }

    if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0) {
      grub_device_close(dev);
      return grub_error(GRUB_ERR_BAD_OS, "invalid boot image magic");
    }

    /* Calculate total size. */
    grub_uint32_t page_size = hdr.v0.page_size;
    grub_uint32_t n_kernel = (hdr.v0.kernel_size + page_size - 1) / page_size;
    grub_uint32_t n_ramdisk
        = (hdr.v0.ramdisk_size + page_size - 1) / page_size;
    grub_uint32_t n_second = (hdr.v0.second_size + page_size - 1) / page_size;
    grub_uint64_t total_pages = 1 + n_kernel + n_ramdisk + n_second;

    if (hdr.v0.header_version >= 1) {
      grub_uint32_t n_recovery
          = (hdr.v1.recovery_dtbo_size + page_size - 1) / page_size;
      total_pages += n_recovery;
    }

    if (hdr.v0.header_version >= 2) {
      grub_uint32_t n_dtb = (hdr.v2.dtb_size + page_size - 1) / page_size;
      total_pages += n_dtb;
    }

    *size = total_pages * page_size;
    *buf = grub_malloc(*size);
    if (!*buf) {
      grub_device_close(dev);
      return grub_error(GRUB_ERR_OUT_OF_MEMORY, "cannot allocate memory");
    }

    if (grub_disk_read(dev->disk, 0, 0, *size, *buf)) {
      grub_free(*buf);
      grub_device_close(dev);
      return grub_error(GRUB_ERR_READ_ERROR, "failed to read boot image");
    }

    grub_device_close(dev);
  }

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_android_boot(grub_command_t cmd __attribute__((unused)), int argc,
                      char** argv) {
  if (argc < 1)
    return grub_error(GRUB_ERR_BAD_ARGUMENT,
                      "usage: android_boot <device or file>");

  void* image = NULL;
  grub_size_t image_size = 0;

  grub_err_t err = android_boot_load_image(argv[0], &image, &image_size);
  if (err)
    return err;

  union boot_img_hdr_v0_v1_v2* hdr = (union boot_img_hdr_v0_v1_v2*)image;
  grub_uint32_t page_size = hdr->v0.page_size;

  grub_uint8_t* p = (grub_uint8_t*)image + page_size;
  grub_uint8_t* kernel __attribute__((unused)) = p;
  p += ((hdr->v0.kernel_size + page_size - 1) / page_size) * page_size;

  grub_uint8_t* ramdisk __attribute__((unused)) = p;
  p += ((hdr->v0.ramdisk_size + page_size - 1) / page_size) * page_size;

  grub_uint8_t* second __attribute__((unused)) = NULL;
  if (hdr->v0.second_size > 0) {
    second = p;
    p += ((hdr->v0.second_size + page_size - 1) / page_size) * page_size;
  }

  grub_uint8_t* recovery_dtbo __attribute__((unused)) = NULL;
  if (hdr->v0.header_version >= 1) {
    if (hdr->v1.recovery_dtbo_size > 0) {
      recovery_dtbo = p;
      p += ((hdr->v1.recovery_dtbo_size + page_size - 1) / page_size)
           * page_size;
    }
  }

  grub_uint8_t* dtb __attribute__((unused)) = NULL;
  if (hdr->v0.header_version >= 2) {
    if (hdr->v2.dtb_size > 0) {
      dtb = p;
      p += ((hdr->v2.dtb_size + page_size - 1) / page_size) * page_size;
    }
  }

  /*
  #if defined(__aarch64__)
    if (dtb)
      grub_fdt_install((void*)dtb);

    grub_arm_linux_boot((void*)kernel, hdr->kernel_size, (void*)ramdisk,
                        hdr->ramdisk_size, (const char*)hdr->cmdline);
  #else
    return grub_error(GRUB_ERR_NOT_IMPLEMENTED_YET,
                      "only AArch64 is supported for now");
  #endif
  */

  /* Should not return. */
  grub_free(image);
  return GRUB_ERR_NONE;
}

static grub_command_t cmd;

GRUB_MOD_INIT(android_boot) {
  cmd = grub_register_command(
      "android_boot", grub_cmd_android_boot, "android_boot <device or file>",
      "Boot an Android boot.img from file or partition.");
}

GRUB_MOD_FINI(android_boot) {
  grub_unregister_command(cmd);
}
