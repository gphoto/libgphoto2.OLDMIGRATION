/***************************************************************************
 *
 * library.c
 *
 *   Canon Camera library for the gphoto project,
 *   (c) 1999 Wolfgang G. Reissnegger
 *   Developed for the Canon PowerShot A50
 *   Additions for PowerShot A5 by Ole W. Saastad
 *   (c) 2000: Other additions  by Edouard Lafargue, Philippe Marzouk
 *
 * This file contains all the "glue code" required to use the canon
 * driver with libgphoto2.
 *
 * $Id$
 ****************************************************************************/


/****************************************************************************
 *
 * include files
 *
 ****************************************************************************/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
//#include <ctype.h>

#include <gphoto2.h>

#ifdef ENABLE_NLS
#  include <libintl.h>
#  undef _
#  define _(String) dgettext (PACKAGE, String)
#  ifdef gettext_noop
#    define N_(String) gettext_noop (String)
#  else
#    define N_(String) (String)
#  endif
#else
#  define textdomain(String) (String)
#  define gettext(String) (String)
#  define dgettext(Domain,Message) (Message)
#  define dcgettext(Domain,Message,Type) (Message)
#  define bindtextdomain(Domain,Directory) (Domain)
#  define _(String) (String)
#  define N_(String) (String)
#endif

#include "util.h"
#include "library.h"
#include "canon.h"
#include "serial.h"
#include "usb.h"

#ifndef HAVE_SNPRINTF
#warning You do not seem to have a snprintf() function. Using sprintf instead.
#warning Note that this leads to SECURITY RISKS!
#define snprintf(buf,size,format,arg) sprintf(buf,format,arg)
#endif

/**
 * models:
 *
 * Contains list of all camera models currently supported with their
 * respective USD IDs and a flag denoting RS232 serial support.
 **/

static struct
{
	char *name;
	unsigned short idVendor;
	unsigned short idProduct;
	char serial;
}
models[] =
{
	{
	"Canon PowerShot A5", 0, 0, 1}
	, {
	"Canon PowerShot A5 Zoom", 0, 0, 1}
	, {
	"Canon PowerShot A50", 0, 0, 1}
	, {
	"Canon PowerShot Pro70", 0, 0, 1}
	, {
	"Canon PowerShot S10", 0x04A9, 0x3041, 1}
	, {
	"Canon PowerShot S20", 0x04A9, 0x3043, 1}
	, {
	"Canon EOS D30", 0x04A9, 0x3044, 0}
	, {
	"Canon PowerShot S100", 0x04A9, 0x3045, 0}
	, {
	"Canon IXY DIGITAL", 0x04A9, 0x3046, 0}
	, {
	"Canon Digital IXUS", 0x04A9, 0x3047, 0}
	, {
	"Canon PowerShot G1", 0x04A9, 0x3048, 1}
	, {
	"Canon PowerShot Pro90 IS", 0x04A9, 0x3049, 1}
	, {
	"Canon IXY DIGITAL 300", 0x04A9, 0x304B, 0}
	, {
	"Canon PowerShot S300", 0x04A9, 0x304C, 0}
	, {
	"Canon Digital IXUS 300", 0x04A9, 0x304D, 0}
	, {
	"Canon PowerShot A20", 0x04A9, 0x304E, 0}
	, {
	"Canon PowerShot A10", 0x04A9, 0x304F, 0}
	, {
	"Canon PowerShot S110", 0x04A9, 0x3051, 0}
	, {
	"Canon DIGITAL IXUS v", 0x04A9, 0x3052, 0}
	, {
	"Canon PowerShot G2", 0x04A9, 0x3055, 0}
	, {
	"Canon PowerShot S40", 0x4A9, 0x3056, 0}
	, {
	"Canon PowerShot S30", 0x4A9, 0x3057, 0}
	, {
	NULL, 0, 0, 0}
};

int
camera_id (CameraText *id)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_id()");

	strcpy (id->text, "canon");

	return GP_OK;
}

static int
camera_manual (Camera *camera, CameraText *manual)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_manual()");

	strcpy (manual->text, _("For the A50, 115200 may not be faster than 57600\n"
				"Folders are NOT supported\n"
				"if you experience a lot of transmissions errors,"
				" try to have you computer as idle as possible (ie: no disk activity)"));

	return GP_OK;
}

int
camera_abilities (CameraAbilitiesList *list)
{
	int i;
	CameraAbilities a;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_abilities()");

	for (i = 0; models[i].name; i++) {
		a.status = GP_DRIVER_STATUS_PRODUCTION;
		strcpy (a.model, models[i].name);
		a.port = 0;
		if (models[i].idProduct) {
			a.port |= GP_PORT_USB;
			a.usb_vendor = models[i].idVendor;
			a.usb_product = models[i].idProduct;
		}
		if (models[i].serial) {
			a.port |= GP_PORT_SERIAL;
			a.speed[0] = 9600;
			a.speed[1] = 19200;
			a.speed[2] = 38400;
			a.speed[3] = 57600;
			a.speed[4] = 115200;
			a.speed[5] = 0;
		}
		a.operations = GP_OPERATION_CONFIG;
		a.folder_operations = GP_FOLDER_OPERATION_PUT_FILE |
			GP_FOLDER_OPERATION_MAKE_DIR | GP_FOLDER_OPERATION_REMOVE_DIR;
		a.file_operations = GP_FILE_OPERATION_DELETE | GP_FILE_OPERATION_PREVIEW;
		gp_abilities_list_append (list, a);
	}

	return GP_OK;
}

