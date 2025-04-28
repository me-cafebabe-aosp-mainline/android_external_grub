/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Exposes the components of Android boot images (boot.img and
 * vendor_boot.img) as files. See system/tools/mkbootimg in AOSP and
 * include/bootimg/bootimg.h in it for the format.
 *
 * There are two kinds of files: the components that are stored in the image
 * as they are, which are read from the disk, and the ones that are generated
 * from the headers. The names of the files are in the tables near the end of
 * this file. The vendor ramdisk fragments are special, as their names come
 * from the image.
 */

#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/partition.h>
#include <grub/types.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024

#define VENDOR_BOOT_MAGIC "VNDRBOOT"
#define VENDOR_BOOT_MAGIC_SIZE 8
#define VENDOR_BOOT_ARGS_SIZE 2048
#define VENDOR_BOOT_NAME_SIZE 16

/* Sizes of the vendor_boot headers, fixed by the format. */
#define VENDOR_BOOT_HEADER_V3_SIZE 2112
#define VENDOR_BOOT_HEADER_V4_SIZE 2128

/* Page size of the boot image header versions 3 and 4, fixed by the format. */
#define BOOT_HEADER_V3_PAGE_SIZE 4096

#define VENDOR_RAMDISK_TYPE_NONE 0
#define VENDOR_RAMDISK_TYPE_PLATFORM 1
#define VENDOR_RAMDISK_TYPE_RECOVERY 2
#define VENDOR_RAMDISK_TYPE_DLKM 3
#define VENDOR_RAMDISK_TYPE_COUNT 4
#define VENDOR_RAMDISK_NAME_SIZE 32
#define VENDOR_RAMDISK_TABLE_ENTRY_BOARD_ID_SIZE 16

#define BOOTCONFIG_MAGIC "#BOOTCONFIG\n"
#define BOOTCONFIG_MAGIC_LEN 12
#define BOOTCONFIG_ALIGN_SHIFT 2
#define BOOTCONFIG_ALIGN (1 << BOOTCONFIG_ALIGN_SHIFT)
#define BOOTCONFIG_ALIGN_MASK (BOOTCONFIG_ALIGN - 1)

/* Limits to keep corrupted images from making us allocate or loop forever. */
#define MAX_BOOTCONFIG_SIZE (1U << 20)
#define MAX_RAMDISK_FRAGMENTS 1024
#define MAX_PAGE_SIZE (1U << 20)

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

  grub_uint8_t name[BOOT_NAME_SIZE];
  grub_uint8_t cmdline[BOOT_ARGS_SIZE];

  grub_uint32_t id[8];

  grub_uint8_t extra_cmdline[BOOT_EXTRA_ARGS_SIZE];
} GRUB_PACKED;

struct boot_img_hdr_v1
{
  struct boot_img_hdr_v0 v0;
  grub_uint32_t recovery_dtbo_size;
  grub_uint64_t recovery_dtbo_offset;
  grub_uint32_t header_size;
} GRUB_PACKED;

struct boot_img_hdr_v2
{
  struct boot_img_hdr_v1 v1;
  grub_uint32_t dtb_size;
  grub_uint64_t dtb_addr;
} GRUB_PACKED;

struct boot_img_hdr_v3
{
  grub_uint8_t magic[BOOT_MAGIC_SIZE];

  grub_uint32_t kernel_size;
  grub_uint32_t ramdisk_size;

  grub_uint32_t os_version;

  grub_uint32_t header_size;

  grub_uint32_t reserved[4];

  grub_uint32_t header_version;

  grub_uint8_t cmdline[BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE];
} GRUB_PACKED;

struct boot_img_hdr_v4
{
  struct boot_img_hdr_v3 v3;
  grub_uint32_t signature_size;
} GRUB_PACKED;

union boot_img_hdr_union
{
  struct boot_img_hdr_v0 v0;
  struct boot_img_hdr_v1 v1;
  struct boot_img_hdr_v2 v2;
  struct boot_img_hdr_v3 v3;
  struct boot_img_hdr_v4 v4;
} GRUB_PACKED;

struct vendor_boot_img_hdr_v3
{
  grub_uint8_t magic[VENDOR_BOOT_MAGIC_SIZE];

  grub_uint32_t header_version;

  grub_uint32_t page_size;

  grub_uint32_t kernel_addr;
  grub_uint32_t ramdisk_addr;

  grub_uint32_t vendor_ramdisk_size;

  grub_uint8_t cmdline[VENDOR_BOOT_ARGS_SIZE];

  grub_uint32_t tags_addr;
  grub_uint8_t name[VENDOR_BOOT_NAME_SIZE];

  grub_uint32_t header_size;

  grub_uint32_t dtb_size;
  grub_uint64_t dtb_addr;
} GRUB_PACKED;

