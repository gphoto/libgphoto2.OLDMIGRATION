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
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

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

int
camera_id (CameraText *id)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_id()");

	strcpy (id->text, "canon");

	return GP_OK;
}

static int
camera_manual (Camera *camera, CameraText *manual, GPContext *context)
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

	for (i = 0; models[i].id_str; i++) {
		memset (&a, 0, sizeof(a));
		a.status = GP_DRIVER_STATUS_PRODUCTION;
		strcpy (a.model, models[i].id_str);
		a.port = 0;
		if (models[i].usb_vendor && models[i].usb_product) {
			a.port |= GP_PORT_USB;
			a.usb_vendor = models[i].usb_vendor;
			a.usb_product = models[i].usb_product;
		}
		if (models[i].serial_support) {
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
	gp_debug_printf (GP_DEBUG_LOW, "canon", "clear_readiness()");

	camera->pl->cached_ready = 0;
}

static int
check_readiness (Camera *camera, GPContext *context)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "check_readiness() cached_ready == %i",
			 camera->pl->cached_ready);

	if (camera->pl->cached_ready)
		return 1;
	if (canon_int_ready (camera, context) == GP_OK) {
		GP_DEBUG ("Camera type: %s (%d)\n", camera->pl->md->id_str,
			  camera->pl->md->model);
		camera->pl->cached_ready = 1;
		return 1;
	}
	gp_camera_status (camera, _("Camera unavailable"));
	return 0;
}

static void
canon_int_switch_camera_off (Camera *camera, GPContext *context)
{
	GP_DEBUG ("switch_camera_off()");

	switch (camera->port->type) {
		case GP_PORT_SERIAL:
			gp_camera_status (camera, _("Switching Camera Off"));
			canon_serial_off (camera);
			break;
		case GP_PORT_USB:
			GP_DEBUG ("Not trying to shut down USB camera...");
			break;
		GP_PORT_DEFAULT_RETURN_EMPTY
	}
	clear_readiness (camera);
}

static int
camera_exit (Camera *camera, GPContext *context)
{
	if (camera->port->type == GP_PORT_USB) {
		canon_usb_unlock_keys (camera);
	}

	if (camera->pl) {
		canon_int_switch_camera_off (camera, context);
		free (camera->pl);
		camera->pl = NULL;
	}

	return GP_OK;
}

static int
canon_get_batt_status (Camera *camera, int *pwr_status, int *pwr_source, GPContext *context)
{
	GP_DEBUG ("canon_get_batt_status() called");

	if (!check_readiness (camera, context))
		return -1;

	return canon_int_get_battery (camera, pwr_status, pwr_source, context);
}

static int
update_disk_cache (Camera *camera, GPContext *context)
{
	char root[10];		/* D:\ or such */
	int res;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "update_disk_cache()");

	if (camera->pl->cached_disk)
		return 1;
	if (!check_readiness (camera, context))
		return 0;
	camera->pl->cached_drive = canon_int_get_disk_name (camera, context);
	if (!camera->pl->cached_drive) {
		gp_context_error (context, _("Could not get disk name: %s"), "No reason available");
		return 0;
	}
	snprintf (root, sizeof (root), "%s\\", camera->pl->cached_drive);
	res = canon_int_get_disk_name_info (camera, root,
					    &camera->pl->cached_capacity,
					    &camera->pl->cached_available, context);
	if (res != GP_OK) {
		gp_context_error (context, _("Could not get disk info: %s"), gp_result_as_string (res));
		return 0;
	}
	camera->pl->cached_disk = 1;

	return 1;
}

static int
file_list_func (CameraFilesystem *fs, const char *folder, CameraList *list,
		void *data, GPContext *context)
{
	Camera *camera = data;

	GP_DEBUG ("file_list_func()");

	if (!check_readiness (camera, context))
		return GP_ERROR;

	return canon_int_list_directory (camera, folder, list, CANON_LIST_FILES, context);
}

static int
folder_list_func (CameraFilesystem *fs, const char *folder, CameraList *list, 
		  void *data, GPContext *context)
{
	Camera *camera = data;

	GP_DEBUG ("folder_list_func()");

	if (!check_readiness (camera, context))
		return GP_ERROR;

	return canon_int_list_directory (camera, folder, list, CANON_LIST_FOLDERS, context);
}


