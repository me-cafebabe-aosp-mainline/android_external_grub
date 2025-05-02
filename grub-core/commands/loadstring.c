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

#include <grub/command.h>
#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* Environment variables are not meant to hold much more than this. */
#define LOADSTRING_MAX_SIZE 65536

static grub_command_t cmd;

static grub_err_t
grub_cmd_loadstring (grub_command_t cmdd __attribute__ ((unused)), int argc,
		     char **argv)
{
  char *buf = NULL, *device_name = NULL;
  grub_device_t dev = NULL;
  grub_off_t offset = 0;
  grub_size_t size;
  grub_err_t err;

  if (argc < 3 || argc > 4)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       N_ ("Usage: loadstring DEVICE VARIABLE SIZE [OFFSET]"));

  size = grub_strtoul (argv[2], NULL, 0);
  if (grub_errno != GRUB_ERR_NONE)
    return grub_errno;
  if (size == 0 || size > LOADSTRING_MAX_SIZE)
    return grub_error (GRUB_ERR_BAD_NUMBER, N_ ("invalid size"));

  if (argc == 4)
    {
      offset = grub_strtoull (argv[3], NULL, 0);
      if (grub_errno != GRUB_ERR_NONE)
	return grub_errno;
    }

  device_name = grub_file_get_device_name (argv[0]);
  if (grub_errno != GRUB_ERR_NONE)
    return grub_errno;

  dev = grub_device_open (device_name);
  grub_free (device_name);
  if (dev == NULL)
    return grub_errno;

  if (dev->disk == NULL)
    {
      err = grub_error (GRUB_ERR_BAD_DEVICE, N_ ("%s is not a disk"), argv[0]);
      goto out;
    }

  buf = grub_zalloc (size + 1);
  if (buf == NULL)
    {
      err = grub_errno;
      goto out;
    }

  err = grub_disk_read (dev->disk, 0, offset, size, buf);
  if (err != GRUB_ERR_NONE)
    goto out;

  err = grub_env_set (argv[1], buf);

out:
  grub_free (buf);
  if (dev != NULL)
    grub_device_close (dev);
  return err;
}

GRUB_MOD_INIT (loadstring)
{
  cmd = grub_register_command ("loadstring", grub_cmd_loadstring,
			       N_ ("DEVICE VARIABLE SIZE [OFFSET]"),
			       N_ ("Load a string from a partition into a "
				   "variable."));
}

GRUB_MOD_FINI (loadstring)
{
  grub_unregister_command (cmd);
}