struct vendor_boot_img_hdr_v4
{
  struct vendor_boot_img_hdr_v3 v3;
  grub_uint32_t vendor_ramdisk_table_size;
  grub_uint32_t vendor_ramdisk_table_entry_num;
  grub_uint32_t vendor_ramdisk_table_entry_size;
  grub_uint32_t bootconfig_size;
} GRUB_PACKED;

union vendor_boot_img_hdr_union
{
  struct vendor_boot_img_hdr_v3 v3;
  struct vendor_boot_img_hdr_v4 v4;
} GRUB_PACKED;

struct vendor_ramdisk_table_entry_v4
{
  grub_uint32_t ramdisk_size;
  grub_uint32_t ramdisk_offset;
  grub_uint32_t ramdisk_type;
  grub_uint8_t ramdisk_name[VENDOR_RAMDISK_NAME_SIZE];

  grub_uint32_t board_id[VENDOR_RAMDISK_TABLE_ENTRY_BOARD_ID_SIZE];
} GRUB_PACKED;

struct vendor_ramdisk_fragment_entry
{
  struct vendor_ramdisk_table_entry_v4 tbl_v4;
  grub_uint64_t offset_from_disk;
  struct vendor_ramdisk_fragment_entry *next;
};

struct bootconfig_tail
{
  grub_uint32_t size;
  grub_uint32_t csum;
  char magic[BOOTCONFIG_MAGIC_LEN];
} GRUB_PACKED;

/* A boot image or vendor_boot image, with its validated header. */
struct android_image
{
  int is_vendor;
  union boot_img_hdr_union hdr;
  union vendor_boot_img_hdr_union vhdr;
};

/* Components that are stored in the images as they are. */
enum boot_img_component
{
  COMPONENT_KERNEL,
  COMPONENT_RAMDISK,
  COMPONENT_SECOND,
  COMPONENT_RECOVERY_DTBO,
  COMPONENT_DTB,
  COMPONENT_SIGNATURE,
};

enum vendor_boot_img_component
{
  VENDOR_COMPONENT_VENDOR_RAMDISK,
  VENDOR_COMPONENT_DTB,
  VENDOR_COMPONENT_BOOTCONFIG_TXT,
  VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE,
};

/* Where the content of an open file comes from. */
struct android_boot_file_data
{
  grub_disk_t disk;
  grub_uint64_t offset_from_disk;
  char *content;
};

/* Helpers */

static inline grub_uint64_t
align_up (grub_uint64_t value, grub_uint32_t alignment)
{
  /* alignment is a power of two, see valid_page_size () */
  return (value + alignment - 1) & ~((grub_uint64_t)alignment - 1);
}

static int
valid_page_size (grub_uint32_t page_size)
{
  return page_size >= 512 && page_size <= MAX_PAGE_SIZE
	 && (page_size & (page_size - 1)) == 0;
}

/* Appends src to *dest, which is freed and set to NULL on failure. */
static int
append (char **dest, const char *src)
{
  grub_size_t len_dest = *dest ? grub_strlen (*dest) : 0;
  grub_size_t len_src = grub_strlen (src);
  char *newbuf = grub_realloc (*dest, len_dest + len_src + 1);

  if (!newbuf)
    {
      grub_free (*dest);
      *dest = NULL;
      return 0;
    }

  grub_memcpy (newbuf + len_dest, src, len_src + 1);
  *dest = newbuf;
  return 1;
}

/*
 * Copies the string in a fixed size field of a header, which does not have to
 * be NUL terminated, and quotes it for the use in single quotes in a GRUB
 * script.
 */
static char *
quoted_field (const grub_uint8_t *field, grub_size_t size)
{
  grub_size_t len = 0, quotes = 0, i;
  char *result, *p;

  while (len < size && field[len])
    {
      if (field[len] == '\'')
	quotes++;
      len++;
    }

  result = grub_malloc (len + quotes * 4 + 1);
  if (!result)
    return NULL;

  p = result;
  for (i = 0; i < len; i++)
    {
      if (field[i] == '\'')
	{
	  /* Close the quote, add one in double quotes, then reopen it */
	  grub_memcpy (p, "'\"'\"'", 5);
	  p += 5;
	}
      else
	*p++ = field[i];
    }
  *p = '\0';
  return result;
}

/* Same as above for strings. */
static char *
quoted_string (const char *str)
{
  return quoted_field ((const grub_uint8_t *)str, grub_strlen (str));
}

/* Image headers */

