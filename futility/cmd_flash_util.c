/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>

#include "flash_helpers.h"
#include "fmap.h"
#include "futility.h"
#include "updater.h"

#ifdef USE_FLASHROM

static int get_ro_range(struct updater_config *cfg,
			uint32_t *start, uint32_t *len)
{
	int ret = 0;

	/* Read fmap */
	const char *const regions[] = {FMAP_RO_FMAP, NULL};
	if (flashrom_read_image(&cfg->image_current, regions,
				cfg->verbosity + 1))
		return -1;

	FmapAreaHeader *wp_ro = NULL;
	uint8_t *r = fmap_find_by_name(cfg->image_current.data,
				       cfg->image_current.size,
				       NULL, FMAP_RO, &wp_ro);
	if (!r || !wp_ro) {
		ERROR("Could not find WP_RO in the FMAP\n");
		ret = -1;
		goto err;
	}

	*start = wp_ro->area_offset;
	*len = wp_ro->area_size;

err:
	free(cfg->image_current.data);
	cfg->image_current.data = NULL;
	cfg->image_current.size = 0;

	return ret;
}

static int print_flash_size(struct updater_config *cfg)
{
	uint32_t flash_size;
	if (flashrom_get_size(cfg->image.programmer, &flash_size,
			      cfg->verbosity + 1)) {
		ERROR("%s failed.\n", __func__);
		return -1;
	}

	printf("Flash size: %#010x\n", flash_size);
	return 0;
}

static int print_flash_info(struct updater_config *cfg)
{
	char *vendor;
	char *name;
	uint32_t vid;
	uint32_t pid;
	uint32_t flash_size;
	if (flashrom_get_info(cfg->image.programmer,
				&vendor, &name,
				&vid, &pid,
				&flash_size,
			      cfg->verbosity + 1)) {
		ERROR("%s failed.\n", __func__);
		return -1;
	}

	printf("Flash vendor: %s\n", vendor);
	free(vendor);
	printf("Flash name: %s\n", name);
	free(name);
	const uint64_t vidpid = (uint64_t) vid << 32 | pid;
	printf("Flash vid-pid: 0x%" PRIx64 "\n", vidpid);
	printf("Flash size: %#010x\n", flash_size);

	/* Get WP_RO region start and length from image */
	uint32_t ro_start, ro_len;
	if (get_ro_range(cfg, &ro_start, &ro_len))
		return -1;
	printf("Expected WP SR configuration by FW image: (start = %#010x, length = %#010x)\n", ro_start, ro_len);

	return 0;
}

static int print_wp_status(struct updater_config *cfg, bool ignore_hw)
{
	/* Get WP_RO region start and length from image */
	uint32_t ro_start, ro_len;
	if (get_ro_range(cfg, &ro_start, &ro_len))
		return -1;

	/* Get current WP region and mode from SPI flash */
	bool wp_mode;
	uint32_t wp_start, wp_len;
	if (flashrom_get_wp(cfg->image.programmer, &wp_mode,
			    &wp_start, &wp_len, cfg->verbosity + 1)) {
		ERROR("Failed to get WP status\n");
		return -1;
	}

	/* A 1 implies HWWP is enabled. */
	int hwwp = ignore_hw ? 1 : dut_get_property(DUT_PROP_WP_HW, cfg);

	if (!hwwp || (!wp_mode && wp_start == 0 && wp_len == 0)) {
		printf("WP status: disabled\n");
	} else if (wp_mode && wp_start == ro_start && wp_len == ro_len) {
		printf("WP status: enabled\n");
	} else {
		printf("WP status: misconfigured (srp = %d, start = %#010x, length = %#010x)\n",
		     wp_mode, wp_start, wp_len);
	}

	return 0;
}


static int set_flash_wp(struct updater_config *cfg, bool enable)
{
	uint32_t wp_start = 0;
	uint32_t wp_len = 0;

	if (enable) {
		/* Use the WP_RO region as the protection range */
		if (get_ro_range(cfg, &wp_start, &wp_len))
			return -1;
	}

	if (flashrom_set_wp(cfg->image.programmer, enable,
			    wp_start, wp_len, cfg->verbosity + 1)) {
		ERROR("Failed to modify WP configuration.\n");
		return -1;
	}

	printf("%s WP\n", enable ? "Enabled" : "Disabled");

	return 0;
}