void
clear_readiness (Camera *camera)
{
	GP_DEBUG("clear_readiness()");
	camera->pl->cached_ready = 0;
}

static int
check_readiness (Camera *camera)
{
	GP_DEBUG("check_readiness()");
	if (camera->pl->cached_ready)
		return 1;
	if (canon_int_ready (camera) == GP_OK) {
		camera->pl->cached_ready = 1;
		return 1;
	}
	gp_camera_status (camera, _("Camera unavailable"));
	return 0;
}

static void
canon_int_switch_camera_off (Camera *camera)
{
	GP_DEBUG ("switch_camera_off()");
	gp_camera_status (camera, _("Switching Camera Off"));

	switch (camera->port->type) {
		case GP_PORT_SERIAL:
			canon_serial_off (camera);
			break;
		case GP_PORT_USB:
		case GP_PORT_NONE:
		default:
			// FIXME: What about USB?
			GP_DEBUG ("Unknown camera->port->type in canon_int_switch_camera_off()");
	}
	clear_readiness (camera);
}

static int
camera_exit (Camera *camera)
{
	if (camera->pl) {
		canon_int_switch_camera_off (camera);
		free (camera->pl);
		camera->pl = NULL;
	}

	return GP_OK;
}

static int
canon_get_batt_status (Camera *camera, int *pwr_status, int *pwr_source)
{
	GP_DEBUG ("canon_get_batt_status() called");

	if (!check_readiness (camera))
		return -1;

	return canon_int_get_battery (camera, pwr_status, pwr_source);
}

/* This function is only used by A5 */

#ifdef OBSOLETE
static int
recurse (Camera *camera, const char *name)
{
	struct canon_dir *dir, *walk;
	char buffer[300];	/* longest path, etc. */
	int count, curr, res;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "recurse() name '%s'", name);

	res = canon_int_list_directory (camera, &dir, name);
	if (res != GP_OK)
		return res;

	if (dir == NULL)
		return 1;

	count = 0;
	for (walk = dir; walk->name; walk++)
		if (walk->size && (is_image (walk->name) || is_movie (walk->name)))
			count++;
	camera->pl->cached_paths =
		realloc (camera->pl->cached_paths,
			 sizeof (char *) * (camera->pl->cached_images + count + 1));
	memset (camera->pl->cached_paths + camera->pl->cached_images + 1, 0,
		sizeof (char *) * count);
	if (!camera->pl->cached_paths) {
		perror ("realloc");
		return 0;
	}
	curr = camera->pl->cached_images;
	camera->pl->cached_images += count;
	for (walk = dir; walk->name; walk++) {
		sprintf (buffer, "%s\\%s", name, walk->name);
		if (!walk->size) {
			if (!recurse (camera, buffer))
				return 0;
		} else {
			if ((!is_image (walk->name)) && (!is_movie (walk->name)))
				continue;
			curr++;
			camera->pl->cached_paths[curr] = strdup (buffer);
			if (!camera->pl->cached_paths[curr]) {
				perror ("strdup");
				return 0;
			}
		}
	}
	free (dir);
	return 1;
}
#endif


/* This function is only used by A50 */

#ifdef OBSOLETE
static struct canon_dir *
dir_tree (Camera *camera, const char *path)
{
	struct canon_dir *dir, *walk;
	char buffer[300];	/* longest path, etc. */
	int res;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "dir_tree() path '%s'", path);

	res = canon_int_list_directory (camera, &dir, path);
	if (res != GP_OK)
		return NULL;

	if (dir == NULL)
		return NULL;	/* assume it's empty @@@ */
	for (walk = dir; walk->name; walk++) {
		if (walk->is_file) {
			if (is_image (walk->name) || is_movie (walk->name)
			    || is_thumbnail (walk->name))
				camera->pl->cached_images++;
		} else {
			sprintf (buffer, "%s\\%s", path, walk->name);
			walk->user = dir_tree (camera, buffer);
		}
	}
	qsort (dir, walk - dir, sizeof (*dir), comp_dir);
	return dir;
}
#endif

static void
clear_dir_cache (Camera *camera)
{
	GP_DEBUG("clear_dir_cache() OBSOLETE DUMMY");
}


/* A5 only: sort THB_ and AUT_ into their proper arrangement. */
static int
compare_a5_paths (const void *p1, const void *p2)
{
	const char *s1 = *((const char **) p1);
	const char *s2 = *((const char **) p2);
	const char *ptr, *base1, *base2;
	int n1 = 0, n2 = 0;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "compare_a5_paths()");

	gp_debug_printf (GP_DEBUG_LOW, "canon", _("Comparing %s to %s\n"), s1, s2);

	ptr = strrchr (s1, '_');
	if (ptr)
		n1 = strtol (ptr + 1, 0, 10);
	ptr = strrchr (s2, '_');
	if (ptr)
		n2 = strtol (ptr + 1, 0, 10);

	gp_debug_printf (GP_DEBUG_LOW, "canon", _("Numbers are %d and %d\n"), n1, n2);

	if (n1 < n2)
		return -1;
	else if (n1 > n2)
		return 1;
	else {
		base1 = strrchr (s1, '\\');
		base2 = strrchr (s2, '\\');
		gp_debug_printf (GP_DEBUG_LOW, "canon", _("Base 1 is %s and base 2 is %s\n"),
				 base1, base2);
		return strcmp (base1, base2);
	}
}