static grub_err_t
probe_boot_image (grub_device_t device, struct android_image *img)
{
  grub_size_t hdr_size;
  grub_uint32_t ver;

  if (grub_disk_read (device->disk, 0, 0, sizeof (img->hdr), &img->hdr))
    return grub_errno;

  ver = grub_le_to_cpu32 (img->hdr.v0.header_version);
  switch (ver)
    {
    case 0:
      hdr_size = sizeof (struct boot_img_hdr_v0);
      break;
    case 1:
      hdr_size = sizeof (struct boot_img_hdr_v1);
      break;
    case 2:
      hdr_size = sizeof (struct boot_img_hdr_v2);
      break;
    case 3:
      hdr_size = sizeof (struct boot_img_hdr_v3);
      break;
    case 4:
      hdr_size = sizeof (struct boot_img_hdr_v4);
      break;
    default:
      return grub_error (GRUB_ERR_BAD_FS,
			 "unsupported boot image header version %u", ver);
    }

  /* The rest was read from the padding after the header */
  grub_memset ((grub_uint8_t *)&img->hdr + hdr_size, 0,
	       sizeof (img->hdr) - hdr_size);

  if (ver <= 2 && !valid_page_size (grub_le_to_cpu32 (img->hdr.v0.page_size)))
    return grub_error (GRUB_ERR_BAD_FS, "invalid boot image page size");

  return GRUB_ERR_NONE;
}

static grub_err_t
probe_vendor_boot_image (grub_device_t device, struct android_image *img)
{
  grub_size_t hdr_size;
  grub_uint32_t ver, page_size;

  if (grub_disk_read (device->disk, 0, 0, sizeof (img->vhdr), &img->vhdr))
    return grub_errno;

  ver = grub_le_to_cpu32 (img->vhdr.v3.header_version);
  if (ver == 3)
    hdr_size = sizeof (struct vendor_boot_img_hdr_v3);
  else if (ver == 4)
    hdr_size = sizeof (struct vendor_boot_img_hdr_v4);
  else
    return grub_error (GRUB_ERR_BAD_FS,
		       "unsupported vendor_boot image header version %u", ver);

  grub_memset ((grub_uint8_t *)&img->vhdr + hdr_size, 0,
	       sizeof (img->vhdr) - hdr_size);

  page_size = grub_le_to_cpu32 (img->vhdr.v3.page_size);
  if (!valid_page_size (page_size))
    return grub_error (GRUB_ERR_BAD_FS, "invalid vendor_boot image page size");

  if (ver == 4)
    {
      grub_uint32_t num
	  = grub_le_to_cpu32 (img->vhdr.v4.vendor_ramdisk_table_entry_num);
      grub_uint32_t entry_size
	  = grub_le_to_cpu32 (img->vhdr.v4.vendor_ramdisk_table_entry_size);
      grub_uint32_t table_size
	  = grub_le_to_cpu32 (img->vhdr.v4.vendor_ramdisk_table_size);

      if (num > MAX_RAMDISK_FRAGMENTS
	  || (num
	      && entry_size < sizeof (struct vendor_ramdisk_table_entry_v4))
	  || (grub_uint64_t)num * entry_size > table_size)
	return grub_error (GRUB_ERR_BAD_FS, "invalid vendor ramdisk table in "
					    "vendor_boot image");
    }

  return GRUB_ERR_NONE;
}