/****************************************************************************
 *
 * gphoto library interface calls
 *
 ****************************************************************************/

#ifdef OBSOLETE
static int
canon_get_picture (Camera *camera, char *filename, char *path, int thumbnail,
		   unsigned char **data, int *size)
{
	unsigned char attribs;
	char complete_filename[300];
	int res;

	GP_DEBUG ("canon_get_picture()");

	if (!check_readiness (camera, context)) {
		return GP_ERROR;
	}
	switch (camera->pl->md->model) {
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
			if (!check_readiness (camera, context)) {
				return GP_ERROR;
			}
			res = canon_int_get_file (cached_paths[picture_number], size, context);
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
#ifdef OBSOLETE
			if (!update_dir_cache (camera)) {
				gp_camera_status (camera,
						  _("Could not obtain directory listing"));
				return GP_ERROR;
			}
#endif

			/* strip trailing backslash on path, if any */
			if (path[strlen (path) - 1] == '\\')
				path[strlen (path) - 1] = 0;

			snprintf (complete_filename, sizeof (complete_filename), "%s\\%s",
				  path, filename);

			GP_DEBUG ("canon_get_picture: path='%s', file='%s'\n\tcomplete filename='%s'\n", path, filename, complete_filename);
			attribs = 0;
			if (!check_readiness (camera, context)) {
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
								   data, size, context);
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
							  size, context);
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
#endif

static int
get_file_func (CameraFilesystem *fs, const char *folder, const char *filename,
	       CameraFileType type, CameraFile *file, void *user_data,
	       GPContext *context)
{
	Camera *camera = user_data;
	unsigned char *data = NULL;
	int datalen, ret;
	char tempfilename[300], canon_path[300];

	/* put complete canon path into canon_path */
	ret = snprintf (canon_path, sizeof (canon_path) - 3, "%s\\%s",
			gphoto2canonpath (camera, folder, context), filename);
	if (ret < 0) {
		gp_context_error (context,
				     "Internal error #1 in get_file_func()"
				     " (%s line %i)", __FILE__, __LINE__);
		return GP_ERROR;
	}

	GP_DEBUG ("get_file_func: "
		  "folder '%s' filename '%s' (i.e. '%s'), getting %s", 
		  folder, filename, canon_path,
		  (type == GP_FILE_TYPE_PREVIEW)?"thumbnail":"file itself");

	/* FIXME:
	 * There are probably some memory leaks in the fetching of
	 * files and thumbnails with the different buffers used in
	 * that process.
	 */

	switch (type) {
		const char *thumbname;
		case GP_FILE_TYPE_NORMAL:
			ret = canon_int_get_file (camera, canon_path, &data, &datalen, context);
			if (ret == GP_OK) {
				uint8_t attr = 0;
				/* This should cover all attribute
				 * bits known of and reflected in
				 * info.file
				 */
				CameraFileInfo info;
				gp_filesystem_get_info(fs, folder, filename, &info, context);
				if (info.file.status == GP_FILE_STATUS_NOT_DOWNLOADED)
					attr |= CANON_ATTR_DOWNLOADED;
				if ((info.file.permissions & GP_FILE_PERM_DELETE) == 0)
					attr |= CANON_ATTR_WRITE_PROTECTED;
				canon_int_set_file_attributes (camera, filename, 
							       gphoto2canonpath (camera, folder, context),
							       attr, context);
			}
			break;
		case GP_FILE_TYPE_PREVIEW:
			thumbname = canon_int_filename2thumbname (camera, canon_path);
			if (thumbname == NULL) {
				/* no thumbnail available */
				gp_context_error (context, "No thumbnail could be fould for %s",
						     canon_path);
				ret = GP_ERROR;
			} else if (*thumbname == '\0') {
				/* file internal thumbnail */
				ret = canon_int_get_thumbnail (camera, canon_path, &data, &datalen, context);
			} else {
				/* extra thumbnail file */
				ret = canon_int_get_file (camera, thumbname,
							  &data, &datalen, context);
			}
			break;
		default:
			GP_DEBUG ("unsupported file type %i", type);
			return (GP_ERROR_NOT_SUPPORTED);
	}

	if (ret != GP_OK) {
		GP_DEBUG ("get_file_func: "
			  "getting image data failed, returned %i", ret);
		return ret;
	}

	if (data == NULL) {
		GP_DEBUG ("get_file_func: "
			  "Fatal error: data == NULL");
		return GP_ERROR_CORRUPTED_DATA;
	}
	/* 256 is picked out of the blue, I figured no JPEG with EXIF header
	 * (not all canon cameras produces EXIF headers I think, but still)
	 * should be less than 256 bytes long.
	 */
	if (datalen < 256) {
		GP_DEBUG ("get_file_func: "
			  "datalen < 256 (datalen = %i = 0x%x)", 
			  datalen, datalen);
		return GP_ERROR_CORRUPTED_DATA;
	}

	switch (type) {
		int size;
		case GP_FILE_TYPE_PREVIEW:
			/* we count the byte returned until the end of the jpeg data
			   which is FF D9 */
			/* It would be prettier to get that info from the exif tags */
			for (size = 1; size < datalen; size++)
				if ((data[size - 1] == JPEG_ESC) && (data[size] == JPEG_END))
					break;
			if (! (data[size - 1] == JPEG_ESC) && (data[size] == JPEG_END)) {
				GP_DEBUG ("JPEG_END not found in %i bytes of data", datalen);
				return GP_ERROR_CORRUPTED_DATA;
			}
			datalen = size + 1;
			gp_file_set_data_and_size (file, data, datalen);
			gp_file_set_mime_type (file, GP_MIME_JPEG);	/* always */
			strncpy (tempfilename, filename, sizeof(tempfilename) - 1);
			tempfilename[sizeof (tempfilename) - 1] = 0;
			/* 4 is length of .JPG, must be at least X.FOO */
			if (strlen (tempfilename) > 4)
				strcpy (tempfilename + strlen (tempfilename) - 4, ".JPG");
			else {
				GP_DEBUG ("File name '%s' too short, cannot make it a .JPG",
					  tempfilename);
				return GP_ERROR_CORRUPTED_DATA;
			}
			gp_file_set_name (file, tempfilename);
			break;
		case GP_FILE_TYPE_NORMAL:
			gp_file_set_mime_type (file, filename2mimetype (filename));
			gp_file_set_data_and_size (file, data, datalen);
			gp_file_set_name (file, filename);
			break;
		default:
			/* this case should've been caught above anyway */
			return (GP_ERROR_NOT_SUPPORTED);
	}

	return GP_OK;
}

#ifdef OBSOLETE
static int
old_get_file_func (CameraFilesystem *fs, const char *folder, const char *filename,
	       CameraFileType type, CameraFile *file, void *user_data)
{
	Camera *camera = user_data;
	unsigned char *data = NULL;
	int buflen, size, ret;
	char path[300], tempfilename[300];

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_file_get() "
			 "folder '%s' filename '%s'", folder, filename);

	if (check_readiness (camera, context) != 1)
		return GP_ERROR;

	strncpy (path, camera->pl->cached_drive, sizeof (path) - 1);

#ifdef OBSOLETE
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
#endif

	gp_debug_printf (GP_DEBUG_HIGH, "canon", "camera_file_get: "
			 "Found picture, '%s' '%s'", path, filename);

	switch (camera->port->type) {
		case GP_PORT_USB:
			/* add trailing backslash */
			if (path[strlen (path) - 1] != '\\')
				strncat (path, "\\", sizeof (path) - 1);
			break;
		case GP_PORT_SERIAL:
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
		GP_PORT_DEFAULT
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
#endif

/****************************************************************************/


static void
pretty_number (int number, char *buffer)
{
	int len, tmp, digits;
	char *pos;
#ifdef HAVE_LOCALE_H
	/* We should really use ->grouping as well */
	char thousands_sep = *localeconv()->thousands_sep;

	if (thousands_sep == '\0')
		thousands_sep = '\'';
#else
	const char thousands_sep = '\''
#endif

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
			*--pos = thousands_sep;
			digits = 0;
		}
	}
	while (number);
}