/* Command line options */
static struct option const long_opts[] = {
	SHARED_FLASH_ARGS_LONGOPTS
	/* name  has_arg *flag val */
	{"help", 0, NULL, 'h'},
	{"wp-status", 0, NULL, 's'},
	{"ignore-hw", 0, NULL, 'o'},
	{"wp-enable", 0, NULL, 'e'},
	{"wp-disable", 0, NULL, 'd'},
	{"flash-info", 0, NULL, 'i'},
	{"flash-size", 0, NULL, 'z'},
	{NULL, 0, NULL, 0},
};

static const char *const short_opts = "h" SHARED_FLASH_ARGS_SHORTOPTS;

static void print_help(int argc, char *argv[])
{
	printf("\n"
	       "Allows for the management of AP SPI flash configuration.\n"
	       "\n"
	       "Usage:  " MYNAME " %s [OPTIONS] \n"
	       "\n"
	       "    --wp-status          \tGet the current flash WP state.\n"
	       "    --wp-status          \tGet the current HW and SW WP state.\n"
	       "        [--ignore-hw]    \tGet SW WP state only.\n"
	       "    --wp-enable          \tEnable protection for the RO image section.\n"
	       "    --wp-disable         \tDisable all write protection.\n"
	       "    --flash-size         \tGet flash size.\n"
	       "    --flash-info         \tGet flash info.\n"
	       "\n"
	       SHARED_FLASH_ARGS_HELP,
	       argv[0]);
}

static int do_flash(int argc, char *argv[])
{
	struct updater_config *cfg = NULL;
	struct updater_config_arguments args = {0};
	bool enable_wp = false;
	bool disable_wp = false;
	bool get_wp_status = false;
	bool ignore_hw_wp = false;
	bool get_size = false;
	bool get_info = false;
	int ret = 0;

	opterr = 0;
	int i;
	while ((i = getopt_long(argc, argv, short_opts, long_opts, 0)) != -1) {
		if (handle_flash_argument(&args, i, optarg))
			continue;
		switch (i) {
		case 'h':
			print_help(argc, argv);
			return 0;
		case 's':
			get_wp_status = true;
			break;
		case 'o':
			ignore_hw_wp = true;
			break;
		case 'e':
			enable_wp = true;
			break;
		case 'd':
			disable_wp = true;
			break;
		case 'i':
			get_info = true;
			break;
		case 'z':
			get_size = true;
			break;
		case 'v':
			args.verbosity++;
			break;
		case '?':
			if (optopt)
				ERROR("Unrecognized option: -%c\n", optopt);
			else if (argv[optind - 1])
				ERROR("Unrecognized option (possibly '%s')\n",
				      argv[optind - 1]);
			else
				ERROR("Unrecognized option.\n");
			return 1;
		default:
			ERROR("Failed parsing options.\n");
			return 1;
		}
	}
	if (optind < argc) {
		ERROR("Unexpected arguments.\n");
		return 1;
	}

	if (!get_size && !get_info && !enable_wp && !disable_wp && !get_wp_status) {
		print_help(argc, argv);
		return 0;
	}

	if (!get_wp_status && ignore_hw_wp) {
		ERROR("--ignore-hw must be used with --wp-status.\n");
		return 1;
	}

	if (enable_wp && disable_wp) {
		ERROR("--wp-enable and --wp-disable cannot be used together.\n");
		return 1;
	}

	if (setup_flash(&cfg, &args, NULL)) {
		ERROR("While preparing flash\n");
		return 1;
	}

	if (get_info)
		ret = print_flash_info(cfg);

	if (!ret && get_size)
		ret = print_flash_size(cfg);

	if (!ret && enable_wp)
		ret = set_flash_wp(cfg, true);

	if (!ret && disable_wp)
		ret = set_flash_wp(cfg, false);

	if (!ret && get_wp_status)
		ret = print_wp_status(cfg, ignore_hw_wp);

	teardown_flash(cfg);
	return ret;
}
#define CMD_HELP_STR "Manage AP SPI flash properties and writeprotect configuration"

#else /* USE_FLASHROM */

static int do_flash(int argc, char *argv[])
{
	FATAL(MYNAME " was built without flashrom support, `flash` command unavailable!\n");
	return -1;
}
#define CMD_HELP_STR "Manage AP SPI flash properties and writeprotect configuration (unavailable in this build)"

#endif /* !USE_FLASHROM */

DECLARE_FUTIL_COMMAND(flash, do_flash, VBOOT_VERSION_ALL, CMD_HELP_STR);