static grub_err_t
probe_image (grub_device_t device, struct android_image *img)
{
  grub_uint8_t magic[BOOT_MAGIC_SIZE];

  grub_memset (img, 0, sizeof (*img));

  if (!device->disk)
    return grub_error (GRUB_ERR_BAD_FS, "not a disk");

  if (grub_disk_read (device->disk, 0, 0, sizeof (magic), magic))
    return grub_errno;

  if (grub_memcmp (magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0)
    return probe_boot_image (device, img);

  if (grub_memcmp (magic, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_SIZE) == 0)
    {
      img->is_vendor = 1;
      return probe_vendor_boot_image (device, img);
    }

  return grub_error (GRUB_ERR_BAD_FS, "invalid Android boot image");
}

/* Location and size of the components that are stored as they are */

static grub_uint64_t
boot_component_offset (const union boot_img_hdr_union *hdr,
		       enum boot_img_component component)
{
  grub_uint32_t ver = grub_le_to_cpu32 (hdr->v0.header_version);
  grub_uint32_t page_size;
  grub_uint64_t offset;

  if (ver <= 2)
    {
      offset = page_size = grub_le_to_cpu32 (hdr->v0.page_size);

      if (component == COMPONENT_KERNEL)
	return offset;
      offset += align_up (grub_le_to_cpu32 (hdr->v0.kernel_size), page_size);

      if (component == COMPONENT_RAMDISK)
	return offset;
      offset += align_up (grub_le_to_cpu32 (hdr->v0.ramdisk_size), page_size);

      if (component == COMPONENT_SECOND)
	return offset;
      offset += align_up (grub_le_to_cpu32 (hdr->v0.second_size), page_size);

      if (ver >= 1)
	{
	  if (component == COMPONENT_RECOVERY_DTBO)
	    return offset;
	  offset += align_up (grub_le_to_cpu32 (hdr->v1.recovery_dtbo_size),
			      page_size);
	}

      if (ver >= 2 && component == COMPONENT_DTB)
	return offset;
    }
  else
    {
      page_size = BOOT_HEADER_V3_PAGE_SIZE;
      offset = page_size;

      if (component == COMPONENT_KERNEL)
	return offset;
      offset += align_up (grub_le_to_cpu32 (hdr->v3.kernel_size), page_size);

      if (component == COMPONENT_RAMDISK)
	return offset;
      offset += align_up (grub_le_to_cpu32 (hdr->v3.ramdisk_size), page_size);

      if (ver == 4 && component == COMPONENT_SIGNATURE)
	return offset;
    }

  return 0;
}

/* The size is 0 if the image does not have the component. */
static grub_uint64_t
boot_component_size (const union boot_img_hdr_union *hdr,
		     enum boot_img_component component)
{
  grub_uint32_t ver = grub_le_to_cpu32 (hdr->v0.header_version);

  switch (component)
    {
    case COMPONENT_KERNEL:
      return ver <= 2 ? grub_le_to_cpu32 (hdr->v0.kernel_size)
		      : grub_le_to_cpu32 (hdr->v3.kernel_size);
    case COMPONENT_RAMDISK:
      return ver <= 2 ? grub_le_to_cpu32 (hdr->v0.ramdisk_size)
		      : grub_le_to_cpu32 (hdr->v3.ramdisk_size);
    case COMPONENT_SECOND:
      return ver <= 2 ? grub_le_to_cpu32 (hdr->v0.second_size) : 0;
    case COMPONENT_RECOVERY_DTBO:
      return ver >= 1 && ver <= 2
		 ? grub_le_to_cpu32 (hdr->v1.recovery_dtbo_size)
		 : 0;
    case COMPONENT_DTB:
      return ver == 2 ? grub_le_to_cpu32 (hdr->v2.dtb_size) : 0;
    case COMPONENT_SIGNATURE:
      return ver == 4 ? grub_le_to_cpu32 (hdr->v4.signature_size) : 0;
    }
  return 0;
}

static grub_uint64_t
vendor_boot_component_offset (const union vendor_boot_img_hdr_union *vhdr,
			      enum vendor_boot_img_component component)
{
  grub_uint32_t ver = grub_le_to_cpu32 (vhdr->v3.header_version);
  grub_uint32_t page_size = grub_le_to_cpu32 (vhdr->v3.page_size);
  grub_uint64_t offset;

  offset = align_up (ver == 3 ? VENDOR_BOOT_HEADER_V3_SIZE
			      : VENDOR_BOOT_HEADER_V4_SIZE,
		     page_size);

  if (component == VENDOR_COMPONENT_VENDOR_RAMDISK)
    return offset;
  offset
      += align_up (grub_le_to_cpu32 (vhdr->v3.vendor_ramdisk_size), page_size);

  if (component == VENDOR_COMPONENT_DTB)
    return offset;
  offset += align_up (grub_le_to_cpu32 (vhdr->v3.dtb_size), page_size);

  if (ver == 4)
    {
      if (component == VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE)
	return offset;
      offset
	  += align_up (grub_le_to_cpu32 (vhdr->v4.vendor_ramdisk_table_size),
		       page_size);

      if (component == VENDOR_COMPONENT_BOOTCONFIG_TXT)
	return offset;
    }

  return 0;
}

static grub_uint64_t
vendor_boot_component_size (const union vendor_boot_img_hdr_union *vhdr,
			    enum vendor_boot_img_component component)
{
  grub_uint32_t ver = grub_le_to_cpu32 (vhdr->v3.header_version);

  switch (component)
    {
    case VENDOR_COMPONENT_VENDOR_RAMDISK:
      /* Version 4 has the fragments instead */
      return ver == 3 ? grub_le_to_cpu32 (vhdr->v3.vendor_ramdisk_size) : 0;
    case VENDOR_COMPONENT_DTB:
      return grub_le_to_cpu32 (vhdr->v3.dtb_size);
    case VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE:
      return ver == 4 ? grub_le_to_cpu32 (vhdr->v4.vendor_ramdisk_table_size)
		      : 0;
    case VENDOR_COMPONENT_BOOTCONFIG_TXT:
      return ver == 4 ? grub_le_to_cpu32 (vhdr->v4.bootconfig_size) : 0;
    }
  return 0;
}

/* Vendor ramdisk fragments */

static void
free_vendor_ramdisk_fragment_entries (struct vendor_ramdisk_fragment_entry *e)
{
  struct vendor_ramdisk_fragment_entry *next;

  while (e)
    {
      next = e->next;
      grub_free (e);
      e = next;
    }
}

/*
 * Reads the vendor ramdisk table. Returns NULL without an error if there are
 * no fragments.
 */
static struct vendor_ramdisk_fragment_entry *
get_vendor_ramdisk_fragment_entries (
    grub_device_t device, const union vendor_boot_img_hdr_union *vhdr)
{
  struct vendor_ramdisk_fragment_entry *head = NULL, **tail = &head, *entry;
  grub_uint64_t section_offset, table_offset;
  grub_uint32_t num, entry_size, ramdisk_total, i;

  if (grub_le_to_cpu32 (vhdr->v3.header_version) != 4)
    return NULL;

  num = grub_le_to_cpu32 (vhdr->v4.vendor_ramdisk_table_entry_num);
  entry_size = grub_le_to_cpu32 (vhdr->v4.vendor_ramdisk_table_entry_size);
  ramdisk_total = grub_le_to_cpu32 (vhdr->v3.vendor_ramdisk_size);
  if (!ramdisk_total || !num)
    return NULL;

  section_offset
      = vendor_boot_component_offset (vhdr, VENDOR_COMPONENT_VENDOR_RAMDISK);
  table_offset
      = vendor_boot_component_offset (vhdr,
				      VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE);

  for (i = 0; i < num; i++, table_offset += entry_size)
    {
      entry = grub_zalloc (sizeof (*entry));
      if (!entry)
	goto fail;

      if (grub_disk_read (device->disk, 0, table_offset,
			  sizeof (entry->tbl_v4), &entry->tbl_v4))
	{
	  grub_free (entry);
	  goto fail;
	}

      if ((grub_uint64_t)grub_le_to_cpu32 (entry->tbl_v4.ramdisk_offset)
	      + grub_le_to_cpu32 (entry->tbl_v4.ramdisk_size)
	  > ramdisk_total)
	{
	  grub_free (entry);
	  grub_error (GRUB_ERR_BAD_FS, "vendor ramdisk fragment out of range");
	  goto fail;
	}

      entry->offset_from_disk
	  = section_offset + grub_le_to_cpu32 (entry->tbl_v4.ramdisk_offset);
      *tail = entry;
      tail = &entry->next;
    }

  return head;

fail:
  free_vendor_ramdisk_fragment_entries (head);
  return NULL;
}

static char *
get_vendor_ramdisk_fragment_filename (
    const struct vendor_ramdisk_fragment_entry *entry)
{
  const char *type;
  char name[VENDOR_RAMDISK_NAME_SIZE + 1];
  unsigned int i;

  switch (grub_le_to_cpu32 (entry->tbl_v4.ramdisk_type))
    {
    case VENDOR_RAMDISK_TYPE_NONE:
      type = "NONE";
      break;
    case VENDOR_RAMDISK_TYPE_PLATFORM:
      type = "PLATFORM";
      break;
    case VENDOR_RAMDISK_TYPE_RECOVERY:
      type = "RECOVERY";
      break;
    case VENDOR_RAMDISK_TYPE_DLKM:
      type = "DLKM";
      break;
    default:
      type = "UNKNOWN";
      break;
    }

  /*
   * The name is not NUL terminated if it fills the field. Only keep
   * characters that are safe to have in a file name that is used in the
   * generated GRUB scripts.
   */
  for (i = 0; i < VENDOR_RAMDISK_NAME_SIZE && entry->tbl_v4.ramdisk_name[i];
       i++)
    {
      char c = entry->tbl_v4.ramdisk_name[i];

      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	  || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
	name[i] = c;
      else
	name[i] = '_';
    }
  name[i] = '\0';

  if (name[0])
    return grub_xasprintf ("vendor_ramdisk-%s-%s.img", type, name);
  return grub_xasprintf ("vendor_ramdisk-%s.img", type);
}

/* Generated files. They return NULL and set size to 0 if there is nothing. */

typedef char *(*generator_t) (grub_device_t device,
			      const struct android_image *img,
			      grub_off_t *size);

static char *
set_size (char *content, grub_off_t *size)
{
  *size = content ? grub_strlen (content) : 0;
  return content;
}

static char *
generate_boot_info (grub_device_t device __attribute__ ((unused)),
		    const struct android_image *img, grub_off_t *size)
{
  const union boot_img_hdr_union *hdr = &img->hdr;
  char *name, *cmdline, *extra_cmdline, *result = NULL;

  if (grub_le_to_cpu32 (hdr->v0.header_version) <= 2)
    {
      name = quoted_field (hdr->v0.name, BOOT_NAME_SIZE);
      cmdline = quoted_field (hdr->v0.cmdline, BOOT_ARGS_SIZE);
      extra_cmdline
	  = quoted_field (hdr->v0.extra_cmdline, BOOT_EXTRA_ARGS_SIZE);
      if (name && cmdline && extra_cmdline)
	result = grub_xasprintf ("set android_boot_img_name='%s'\n"
				 "set android_boot_img_cmdline='%s %s'\n",
				 name, cmdline, extra_cmdline);
      grub_free (name);
      grub_free (cmdline);
      grub_free (extra_cmdline);
    }
  else
    {
      /* The command line is not split in two fields in the newer versions */
      cmdline = quoted_field (hdr->v3.cmdline, sizeof (hdr->v3.cmdline));
      if (cmdline)
	result
	    = grub_xasprintf ("set android_boot_img_cmdline='%s'\n", cmdline);
      grub_free (cmdline);
    }

  return set_size (result, size);
}

static char *
generate_vendor_boot_info (grub_device_t device __attribute__ ((unused)),
			   const struct android_image *img, grub_off_t *size)
{
  char *name, *cmdline, *result = NULL;

  cmdline = quoted_field (img->vhdr.v3.cmdline, VENDOR_BOOT_ARGS_SIZE);
  name = quoted_field (img->vhdr.v3.name, VENDOR_BOOT_NAME_SIZE);
  if (name && cmdline)
    result = grub_xasprintf ("set android_vendor_boot_img_cmdline='%s'\n"
			     "set android_vendor_boot_img_name='%s'\n",
			     cmdline, name);
  grub_free (name);
  grub_free (cmdline);
  return set_size (result, size);
}

#define VENDOR_RAMDISK_FRAGMENT_INFO_FMT                                      \
  "set android_vendor_boot_img_ramdisk_fragment_none_list='%s'\n"             \
  "set android_vendor_boot_img_ramdisk_fragment_platform_list='%s'\n"         \
  "set android_vendor_boot_img_ramdisk_fragment_recovery_list='%s'\n"         \
  "set android_vendor_boot_img_ramdisk_fragment_dlkm_list='%s'\n"

/* Lists, by type, the fragments as paths that can be passed to initrd. */
static char *
generate_vendor_ramdisk_fragment_info (grub_device_t device,
				       const struct android_image *img,
				       grub_off_t *size)
{
  char *lists[VENDOR_RAMDISK_TYPE_COUNT] = {NULL, NULL, NULL, NULL};
  char *quoted[VENDOR_RAMDISK_TYPE_COUNT] = {NULL, NULL, NULL, NULL};
  char *result = NULL, *part_name, *filename;
  struct vendor_ramdisk_fragment_entry *entries, *entry;
  grub_uint32_t type;
  int i, ok = 1;

  *size = 0;

  entries = get_vendor_ramdisk_fragment_entries (device, &img->vhdr);
  if (!entries)
    return NULL;

  part_name = grub_partition_get_name (device->disk->partition);
  if (!part_name)
    {
      free_vendor_ramdisk_fragment_entries (entries);
      return NULL;
    }

  for (entry = entries; entry && ok; entry = entry->next)
    {
      type = grub_le_to_cpu32 (entry->tbl_v4.ramdisk_type);
      if (type >= VENDOR_RAMDISK_TYPE_COUNT)
	continue;

      filename = get_vendor_ramdisk_fragment_filename (entry);
      if (!filename)
	{
	  ok = 0;
	  break;
	}

      ok = append (&lists[type], "(")
	   && append (&lists[type], device->disk->name);
      if (ok && part_name[0])
	ok = append (&lists[type], ",") && append (&lists[type], part_name);
      ok = ok && append (&lists[type], ")/") && append (&lists[type], filename)
	   && append (&lists[type], " ");
      grub_free (filename);
    }

  for (i = 0; ok && i < VENDOR_RAMDISK_TYPE_COUNT; i++)
    {
      quoted[i] = quoted_string (lists[i] ? lists[i] : "");
      ok = quoted[i] != NULL;
    }

  if (ok)
    result = grub_xasprintf (VENDOR_RAMDISK_FRAGMENT_INFO_FMT, quoted[0],
			     quoted[1], quoted[2], quoted[3]);

  for (i = 0; i < VENDOR_RAMDISK_TYPE_COUNT; i++)
    {
      grub_free (lists[i]);
      grub_free (quoted[i]);
    }
  grub_free (part_name);
  free_vendor_ramdisk_fragment_entries (entries);
  return set_size (result, size);
}

static grub_uint32_t
xbc_calc_checksum (const void *data, grub_uint32_t size)
{
  const unsigned char *p = data;
  grub_uint32_t ret = 0;

  while (size--)
    ret += *p++;

  return ret;
}

/*
 * Returns the bootconfig as the kernel expects to find it at the end of the
 * initramfs: the text, padding, and then its size, checksum and magic.
 */
static char *
generate_bootconfig (grub_device_t device, const struct android_image *img,
		     grub_off_t *size)
{
  char *result;
  const char magic[BOOTCONFIG_MAGIC_LEN] = BOOTCONFIG_MAGIC;
  struct bootconfig_tail *result_tail;
  grub_uint32_t bootconfig_size;
  grub_size_t bootconfig_txt_size, content_size, pad, total_size;

  *size = 0;

  if (grub_le_to_cpu32 (img->vhdr.v3.header_version) != 4)
    return NULL;

  bootconfig_size = grub_le_to_cpu32 (img->vhdr.v4.bootconfig_size);
  if (!bootconfig_size)
    return NULL;
  if (bootconfig_size > MAX_BOOTCONFIG_SIZE)
    {
      grub_error (GRUB_ERR_BAD_FS, "bootconfig is too large");
      return NULL;
    }

  bootconfig_txt_size = bootconfig_size + 1; /* 1 = \0 */
  total_size = bootconfig_txt_size + sizeof (struct bootconfig_tail);
  pad = ((total_size + BOOTCONFIG_ALIGN - 1) & (~BOOTCONFIG_ALIGN_MASK))
	- total_size;
  content_size = total_size + pad;

  result = grub_zalloc (content_size + 1); /* 1 = \0 */
  if (!result)
    return NULL;

  if (grub_disk_read (device->disk, 0,
		      vendor_boot_component_offset (
			  &img->vhdr, VENDOR_COMPONENT_BOOTCONFIG_TXT),
		      bootconfig_size, result))
    {
      grub_free (result);
      return NULL;
    }

  result_tail = (struct bootconfig_tail *)(result + bootconfig_txt_size + pad);
  result_tail->size = grub_cpu_to_le32 (bootconfig_txt_size + pad);
  result_tail->csum = grub_cpu_to_le32 (
      xbc_calc_checksum (result, bootconfig_txt_size + pad));
  grub_memcpy (&result_tail->magic, magic, BOOTCONFIG_MAGIC_LEN);

  *size = content_size;
  return result;
}

/* The files */

struct component
{
  const char *name;
  /* Which one of the components stored in the image, if not generated */
  int id;
  /* NULL if the component is stored in the image */
  generator_t generate;
};

static const struct component boot_components[] = {
    {"kernel", COMPONENT_KERNEL, NULL},
    {"ramdisk.img", COMPONENT_RAMDISK, NULL},
    {"second", COMPONENT_SECOND, NULL},
    {"recovery_dtbo", COMPONENT_RECOVERY_DTBO, NULL},
    {"dtb", COMPONENT_DTB, NULL},
    {"signature", COMPONENT_SIGNATURE, NULL},
    {"boot-info.cfg", 0, generate_boot_info},
};

static const struct component vendor_boot_components[] = {
    {"vendor_ramdisk.img", VENDOR_COMPONENT_VENDOR_RAMDISK, NULL},
    {"dtb", VENDOR_COMPONENT_DTB, NULL},
    {"bootconfig.txt", VENDOR_COMPONENT_BOOTCONFIG_TXT, NULL},
    {"bootconfig.bin", 0, generate_bootconfig},
    {"vendor_boot-info.cfg", 0, generate_vendor_boot_info},
    {"vendor_boot-ramdisk-fragment-info.cfg", 0,
     generate_vendor_ramdisk_fragment_info},
};

struct located_file
{
  grub_uint64_t offset_from_disk; /* if stored in the image */
  char *content;		  /* if generated */
  grub_off_t size;
};

/*
 * Finds where the content of a file is. The size is 0 if the image does not
 * have the file, and grub_errno is set if that could not be told.
 */
static void
locate_component (grub_device_t device, const struct android_image *img,
		  const struct component *component, struct located_file *file)
{
  grub_memset (file, 0, sizeof (*file));

  if (component->generate)
    {
      file->content = component->generate (device, img, &file->size);
      return;
    }

  if (img->is_vendor)
    {
      file->offset_from_disk
	  = vendor_boot_component_offset (&img->vhdr, component->id);
      file->size = vendor_boot_component_size (&img->vhdr, component->id);
    }
  else
    {
      file->offset_from_disk
	  = boot_component_offset (&img->hdr, component->id);
      file->size = boot_component_size (&img->hdr, component->id);
    }
}

static const struct component *
get_components (const struct android_image *img, grub_size_t *count)
{
  if (img->is_vendor)
    {
      *count = ARRAY_SIZE (vendor_boot_components);
      return vendor_boot_components;
    }
  *count = ARRAY_SIZE (boot_components);
  return boot_components;
}

/* Directory listing */

static grub_err_t
android_bootimg_dir (grub_device_t device,
		     const char *path __attribute__ ((unused)),
		     grub_fs_dir_hook_t hook, void *hook_data)
{
  struct grub_dirhook_info info;
  struct android_image img;
  const struct component *components;
  struct located_file file;
  struct vendor_ramdisk_fragment_entry *entries, *entry;
  char *filename;
  grub_size_t count, i;
  int stop = 0;

  if (probe_image (device, &img))
    return grub_errno;

  grub_memset (&info, 0, sizeof (info));
  components = get_components (&img, &count);

  for (i = 0; i < count; i++)
    {
      locate_component (device, &img, &components[i], &file);
      grub_free (file.content);
      if (grub_errno)
	return grub_errno;
      if (file.size && hook (components[i].name, &info, hook_data))
	return GRUB_ERR_NONE;
    }

  if (!img.is_vendor)
    return GRUB_ERR_NONE;

  entries = get_vendor_ramdisk_fragment_entries (device, &img.vhdr);
  for (entry = entries; entry && !stop; entry = entry->next)
    {
      filename = get_vendor_ramdisk_fragment_filename (entry);
      if (!filename)
	break;
      stop = hook (filename, &info, hook_data);
      grub_free (filename);
    }
  free_vendor_ramdisk_fragment_entries (entries);

  return grub_errno;
}

/* Open a file */

static grub_err_t
open_fragment (grub_file_t file, const struct android_image *img,
	       const char *name, struct android_boot_file_data *data)
{
  struct vendor_ramdisk_fragment_entry *entries, *entry;
  char *filename;

  entries = get_vendor_ramdisk_fragment_entries (file->device, &img->vhdr);
  for (entry = entries; entry; entry = entry->next)
    {
      filename = get_vendor_ramdisk_fragment_filename (entry);
      if (!filename)
	break;
      if (!grub_strcmp (name, filename))
	{
	  data->offset_from_disk = entry->offset_from_disk;
	  file->size = grub_le_to_cpu32 (entry->tbl_v4.ramdisk_size);
	}
      grub_free (filename);
      if (file->size)
	break;
    }
  free_vendor_ramdisk_fragment_entries (entries);

  return grub_errno;
}

static grub_err_t
android_bootimg_open (grub_file_t file, const char *path)
{
  struct android_boot_file_data *data;
  struct android_image img;
  const struct component *components;
  struct located_file located;
  grub_size_t count, i;
  grub_err_t err;

  file->size = 0;

  if (path[0] != '/')
    return grub_error (GRUB_ERR_BAD_FILENAME, N_ ("invalid file name `%s'"),
		       path);
  path++;

  err = probe_image (file->device, &img);
  if (err)
    return err;

  data = grub_zalloc (sizeof (*data));
  if (!data)
    return grub_errno;

  components = get_components (&img, &count);
  for (i = 0; i < count; i++)
    {
      if (grub_strcmp (path, components[i].name))
	continue;

      locate_component (file->device, &img, &components[i], &located);
      data->offset_from_disk = located.offset_from_disk;
      data->content = located.content;
      file->size = located.size;
      break;
    }

  if (i == count && img.is_vendor)
    open_fragment (file, &img, path, data);

  if (grub_errno)
    err = grub_errno;
  else if (!file->size)
    err = grub_error (GRUB_ERR_FILE_NOT_FOUND, N_ ("file `%s' not found"),
		      path);
  if (err)
    {
      file->size = 0;
      grub_free (data->content);
      grub_free (data);
      return err;
    }

  data->disk = file->device->disk;
  file->data = data;
  file->not_easily_seekable = 0;
  file->offset = 0;
  return GRUB_ERR_NONE;
}

/* Read */

static grub_ssize_t
android_bootimg_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct android_boot_file_data *data = file->data;
  grub_ssize_t ret;

  if (data->content)
    {
      grub_memcpy (buf, data->content + file->offset, len);
      return len;
    }

  data->disk->read_hook = file->read_hook;
  data->disk->read_hook_data = file->read_hook_data;
  ret = grub_disk_read (data->disk, 0, data->offset_from_disk + file->offset,
			len, buf)
	    ? -1
	    : (grub_ssize_t)len;
  data->disk->read_hook = 0;

  return ret;
}

/* Close */

static grub_err_t
android_bootimg_close (grub_file_t file)
{
  struct android_boot_file_data *data = file->data;

  grub_free (data->content);
  grub_free (data);
  return GRUB_ERR_NONE;
}

/* Register FS */

static struct grub_fs android_bootimg_fs = {
    .name = "android_bootimg_fs",
    .fs_dir = android_bootimg_dir,
    .fs_open = android_bootimg_open,
    .fs_read = android_bootimg_read,
    .fs_close = android_bootimg_close,
#ifdef GRUB_UTIL
    .reserved_first_sector = 0,
    .blocklist_install = 0,
#endif
};

GRUB_MOD_INIT (android_bootimg_fs)
{
  grub_fs_register (&android_bootimg_fs);
}

GRUB_MOD_FINI (android_bootimg_fs)
{
  grub_fs_unregister (&android_bootimg_fs);
}