static int
camera_summary (Camera *camera, CameraText *summary, GPContext *context)
{
	char a[20], b[20];
	int pwr_source, pwr_status;
	int res;
	char disk_str[128], power_str[128], time_str[128];
	time_t camera_time;
	double time_diff;
	char formatted_camera_time[20];

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_summary()");

	if (check_readiness (camera, context) != 1)
		return GP_ERROR;

	/*clear_readiness(); */
	if (!update_disk_cache (camera, context))
		return GP_ERROR;

	pretty_number (camera->pl->cached_capacity, a);
	pretty_number (camera->pl->cached_available, b);

	snprintf (disk_str, sizeof (disk_str), _("  Drive %s\n  %11s bytes total\n  %11s bytes available"),
		 camera->pl->cached_drive, a, b);

	res = canon_get_batt_status (camera, &pwr_status, &pwr_source, context);
	if (res == GP_OK) {
		if (pwr_status == CAMERA_POWER_OK || pwr_status == CAMERA_POWER_BAD)
			snprintf (power_str, sizeof (power_str), "%s (%s)",
				  ((pwr_source & CAMERA_MASK_BATTERY) ==
				   0) ? _("AC adapter") : _("on battery"),
				  pwr_status == CAMERA_POWER_OK ? _("power OK") : _("power bad"));
		else
			snprintf (power_str, sizeof (power_str), "%s - %i",
				  ((pwr_source & CAMERA_MASK_BATTERY) ==
				   0) ? _("AC adapter") : _("on battery"), pwr_status);
	} else {
		GP_DEBUG ("canon_get_batt_status() returned error: %s (%i), ",
			 gp_result_as_string (res), res);
		snprintf (power_str, sizeof (power_str), _("not available: %s"), gp_result_as_string (res));
	}

	camera_time = canon_int_get_time (camera, context);
	if (camera_time > 0) {
		time_diff = difftime(camera_time, time(NULL));
	
		strftime (formatted_camera_time, sizeof (formatted_camera_time),
			  "%Y-%m-%d %H:%M:%S", localtime(&camera_time));
	  
		snprintf (time_str, sizeof (time_str), _("%s (host time %s%i seconds)"),
			  formatted_camera_time, time_diff>=0?"+":"", (int) time_diff);
	} else {
		GP_DEBUG ("canon_int_get_time() returned negative result: %s (%i)",
			  gp_result_as_string ((int) camera_time), (int) camera_time);
		snprintf (time_str, sizeof (time_str), ("not available: %s"), 
			gp_result_as_string ((int) camera_time));
	}

	sprintf (summary->text, _("\nCamera identification:\n  Model: %s\n  Owner: %s\n\n"
				  "Power status: %s\n\n"
				  "Flash disk information:\n%s\n\n"
				  "Time: %s\n"),
				camera->pl->md->id_str, camera->pl->owner, power_str,
				disk_str, time_str);

	return GP_OK;
}