static int
update_dir_cache (Camera *camera)
{
	GP_DEBUG("update_dir_cache() OBSOLETE DUMMY");
	return GP_OK;
}
#ifdef OBSOLETE
	int i;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "update_dir_cache() "
			 "cached_dir = %i", camera->pl->cached_dir);

	if (camera->pl->cached_dir)
		return 1;
	if (!update_disk_cache (camera))
		return 0;
	if (!check_readiness (camera))
		return 0;
	camera->pl->cached_images = 0;
	switch (camera->pl->model) {
		case CANON_PS_A5:
		case CANON_PS_A5_ZOOM:
			/* FIXME: This must be worked into canon_int_list_directory() */
			if (recurse (camera, camera->pl->cached_drive)) {
				gp_debug_printf (GP_DEBUG_LOW, "canon", _("Before sort:\n"));
				for (i = 1; i < camera->pl->cached_images; i++) {
					gp_debug_printf (GP_DEBUG_LOW, "canon", "%d: %s\n", i,
							 camera->pl->cached_paths[i]);
				}
				qsort (camera->pl->cached_paths + 1, camera->pl->cached_images,
				       sizeof (char *), compare_a5_paths);
				gp_debug_printf (GP_DEBUG_LOW, "canon", _("After sort:\n"));
				for (i = 1; i < camera->pl->cached_images; i++) {
					printf ("%d: %s\n", i, camera->pl->cached_paths[i]);
				}
				camera->pl->cached_dir = 1;
				return 1;
			}
			clear_dir_cache (camera);
			return 0;
			break;

		default:	/* A50 or S10 or other */
			camera->pl->cached_tree = dir_tree (camera, camera->pl->cached_drive);
			if (!camera->pl->cached_tree)
				return 0;
			camera->pl->cached_dir = 1;
			return 1;
			break;
	}
}
#endif

static int
file_list_func (CameraFilesystem *fs, const char *folder, CameraList *list, void *data)
{
	Camera *camera = data;

	GP_DEBUG("file_list()");

	return canon_int_list_directory (camera, folder, list, CANON_LIST_FILES);
}

static int
folder_list_func (CameraFilesystem *fs, const char *folder, CameraList *list, void *data)
{
	Camera *camera = data;

	GP_DEBUG("folder_list()");

	return canon_int_list_directory (camera, folder, list, CANON_LIST_FOLDERS);
}

/****************************************************************************
 *
 * gphoto library interface calls
 *
 ****************************************************************************/

static int
canon_get_picture (Camera *camera, char *filename, char *path, int thumbnail,
		   unsigned char **data, int *size)
{
	unsigned char attribs;
	char complete_filename[300];
	int res;

	GP_DEBUG ("canon_get_picture()");

	if (!check_readiness (camera)) {
		return GP_ERROR;
	}
	switch (camera->pl->model) {
		case CANON_PS_A5:
		case CANON_PS_A5_ZOOM:
#if 0
			picture_number = picture_number * 2 - 1;
			if (thumbnail)
				picture_number += 1;
			GP_DEBUG ("Picture number %i", picture_number);

			if (!picture_number || picture_number > cached_images) {
				gp_camera_status (camera, _("Invalid index"));
				return GP_ERROR;
			}
			gp_camera_status (camera, cached_paths[picture_number]);
			if (!check_readiness (camera)) {
				return GP_ERROR;
			}
			res = canon_int_get_file (cached_paths[picture_number], size);
			if (res != GP_OK)
				return res;
#else
			GP_DEBUG ("canon_get_picture: downloading "
				  "pictures disabled for cameras: PowerShot A5, "
				  "PowerShot A5 ZOOM");

			return GP_ERROR_NOT_SUPPORTED;
#endif /* 0 */
			break;
		default:
			/* For A50 or others */
			/* clear_readiness(); */
			if (!update_dir_cache (camera)) {
				gp_camera_status (camera,
						  _("Could not obtain directory listing"));
				return GP_ERROR;
			}

			/* strip trailing backslash on path, if any */
			if (path[strlen (path) - 1] == '\\')
				path[strlen (path) - 1] = 0;

			snprintf (complete_filename, sizeof (complete_filename), "%s\\%s",
				  path, filename);

			GP_DEBUG ("canon_get_picture: path='%s', file='%s'\n\tcomplete filename='%s'\n", path, filename, complete_filename);
			attribs = 0;
			if (!check_readiness (camera)) {
				return GP_ERROR;
			}
			if (thumbnail) {
				/* The thumbnail of a movie in on a file called MVI_XXXX.THM
				 * we replace .AVI by .THM to download the thumbnail (jpeg format)
				 */
				if (is_movie (filename)) {
					strcpy (complete_filename +
						(strlen (complete_filename) - 3), "THM");
					/* XXX check that this works */
					GP_DEBUG ("canon_get_picture: movie thumbnail: %s\n",
						  complete_filename);
					return canon_int_get_file (camera, complete_filename,
								   data, size);
				} else {
					*data = canon_int_get_thumbnail (camera,
									 complete_filename,
									 size);
					if (*data)
						return GP_OK;
					else {
						GP_DEBUG ("canon_get_picture: ",
							  "canon_int_get_thumbnail() '%s' %d failed!",
							  complete_filename, size);
						return GP_ERROR;
					}
				}
			} else {
				res = canon_int_get_file (camera, complete_filename, data,
							  size);
				if (res != GP_OK) {
					GP_DEBUG ("canon_get_picture: "
						  "canon_int_get_file() failed! returned %i",
						  res);
					return res;
				}

				GP_DEBUG ("canon_get_picture: We now have to set the \"downloaded\" " "flag on the picture");
				/* XXX this is bogus, attrib is not fetched - it is always set to 0 above */
				GP_DEBUG ("canon_get_picture: The old file attributes were: %#x\n", attribs);
				attribs &= ~CANON_ATTR_DOWNLOADED;
				res = canon_int_set_file_attributes (camera, filename, path,
								     attribs);
				if (res != GP_OK) {
					/* warn but continue since we allready have the downloaded picture */
					GP_DEBUG ("canon_get_picture: "
						  "WARNING: canon_int_set_file_attributes on "
						  "'%s' '%s' to 0x%x failed! returned %d.",
						  path, filename, attribs, res);
				}
			}
			return GP_OK;
			break;
	}
	/* NOT REACHED */
	return GP_ERROR;
}