/****************************************************************************/

static int
camera_about (Camera *camera, CameraText *about, GPContext *context)
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
delete_file_func (CameraFilesystem *fs, const char *folder,
		  const char *filename, void *data, GPContext *context)
{
	Camera *camera = data;
	const char *thumbname;
	char canonfolder[300];
	const char *_canonfolder;

	GP_DEBUG ("delete_file_func()");

	_canonfolder = gphoto2canonpath (camera, folder, context);
	strncpy (canonfolder, _canonfolder, sizeof(canonfolder) - 1);
	canonfolder[sizeof(canonfolder) - 1] = 0;

	if (check_readiness (camera, context) != 1)
		return GP_ERROR;

	if (camera->pl->md->model == CANON_PS_A5 || camera->pl->md->model == CANON_PS_A5_ZOOM) {
		GP_DEBUG ("delete_file_func: deleting "
			  "pictures disabled for cameras: PowerShot A5, PowerShot A5 ZOOM");

		return GP_ERROR_NOT_SUPPORTED;
	}

#ifdef OBSOLETE
	if (!update_dir_cache (camera)) {
		gp_camera_status (camera, _("Could not obtain directory listing"));
		return GP_ERROR;
	}
#endif

	GP_DEBUG ("delete_file_func: "
		  "filename: %s\nfolder: %s\n", filename, canonfolder);
	if (canon_int_delete_file (camera, filename, canonfolder, context) != GP_OK) {
		gp_context_error (context, _("Error deleting file"));
		return GP_ERROR;
	}

	/* If we have a file with associated thumbnail file, delete
	 * its thumbnail as well */
	thumbname = canon_int_filename2thumbname (camera, filename);
	if ((thumbname != NULL) && (*thumbname != '\0')) {
		GP_DEBUG ("delete_file_func: "
			  "thumbname: %s\n folder: %s\n", thumbname, canonfolder);
		if (canon_int_delete_file (camera, thumbname, canonfolder, context) != GP_OK) {
			/* XXX should we handle this as an error?
			 * Probably only in case the camera link died,
			 * but not if the file just had already been
			 * deleted before. */
			gp_context_error (context, _("Error deleting associated thumbnail file"));
			return GP_ERROR;
		}
	}

	return GP_OK;
}

static int
put_file_func (CameraFilesystem *fs, const char *folder, CameraFile *file,
	       void *data, GPContext *context)
{
	Camera *camera = data;
	char destpath[300], destname[300], dir[300], dcf_root_dir[10];
	int j, dirnum = 0, r;
	char buf[10];
	CameraAbilities a;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "camera_folder_put_file()");

	if (check_readiness (camera, context) != 1)
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

	if (!check_readiness (camera, context)) {
		return GP_ERROR;
	}

	for (j = 0; j < sizeof (destpath); j++) {
		destpath[j] = '\0';
		dir[j] = '\0';
		destname[j] = '\0';
	}

#ifdef OBSOLETE
	if (!update_dir_cache (camera)) {
		gp_camera_status (camera, _("Could not obtain directory listing"));
		return GP_ERROR;
	}
#endif

	sprintf (dcf_root_dir, "%s\\DCIM", camera->pl->cached_drive);

#ifdef OBSOLETE
	if (get_last_dir (camera, dir) == GP_ERROR)
		return GP_ERROR;
#endif

	if (strlen (dir) == 0) {
		sprintf (dir, "\\100CANON");
		sprintf (destname, "AUT_0001.JPG");
	} else {
#ifdef OBSOLETE
		if (get_last_picture (camera, dir + 1, destname) == GP_ERROR)
			return GP_ERROR;
#endif

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

	r = canon_int_directory_operations (camera, dcf_root_dir, DIR_CREATE, context);
	if (r < 0) {
		gp_camera_message (camera, "could not create \\DCIM directory");
		return (r);
	}

	r = canon_int_directory_operations (camera, destpath, DIR_CREATE, context);
	if (r < 0) {
		gp_camera_message (camera, "could not create destination directory");
		return (r);
	}


	j = strlen (destpath);
	destpath[j] = '\\';
	destpath[j + 1] = '\0';

	clear_readiness (camera);

	return canon_int_put_file (camera, file, destname, destpath, context);
}

/****************************************************************************/

static int
camera_get_config (Camera *camera, CameraWidget **window, GPContext *context)
{
	CameraWidget *t, *section;
	char power_str[128], firm[64];
	int pwr_status, pwr_source;
	time_t camtime;

	GP_DEBUG ("camera_get_config()");

	gp_widget_new (GP_WIDGET_WINDOW, _("Camera and Driver Configuration"), window);

	gp_widget_new (GP_WIDGET_SECTION, _("Camera"), &section);
	gp_widget_append (*window, section);

	gp_widget_new (GP_WIDGET_TEXT, _("Camera Model (readonly)"), &t);
	gp_widget_set_value (t, camera->pl->ident);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Owner name"), &t);
	gp_widget_set_value (t, camera->pl->owner);
	gp_widget_append (section, t);

	if (camera->pl->cached_ready == 1) {
		camtime = canon_int_get_time (camera, context);
		if (camtime >= 0) {
			gp_widget_new (GP_WIDGET_DATE, _("Date and Time (readonly)"), &t);
			gp_widget_set_value (t, &camtime);
			gp_widget_append (section, t);
		} else {
			gp_widget_new (GP_WIDGET_TEXT, _("Date and Time (readonly)"), &t);
			gp_widget_set_value (t, _("Error"));
			gp_widget_append (section, t);
		}
	} else {
		gp_widget_new (GP_WIDGET_TEXT, _("Date and Time (readonly)"), &t);
		gp_widget_set_value (t, _("Unavailable"));
		gp_widget_append (section, t);
	}

	gp_widget_new (GP_WIDGET_TOGGLE, _("Set camera date to PC date"), &t);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Firmware revision (readonly)"), &t);
	sprintf (firm, "%i.%i.%i.%i", camera->pl->firmwrev[3],
		 camera->pl->firmwrev[2], camera->pl->firmwrev[1], camera->pl->firmwrev[0]);
	gp_widget_set_value (t, firm);
	gp_widget_append (section, t);

	if (camera->pl->cached_ready == 1) {
		canon_get_batt_status (camera, &pwr_status, &pwr_source, context);

		if (pwr_status == CAMERA_POWER_OK || pwr_status == CAMERA_POWER_BAD)
			snprintf (power_str, sizeof (power_str), "%s (%s)",
				  ((pwr_source & CAMERA_MASK_BATTERY) ==
				   0) ? _("AC adapter") : _("on battery"),
				  pwr_status ==
				  CAMERA_POWER_OK ? _("power OK") : _("power bad"));
		else
			snprintf (power_str, sizeof (power_str), "%s - %i",
				  ((pwr_source & CAMERA_MASK_BATTERY) ==
				   0) ? _("AC adapter") : _("on battery"), pwr_status);
	} else {
		strncpy (power_str, _("Unavaliable"), sizeof (power_str) - 1);
		power_str[sizeof (power_str) - 1] = 0;
	}

	gp_widget_new (GP_WIDGET_TEXT, _("Power (readonly)"), &t);
	gp_widget_set_value (t, power_str);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_SECTION, _("Driver"), &section);
	gp_widget_append (*window, section);

	gp_widget_new (GP_WIDGET_TOGGLE, _("List all files"), &t);
	gp_widget_set_value (t, &camera->pl->list_all_files);
	gp_widget_append (section, t);

	return GP_OK;
}