/**
 * _get_file_path:
 * @camera: a #Camera to work with
 * @canon_dir: seems to be a reference to the current
 * @filename: seems to be the file name to look for
 * @path: returns (?!) the full path to the file given by @filename
 *
 * _get_file_path seems to find a given filename anywhere in the
 * directory tree and returns the complete path to the file in @path.
 *
 * _get_file_path is an internal function and is only called from #get_file_path.
 *
 * This function recursively looks for @filename in @tree and returns a concatenated
 * @path if the filename is found somewhere in @tree.
 *
 * Return value: a gphoto2 error code.
 **/
#ifdef OBSOLETE
int
_get_file_path (Camera *camera, struct canon_dir *tree, const char *filename, char *path,
		int recursively_entered)
{
	static char newpath[1024];
	char *temp_ch;

	GP_DEBUG ("_get_file_path() called: filename '%s' path '%s' recurs %i", filename, path,
		  recursively_entered);

	if (tree == NULL)
		return GP_ERROR;

	/* initiate newpath if we are not recursively entered */
	if (!recursively_entered)
		strcpy (newpath, path);

	/* add trailing backslash if missing */
	if (newpath[strlen (newpath) - 1] != '\\')
		strcat (newpath, "\\");

	while (tree->name) {
		if (strcmp (tree->name, filename) == 0 &&
		    (is_image (tree->name) || is_movie (tree->name) ||
		     is_thumbnail (tree->name))) {
			/* we've got a match! */
			GP_DEBUG ("_get_file_path() returns with "
				  "filename '%s' path '%s'", filename, path);
			if (!recursively_entered)
				strcpy (path, newpath);
			return GP_OK;
		}
		if (!tree->is_file) {
			/* remember where newpath ends */
			temp_ch = strrchr (newpath, 0);

			/* not a file, must be a directory. append name to newpath */
			strncat (newpath, tree->name, sizeof (newpath) - strlen (newpath));
			newpath[sizeof (newpath) - 1] = 0;

			/* enter self recursively */
			if (_get_file_path (camera, tree->user, filename, (char *) newpath, 1)
			    == GP_OK) {
				/* file found somewhere underneath */

				if (!recursively_entered)
					strcpy (path, newpath);

				return GP_OK;
			}
			/* back out appending of this tree->name to newpath
			 * since filename was not found in this newpath
			 */
			*temp_ch = 0;
		}
		tree++;
	}

	/* the requested file was not found */

	return GP_ERROR;
}

#endif // OBSOLETE

static int
get_file_path (Camera *camera, const char *filename, char *path)
{
	GP_DEBUG("get_file_path() OBSOLETE DUMMY");
	strcpy(path,filename);
	return GP_OK;
}

static int
get_file_func (CameraFilesystem *fs, const char *folder, const char *filename,
	       CameraFileType type, CameraFile *file, void *user_data)
{
	Camera *camera = user_data;
	unsigned char *data = NULL;
	int buflen, size, ret;
	char path[300], tempfilename[300];

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_file_get() "
			 "folder '%s' filename '%s'", folder, filename);

	if (check_readiness (camera) != 1)
		return GP_ERROR;

	strncpy (path, camera->pl->cached_drive, sizeof (path) - 1);

	/* update file cache (if necessary) first */
	if (!update_dir_cache (camera)) {
		gp_camera_status (camera, _("Could not obtain directory listing"));
		return GP_ERROR;
	}

	/* update_dir_cache() loads all file names into cache, now call
	 * get_file_path() to determine in which folder on flash the file
	 * is located
	 */
	if (get_file_path (camera, filename, path) == GP_ERROR) {
		gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_file_get: "
				 "Filename '%s' path '%s' not found!", filename, path);
		return GP_ERROR;
	}

	gp_debug_printf (GP_DEBUG_HIGH, "canon", "camera_file_get: "
			 "Found picture, '%s' '%s'", path, filename);

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			/* add trailing backslash */
			if (path[strlen (path) - 1] != '\\')
				strncat (path, "\\", sizeof (path) - 1);
			break;
		case CANON_SERIAL_RS232:
		default:
			/* find rightmost \ in path */
			if (strrchr (path, '\\') == NULL) {
				gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_file_get: "
						 "Could not determine directory part of path '%s'",
						 path);
				return GP_ERROR;
			}
			/* truncate path after the last \ */
			path[strrchr (path, '\\') - path + 1] = '\0';

			break;
	}

	switch (type) {
		case GP_FILE_TYPE_NORMAL:
			ret = canon_get_picture (camera, (char *) filename,
						 (char *) path, 0, &data, &buflen);
			break;
		case GP_FILE_TYPE_PREVIEW:
			ret = canon_get_picture (camera, (char *) filename,
						 (char *) path, 1, &data, &buflen);
			break;
		default:
			return (GP_ERROR_NOT_SUPPORTED);
	}

	if (ret != GP_OK) {
		gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_file_get: "
				 "canon_get_picture() failed, returned %i", ret);
		return ret;
	}

	/* 256 is picked out of the blue, I figured no JPEG with EXIF header
	 * (not all canon cameras produces EXIF headers I think, but still)
	 * should be less than 256 bytes long.
	 */
	if (!data || buflen < 256)
		return GP_ERROR;

	switch (type) {
		case GP_FILE_TYPE_PREVIEW:
			/* we count the byte returned until the end of the jpeg data
			   which is FF D9 */
			/* It would be prettier to get that info from the exif tags */
			for (size = 1; size < buflen; size++)
				if ((data[size - 1] == JPEG_ESC) && (data[size] == JPEG_END))
					break;
			buflen = size + 1;
			gp_file_set_data_and_size (file, data, buflen);
			gp_file_set_mime_type (file, GP_MIME_JPEG);	/* always */
			strcpy (tempfilename, filename);
			strcat (tempfilename, "\0");
			strcpy (tempfilename + strlen ("IMG_XXXX"), ".JPG\0");
			gp_file_set_name (file, tempfilename);
			break;
		case GP_FILE_TYPE_NORMAL:
			if (is_movie (filename))
				gp_file_set_mime_type (file, GP_MIME_AVI);
			else if (is_image (filename))
				gp_file_set_mime_type (file, GP_MIME_JPEG);
			/* else if (is_crw (filename))
			 * gp_file_set_mime_type (file, GP_MIME_CRW);
			 */
			else
				gp_file_set_mime_type (file, GP_MIME_UNKNOWN);
			gp_file_set_data_and_size (file, data, buflen);
			gp_file_set_name (file, filename);
			break;
		default:
			return (GP_ERROR_NOT_SUPPORTED);
	}

	return GP_OK;
}