static int
camera_set_config (Camera *camera, CameraWidget *window, GPContext *context)
{
	CameraWidget *w;
	char *wvalue;

	GP_DEBUG ("camera_set_config()");

	gp_widget_get_child_by_label (window, _("Owner name"), &w);
	if (gp_widget_changed (w)) {
		gp_widget_get_value (w, &wvalue);
		if (!check_readiness (camera, context)) {
			gp_camera_status (camera, _("Camera unavailable"));
		} else {
			if (canon_int_set_owner_name (camera, wvalue, context) == GP_OK)
				gp_camera_status (camera, _("Owner name changed"));
			else
				gp_camera_status (camera, _("could not change owner name"));
		}
	}

	gp_widget_get_child_by_label (window, _("Set camera date to PC date"), &w);
	if (gp_widget_changed (w)) {
		gp_widget_get_value (w, &wvalue);
		if (!check_readiness (camera, context)) {
			gp_camera_status (camera, _("Camera unavailable"));
		} else {
			if (canon_int_set_time (camera, time(NULL), context) == GP_OK) {
				gp_camera_status (camera, _("time set"));
			} else {
				gp_camera_status (camera, _("could not set time"));
			}
		}
	}

	gp_widget_get_child_by_label (window, _("List all files"), &w);
	if (gp_widget_changed (w)) {
		/* XXXXX mark CameraFS as dirty */
		gp_widget_get_value (w, &camera->pl->list_all_files);
		GP_DEBUG ("New config value for tmb: %i", &camera->pl->list_all_files);
	}

	GP_DEBUG ("done configuring camera.");

	return GP_OK;
}