/****************************************************************************/


static void
pretty_number (int number, char *buffer)
{
	int len, tmp, digits;
	char *pos;

	len = 0;
	tmp = number;
	do {
		len++;
		tmp /= 10;
	}
	while (tmp);
	len += (len - 1) / 3;
	pos = buffer + len;
	*pos = 0;
	digits = 0;
	do {
		*--pos = (number % 10) + '0';
		number /= 10;
		if (++digits == 3) {
			*--pos = '\'';
			digits = 0;
		}
	}
	while (number);
}

static int
camera_summary (Camera *camera, CameraText *summary)
{
	char a[20], b[20];
	char *model;
	int pwr_source, pwr_status;
	char power_stats[48], cde[16];

	unsigned int capacity, available;

	GP_DEBUG("camera_summary()");

	if (check_readiness (camera) != 1)
		return GP_ERROR;

	if (camera->pl->cached_drive == NULL)
		camera->pl->cached_drive = canon_int_get_disk_name (camera);
	
	canon_int_get_disk_name_info (camera, camera->pl->cached_drive,
				      &capacity, &available);

	pretty_number (capacity, a);
	pretty_number (available, b);

	model = "Canon PowerShot";
	switch (camera->pl->model) {
		case CANON_PS_A5:
			model = "Canon PowerShot A5";
			break;
		case CANON_PS_A5_ZOOM:
			model = "Canon PowerShot A5 Zoom";
			break;
		case CANON_PS_A50:
			model = "Canon PowerShot A50";
			break;
		case CANON_PS_A70:
			model = "Canon PowerShot A70";
			break;
		case CANON_PS_S10:
			model = "Canon PowerShot S10";
			break;
		case CANON_PS_S20:
			model = "Canon PowerShot S20";
			break;
		case CANON_PS_S30:
			model = "Canon PowerShot S30";
			break;
		case CANON_PS_S40:
			model = "Canon PowerShot S40";
			break;
		case CANON_PS_G1:
			model = "Canon PowerShot G1";
			break;
		case CANON_PS_G2:
			model = "Canon PowerShot G2";
			break;
		case CANON_PS_S100:
			model = "Canon PowerShot S100 / Digital IXUS / IXY DIGITAL";
			break;
		case CANON_PS_S300:
			model = "Canon PowerShot S300 / Digital IXUS 300 / IXY DIGITAL 300";
			break;
		case CANON_PS_A10:
			model = "Canon PowerShot A10";
			break;
		case CANON_PS_A20:
			model = "Canon PowerShot A20";
			break;
		case CANON_EOS_D30:
			model = "Canon EOS D30";
			break;
		case CANON_PS_PRO90_IS:
			model = "Canon Pro90 IS";
			break;
	}

	canon_get_batt_status (camera, &pwr_status, &pwr_source);
	if ((pwr_source & CAMERA_MASK_BATTERY) == 0) {
		strcpy (power_stats, _("AC adapter "));
	} else {
		strcpy (power_stats, _("on battery "));
	}

	switch (pwr_status) {
		case CAMERA_POWER_OK:
			strcat (power_stats, _("(power OK)"));
			break;
		case CAMERA_POWER_BAD:
			strcat (power_stats, _("(power low)"));
			break;
		default:
			strcat (power_stats, cde);
			sprintf (cde, " - %i)", pwr_status);
			break;
	}

	sprintf (summary->text,
		 _("%s\n%s\n%s\nDrive %s\n%11s bytes total\n%11s bytes available\n"), model,
		 camera->pl->owner, power_stats, camera->pl->cached_drive, a, b);
	return GP_OK;
}

/****************************************************************************/