static int
get_info_func (CameraFilesystem *fs, const char *folder, const char *filename,
	       CameraFileInfo * info, void *data, GPContext *context)
{
	gp_debug_printf (GP_DEBUG_LOW, "canon", "canon get_info_func() "
			 "called for '%s'/'%s'", folder, filename);

	info->preview.fields = GP_FILE_INFO_TYPE;

	/* thumbnails are always jpeg on Canon Cameras */
	strcpy (info->preview.type, GP_MIME_JPEG);

	/* FIXME GP_FILE_INFO_PERMISSIONS to add */
	info->file.fields = GP_FILE_INFO_NAME | GP_FILE_INFO_TYPE;
	/* | GP_FILE_INFO_PERMISSIONS | GP_FILE_INFO_SIZE; */
	/* info->file.fields.permissions =  */

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
make_dir_func (CameraFilesystem *fs, const char *folder, const char *name,
	       void *data, GPContext *context)
{
	Camera *camera = data;
	char gppath[2048];
	const char *canonpath;
	int r;

	GP_DEBUG ("make_dir_func folder '%s' name '%s'", folder, name);
	
	if (strlen (folder) > 1) {
		/* folder is something more than / */
	
		if (strlen (folder) + 1 + strlen (name) > sizeof (gppath) - 1) {
			GP_DEBUG ("make_dir_func: Arguments too long");
			return GP_ERROR_BAD_PARAMETERS;
		}
		
		sprintf (gppath, "%s/%s", folder, name);
	} else {
		if (1 + strlen (name) > sizeof (gppath) - 1) {
			GP_DEBUG ("make_dir_func: Arguments too long");
			return GP_ERROR_BAD_PARAMETERS;
		}
		
		sprintf (gppath, "/%s", name);
	}
	
	canonpath = gphoto2canonpath (camera, gppath, context);
	if (canonpath == NULL)
		return GP_ERROR;

	r = canon_int_directory_operations (camera, canonpath, DIR_CREATE, context);
	if (r != GP_OK)
		return (r);

	return (GP_OK);
}

static int
remove_dir_func (CameraFilesystem *fs, const char *folder, const char *name,
		 void *data, GPContext *context)
{
	Camera *camera = data;
	char gppath[2048];
	const char *canonpath;
	int r;

	GP_DEBUG ("remove_dir_func folder '%s' name '%s'", folder, name);
	
	if (strlen (folder) > 1) {
		/* folder is something more than / */
	
		if (strlen (folder) + 1 + strlen (name) > sizeof (gppath) - 1) {
			GP_DEBUG ("make_dir_func: Arguments too long");
			return GP_ERROR_BAD_PARAMETERS;
		}
		
		sprintf (gppath, "%s/%s", folder, name);
	} else {
		if (1 + strlen (name) > sizeof (gppath) - 1) {
			GP_DEBUG ("make_dir_func: Arguments too long");
			return GP_ERROR_BAD_PARAMETERS;
		}
		
		sprintf (gppath, "/%s", name);
	}
	
	canonpath = gphoto2canonpath (camera, gppath, context);
	if (canonpath == NULL)
		return GP_ERROR;

	r = canon_int_directory_operations (camera, canonpath, DIR_REMOVE, context);
	if (r != GP_OK)
		return (r);

	return (GP_OK);
}

/****************************************************************************/

/**
 * camera_init:
 * @camera: the camera to initialize
 * @context: a #GPContext
 *
 * This routine initializes the serial/USB port and also load the
 * camera settings. Right now it is only the speed that is
 * saved.
 **/
int
camera_init (Camera *camera, GPContext *context)
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
	gp_filesystem_set_folder_funcs (camera->fs, put_file_func, NULL,
					make_dir_func, remove_dir_func, camera);

	camera->pl = malloc (sizeof (CameraPrivateLibrary));
	if (!camera->pl)
		return (GP_ERROR_NO_MEMORY);
	memset (camera->pl, 0, sizeof (CameraPrivateLibrary));
	camera->pl->first_init = 1;
	camera->pl->seq_tx = 1;
	camera->pl->seq_rx = 1;

	/* default to false, i.e. list only known file types */
	camera->pl->list_all_files = FALSE;

	switch (camera->port->type) {
		case GP_PORT_USB:
			GP_DEBUG ("GPhoto tells us that we should use a USB link.");

			return canon_usb_init (camera, context);
			break;
		case GP_PORT_SERIAL:
			GP_DEBUG ("GPhoto tells us that we should use a RS232 link.");

			/* Figure out the speed (and set to default speed if 0) */
			gp_port_get_settings (camera->port, &settings);
			camera->pl->speed = settings.serial.speed;

			if (camera->pl->speed == 0)
				camera->pl->speed = 9600;

			GP_DEBUG ("Camera transmission speed : %i",
				  camera->pl->speed);

			return canon_serial_init (camera);
			break;
		default:
			gp_context_error (context, 
					     _("Unsupported port type %i = 0x%x given. "
					       "Initialization impossible."), 
					     camera->port->type, camera->port->type);
			return GP_ERROR_NOT_SUPPORTED;
			break;
	}

	/* NOT REACHED */
	return GP_ERROR;
}

/*
 * Local Variables:
 * c-file-style:"linux"
 * indent-tabs-mode:t
 * End:
 */