static int
camera_about (Camera *camera, CameraText *about)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_about()");

	strcpy (about->text,
		_("Canon PowerShot series driver by\n"
		  "Wolfgang G. Reissnegger,\n"
		  "Werner Almesberger,\n"
		  "Edouard Lafargue,\n"
		  "Philippe Marzouk,\n" "A5 additions by Ole W. Saastad\n" "Holger Klemm\n")
		);

	return GP_OK;
}

/****************************************************************************/

static int
delete_file_func (CameraFilesystem *fs, const char *folder, const char *filename, void *data)
{
	Camera *camera = data;
	char path[300], thumbname[300];
	int j;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_file_delete()");

	// initialize memory to avoid problems later
	for (j = 0; j < sizeof (path); j++)
		path[j] = '\0';
	memset (thumbname, 0, sizeof (thumbname));

	if (check_readiness (camera) != 1)
		return GP_ERROR;

	if (!(camera->pl->model == CANON_PS_A5 || camera->pl->model == CANON_PS_A5_ZOOM)) {	/* Tested only on PowerShot A50 */

		if (!update_dir_cache (camera)) {
			gp_camera_status (camera, _("Could not obtain directory listing"));
			return 0;
		}
		strcpy (path, camera->pl->cached_drive);

		if (get_file_path (camera, filename, path) == GP_ERROR) {
			gp_debug_printf (GP_DEBUG_LOW, "canon", "Filename not found!\n");
			return GP_ERROR;
		}
		if (camera->pl->canon_comm_method != CANON_USB) {
			j = strrchr (path, '\\') - path;
			path[j] = '\0';
		} else {
			j = strchr (path, '\0') - path;
			path[j] = '\0';
//                      path[j+1] = '\0';
		}

		gp_debug_printf (GP_DEBUG_LOW, "canon", "filename: %s\n path: %s\n", filename,
				 path);
		if (canon_int_delete_file (camera, filename, path)) {
			gp_camera_status (camera, _("error deleting file"));
			return GP_ERROR;
		} else {
			/* If we have a movie, delete its thumbnail as well */
			if (is_movie (filename)) {
				strcpy (thumbname, filename);
				strcpy (thumbname + strlen ("MVI_XXXX"), ".THM\0");
				if (canon_int_delete_file (camera, thumbname, path)) {
					gp_camera_status (camera,
							  _("error deleting thumbnail"));
					return GP_ERROR;
				}
			}
			return GP_OK;
		}
	}
	return GP_ERROR;
}

/****************************************************************************/

#ifdef OBSOLETE
/* This code could probably be used to get a default path */
static int
_get_last_dir (Camera *camera, struct canon_dir *tree, char *path, char *temppath)
{

	if (tree == NULL)
		return GP_ERROR;

	if (camera->pl->canon_comm_method != CANON_USB) {
		path = strchr (path, 0);
		*path = '\\';
	}

	while (tree->name) {
		if (!is_image (tree->name) && !is_movie (tree->name)
		    && !is_thumbnail (tree->name)) {
			switch (camera->pl->canon_comm_method) {
				case CANON_USB:
					strcpy (path, tree->name);
					break;
				case CANON_SERIAL_RS232:
				default:
					strcpy (path + 1, tree->name);
					break;
			}
		}

		if (!tree->is_file) {
			if ((strcmp (path + 4, "CANON") == 0) && (strcmp (temppath, path) < 0))
				strcpy (temppath, path);
			_get_last_dir (camera, tree->user, path, temppath);
		}
		tree++;
	}

	strcpy (path, temppath);

	return GP_OK;
}

/*
 * get from the cache the name of the highest numbered directory
 * 
 */
static int
get_last_dir (Camera *camera, char *path)
{
	char temppath[300];

	gp_debug_printf (GP_DEBUG_LOW, "canon", "get_last_dir()");

	strncpy (temppath, path, sizeof (temppath));

	return _get_last_dir (camera, camera->pl->cached_tree, path, temppath);
}

static int
_get_last_picture (struct canon_dir *tree, char *directory, char *filename)
{

	if (tree == NULL)
		return GP_ERROR;

	while (tree->name) {

		if (is_image (tree->name) || is_movie (tree->name)
		    || is_thumbnail (tree->name)) {
			if (strcmp (tree->name, filename) > 0)
				strcpy (filename, tree->name);
		}

		if (!tree->is_file) {
			if ((strcmp (tree->name, "DCIM") == 0)
			    || (strcmp (tree->name, directory) == 0)) {
				_get_last_picture (tree->user, directory, filename);
			}
		}

		tree++;
	}

	return GP_OK;
}

/*
 * get from the cache the name of the highest numbered picture
 * 
 */
static int
get_last_picture (Camera *camera, char *directory, char *filename)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "get_last_picture()");

	return _get_last_picture (camera->pl->cached_tree, directory, filename);
}
#endif //OBSOLETE


#ifdef TO_BE_REWORKED
static int
put_file_func (CameraFilesystem *fs, const char *folder, CameraFile *file, void *data)
{
	Camera *camera = data;
	char destpath[300], destname[300], dir[300], dcf_root_dir[10];
	int j, dirnum = 0, r;
	char buf[10];
	CameraAbilities a;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_folder_put_file()");

	if (check_readiness (camera) != 1)
		return GP_ERROR;

	gp_camera_get_abilities (camera, &a);
	if (camera->pl->speed > 57600 &&
	    (!strcmp (a.model, "Canon PowerShot A50") ||
	     !strcmp (a.model, "Canon PowerShot Pro70"))) {
		gp_camera_message (camera,
				   _
				   ("Speeds greater than 57600 are not supported for uploading to this camera"));
		return GP_ERROR_NOT_SUPPORTED;
	}

	if (!check_readiness (camera)) {
		return GP_ERROR;
	}

	for (j = 0; j < sizeof (destpath); j++) {
		destpath[j] = '\0';
		dir[j] = '\0';
		destname[j] = '\0';
	}

	if (!update_dir_cache (camera)) {
		gp_camera_status (camera, _("Could not obtain directory listing"));
		return GP_ERROR;
	}

	sprintf (dcf_root_dir, "%s\\DCIM", camera->pl->cached_drive);

	if (get_last_dir (camera, dir) == GP_ERROR)
		return GP_ERROR;

	if (strlen (dir) == 0) {
		sprintf (dir, "\\100CANON");
		sprintf (destname, "AUT_0001.JPG");
	} else {
		if (get_last_picture (camera, dir + 1, destname) == GP_ERROR)
			return GP_ERROR;

		if (strlen (destname) == 0) {
			sprintf (destname, "AUT_%c%c01.JPG", dir[2], dir[3]);
		} else {
			sprintf (buf, "%c%c", destname[6], destname[7]);
			j = 1;
			j = atoi (buf);
			if (j == 99) {
				j = 1;
				sprintf (buf, "%c%c%c", dir[1], dir[2], dir[3]);
				dirnum = atoi (buf);
				if (dirnum == 999) {
					gp_camera_message (camera,
							   _
							   ("Could not upload, no free folder name available!\n"
							    "999CANON folder name exists and has an AUT_9999.JPG picture in it."));
					return GP_ERROR;
				} else {
					dirnum++;
					sprintf (dir, "\\%03iCANON", dirnum);
				}
			} else
				j++;

			sprintf (destname, "AUT_%c%c%02i.JPG", dir[2], dir[3], j);
		}

		sprintf (destpath, "%s%s", dcf_root_dir, dir);

		gp_debug_printf (GP_DEBUG_LOW, "canon", "destpath: %s destname: %s\n",
				 destpath, destname);
	}

	r = canon_int_directory_operations (camera, dcf_root_dir, DIR_CREATE);
	if (r < 0) {
		gp_camera_message (camera, "could not create \\DCIM directory");
		return (r);
	}

	r = canon_int_directory_operations (camera, destpath, DIR_CREATE);
	if (r < 0) {
		gp_camera_message (camera, "could not create destination directory");
		return (r);
	}


	j = strlen (destpath);
	destpath[j] = '\\';
	destpath[j + 1] = '\0';

	clear_readiness (camera);

	return canon_int_put_file (camera, file, destname, destpath);
}
#endif //TO_BE_REWORKED

/****************************************************************************/

static int
camera_get_config (Camera *camera, CameraWidget **window)
{
	CameraWidget *t, *section;
	char power_stats[48], firm[64];
	int pwr_status, pwr_source;
	struct tm *camtm;
	time_t camtime;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_get_config()");

	gp_widget_new (GP_WIDGET_WINDOW, "Canon PowerShot Configuration", window);

	gp_widget_new (GP_WIDGET_SECTION, _("Configure"), &section);
	gp_widget_append (*window, section);

	gp_widget_new (GP_WIDGET_TEXT, _("Camera Model"), &t);
	gp_widget_set_value (t, camera->pl->ident);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Owner name"), &t);
	gp_widget_set_value (t, camera->pl->owner);
	gp_widget_append (section, t);

#ifdef TBD
	gp_widget_new (GP_WIDGET_TEXT, "date", &t);
	if (camera->pl->cached_ready == 1) {
		camtime = canon_int_get_time (camera);
		if (camtime != GP_ERROR) {
			camtm = gmtime (&camtime);
			gp_widget_set_value (t, asctime (camtm));
		} else
			gp_widget_set_value (t, _("Error"));
	} else
		gp_widget_set_value (t, _("Unavailable"));
	gp_widget_append (section, t);
#endif

	gp_widget_new (GP_WIDGET_TOGGLE, _("Set camera date to PC date"), &t);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Firmware revision"), &t);
	sprintf (firm, "%i.%i.%i.%i", camera->pl->firmwrev[3],
		 camera->pl->firmwrev[2], camera->pl->firmwrev[1], camera->pl->firmwrev[0]);
	gp_widget_set_value (t, firm);
	gp_widget_append (section, t);

#ifdef TBD
	if (camera->pl->cached_ready == 1) {
		canon_get_batt_status (camera, &pwr_status, &pwr_source);
		if ((pwr_source & CAMERA_MASK_BATTERY) == 0) {
			strcpy (power_stats, _("AC adapter "));
		} else {
			strcpy (power_stats, _("on battery "));
		}

		switch (pwr_status) {
				char cde[16];

			case CAMERA_POWER_OK:
				strcat (power_stats, _("(power OK)"));
				break;
			case CAMERA_POWER_BAD:
				strcat (power_stats, _("(power low)"));
				break;
			default:
				strcat (power_stats, cde);
				sprintf (cde, " - %i)", pwr_status);
				break;
		}
	} else
		strcpy (power_stats, _("Power: camera unavailable"));

	gp_widget_new (GP_WIDGET_TEXT, _("Power"), &t);
	gp_widget_set_value (t, power_stats);
	gp_widget_append (section, t);
#endif

	gp_widget_new (GP_WIDGET_SECTION, _("Debug"), &section);
	gp_widget_append (*window, section);

	return GP_OK;
}

static int
camera_set_config (Camera *camera, CameraWidget *window)
{
	CameraWidget *w;
	char *wvalue;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_set_config()");

	gp_widget_get_child_by_label (window, _("Owner name"), &w);
	if (gp_widget_changed (w)) {
		gp_widget_get_value (w, &wvalue);
		if (!check_readiness (camera)) {
			gp_camera_status (camera, _("Camera unavailable"));
		} else {
			if (canon_int_set_owner_name (camera, wvalue) == GP_OK)
				gp_camera_status (camera, _("Owner name changed"));
			else
				gp_camera_status (camera, _("could not change owner name"));
		}
	}

	gp_widget_get_child_by_label (window, _("Set camera date to PC date"), &w);
	if (gp_widget_changed (w)) {
		gp_widget_get_value (w, &wvalue);
		if (!check_readiness (camera)) {
			gp_camera_status (camera, _("Camera unavailable"));
		} else {
			if (canon_int_set_time (camera)) {
				gp_camera_status (camera, _("time set"));
			} else {
				gp_camera_status (camera, _("could not set time"));
			}
		}
	}

	gp_debug_printf (GP_DEBUG_LOW, "canon", _("done configuring camera.\n"));

	return GP_OK;
}

static int
get_info_func (CameraFilesystem *fs, const char *folder, const char *filename,
	       CameraFileInfo * info, void *data)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "canon get_info_func() "
			 "called for '%s'/'%s'", folder, filename);

	info->preview.fields = GP_FILE_INFO_TYPE;

	/* thumbnails are always jpeg on Canon Cameras */
	strcpy (info->preview.type, GP_MIME_JPEG);

	/* FIXME GP_FILE_INFO_PERMISSIONS to add */
	info->file.fields = GP_FILE_INFO_NAME | GP_FILE_INFO_TYPE;
	// | GP_FILE_INFO_PERMISSIONS | GP_FILE_INFO_SIZE;
	//info->file.fields.permissions = 

	if (is_movie (filename))
		strcpy (info->file.type, GP_MIME_AVI);
	else if (is_image (filename))
		strcpy (info->file.type, GP_MIME_JPEG);
	else
		/* May no be correct behaviour ... */
		strcpy (info->file.type, "unknown");

	strcpy (info->file.name, filename);

	return GP_OK;
}

static int
make_dir_func (CameraFilesystem *fs, const char *folder, const char *name, void *data)
{
	Camera *camera = data;
	char path[2048];
	int r;

	strncpy (path, folder, sizeof (path));
	if (strlen (folder) > 1)
		strncat (path, "/", sizeof (path));
	strncat (path, name, sizeof (path));

	r = canon_int_directory_operations (camera, path, DIR_CREATE);
	if (r < 0)
		return (r);

	return (GP_OK);
}

static int
remove_dir_func (CameraFilesystem *fs, const char *folder, const char *name, void *data)
{
	Camera *camera = data;
	char path[2048];
	int r;

	strncpy (path, folder, sizeof (path));
	if (strlen (folder) > 1)
		strncat (path, "/", sizeof (path));
	strncat (path, name, sizeof (path));

	r = canon_int_directory_operations (camera, path, DIR_REMOVE);
	if (r < 0)
		return (r);

	return (GP_OK);
}

/****************************************************************************/

/**
 * camera_init:
 * @camera: the camera to initialize
 *
 * This routine initializes the serial/USB port and also load the
 * camera settings. Right now it is only the speed that is
 * saved.
 **/
int
camera_init (Camera *camera)
{
	GPPortSettings settings;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "canon camera_init()");

	/* First, set up all the function pointers */
	camera->functions->exit = camera_exit;
	camera->functions->get_config = camera_get_config;
	camera->functions->set_config = camera_set_config;
	camera->functions->summary = camera_summary;
	camera->functions->manual = camera_manual;
	camera->functions->about = camera_about;

	/* Set up the CameraFilesystem */
	gp_filesystem_set_list_funcs (camera->fs, file_list_func, folder_list_func, camera);
	gp_filesystem_set_info_funcs (camera->fs, get_info_func, NULL, camera);
	gp_filesystem_set_file_funcs (camera->fs, get_file_func, delete_file_func, camera);
	gp_filesystem_set_folder_funcs (camera->fs, NULL, NULL,
					make_dir_func, remove_dir_func, camera);

	camera->pl = malloc (sizeof (CameraPrivateLibrary));
	if (!camera->pl)
		return (GP_ERROR_NO_MEMORY);
	memset (camera->pl, 0, sizeof (CameraPrivateLibrary));
	camera->pl->first_init = 1;
	camera->pl->seq_tx = 1;
	camera->pl->seq_rx = 1;

	switch (camera->port->type) {
		case GP_PORT_USB:
			gp_debug_printf (GP_DEBUG_LOW, "canon",
					 "GPhoto tells us that we should use a USB link.\n");
			camera->pl->canon_comm_method = CANON_USB;

			return canon_usb_init (camera);
			break;
		case GP_PORT_SERIAL:
		default:
			gp_debug_printf (GP_DEBUG_LOW, "canon",
					 "GPhoto tells us that we should use a RS232 link.\n");

			/* Figure out the speed (and set to default speed if 0) */
			gp_port_get_settings (camera->port, &settings);
			camera->pl->speed = settings.serial.speed;

			if (camera->pl->speed == 0)
				camera->pl->speed = 9600;

			gp_debug_printf (GP_DEBUG_LOW, "canon",
					 "Camera transmission speed : %i\n",
					 camera->pl->speed);
			camera->pl->canon_comm_method = CANON_SERIAL_RS232;

			return canon_serial_init (camera);
			break;
	}

	/* NOT REACHED */
	return GP_ERROR;
}
