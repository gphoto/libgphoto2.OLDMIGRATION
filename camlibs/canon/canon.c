/*
 * canon.c - Canon protocol "native" operations.
 *
 * Written 1999 by Wolfgang G. Reissnegger and Werner Almesberger
 * Additions 2000 by Philippe Marzouk and Edouard Lafargue
 * USB support, 2000, by Mikael Nyström
 *
 * $Id$
 *
 * This file includes both USB and serial support for the cameras
 * manufactured by Canon. These comprise all (or at least almost all)
 * of the digital models of the IXUS, PowerShot and EOS series.
 *
 * We are working at moving serial and USB specific stuff to serial.c
 * and usb.c, keeping the common protocols/busses support in this
 * file.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#ifdef OS2
#include <db.h>
#endif

#include <gphoto2.h>
#include <gphoto2-port-log.h>

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
#  define _(String) (String)
#  define N_(String) (String)
#endif

#include "../../libgphoto2/exif.h"

#include "usb.h"
#include "util.h"
#include "library.h"
#include "canon.h"
#include "serial.h"

/* These contain the default label for all the 
 * switch (camera->port->type) statements
 */
#define OTHER_PORT_TYPE_RETURN(RETVAL) \
		default: \
			gp_camera_set_error (camera, "Don't know how to handle " \
					     "camera->port->type value %i aka 0x%x" \
					     "in %s line %i.", camera->port->type, \
					     camera->port->type, __FILE__, __LINE__); \
			return (RETVAL); \
			break;

#define OTHER_PORT_TYPE OTHER_PORT_TYPE_RETURN(GP_ERROR_NOT_SUPPORTED)

/* Assertion on method parameter.
 */
#define ASSERT_PARAM(what) \
	if (!(what)) { \
		GP_DEBUG("Assertion %s failed",#what); \
		return GP_ERROR_BAD_PARAMETERS; \
	}

/*
 * does operations on a directory based on the value
 * of action : DIR_CREATE, DIR_REMOVE
 *
 */
int
canon_int_directory_operations (Camera *camera, const char *path, int action)
{
	unsigned char *msg;
	int len, canon_usb_funct;
	char type;

	switch (action) {
		case DIR_CREATE:
			type = 0x5;
			canon_usb_funct = CANON_USB_FUNCTION_MKDIR;
			break;
		case DIR_REMOVE:
			type = 0x6;
			canon_usb_funct = CANON_USB_FUNCTION_RMDIR;
			break;
		default:
			gp_debug_printf (GP_DEBUG_LOW, "canon",
					 "canon_int_directory_operations: "
					 "Bad operation specified : %i\n", action);
			return GP_ERROR_BAD_PARAMETERS;
			break;
	}

	gp_debug_printf (GP_DEBUG_LOW, "canon", "canon_int_directory_operations() "
			 "called to %s the directory '%s'",
			 canon_usb_funct == CANON_USB_FUNCTION_MKDIR ? "create" : "remove",
			 path);
	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, canon_usb_funct, &len, path,
						  strlen (path) + 1);
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, type, 0x11, &len, path,
						     strlen (path) + 1, NULL);
			break;
		OTHER_PORT_TYPE;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return GP_ERROR;
	}

	if (msg[0] != 0x00) {
		gp_debug_printf (GP_DEBUG_LOW, "canon",
				 "Could not %s directory %s",
				 canon_usb_funct ==
				 CANON_USB_FUNCTION_MKDIR ? "create" : "remove", path);
		return GP_ERROR;
	}

	return GP_OK;
}

/**
 * canon_int_identify_camera:
 * @camera: the camera to work with
 * @Returns: gphoto2 error code
 *
 * Gets the camera identification string, usually the owner name.
 *
 * This information is then stored in the "camera" structure, which 
 * is a global variable for the driver.
 *
 * This function also gets the firmware revision in the camera struct.
 **/
int
canon_int_identify_camera (Camera *camera)
{
	unsigned char *msg;
	int len;

	GP_DEBUG ("canon_int_identify_camera() called");

	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_IDENTIFY_CAMERA,
						  &len, NULL, 0);
			if (!msg)
				return GP_ERROR;
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x01, 0x12, &len, NULL);
			if (!msg) {
				gp_debug_printf (GP_DEBUG_LOW, "canon",
						 "canon_int_identify_camera: " "msg error");
				canon_serial_error_type (camera);
				return GP_ERROR;
			}
			break;
		OTHER_PORT_TYPE;
	}

	if (len != 0x4c)
		return GP_ERROR;

	/* Store these values in our "camera" structure: */
	memcpy (camera->pl->firmwrev, (char *) msg + 8, 4);
	strncpy (camera->pl->ident, (char *) msg + 12, 30);
	strncpy (camera->pl->owner, (char *) msg + 44, 30);

	GP_DEBUG ("canon_int_identify_camera: ident '%s' owner '%s'", camera->pl->ident,
		  camera->pl->owner);

	return GP_OK;
}

/**
 * canon_int_get_battery:
 * @camera: the camera to work on
 * @pwr_status: pointer to integer determining power status
 * @pwr_source: pointer to integer determining power source
 * @Returns: gphoto2 error code
 *
 * Gets battery status.
 **/
int
canon_int_get_battery (Camera *camera, int *pwr_status, int *pwr_source)
{
	unsigned char *msg;
	int len;

	GP_DEBUG ("canon_int_get_battery()");

	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_POWER_STATUS,
						  &len, NULL, 0);
			if (!msg)
				return GP_ERROR;
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x0a, 0x12, &len, NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return GP_ERROR;
			}
			break;
		OTHER_PORT_TYPE;
	}

	if (len != 8)
		return GP_ERROR;

	if (pwr_status)
		*pwr_status = msg[4];
	if (pwr_source)
		*pwr_source = msg[7];
	GP_DEBUG ("canon_int_get_battery: Status: %i / Source: %i\n", *pwr_status,
		  *pwr_source);
	return GP_OK;
}


/**
 * canon_int_set_file_attributes:
 * @camera: camera to work with
 * @file: file to work on
 * @dir: directory to work in
 * @attrs: attribute bit field
 * @Returns: gphoto2 error code
 *
 * Sets a file's attributes. See the 'Protocol' file for details.
 **/
int
canon_int_set_file_attributes (Camera *camera, const char *file, const char *dir,
			       unsigned char attrs)
{
	unsigned char payload[300];
	unsigned char *msg;
	unsigned char attr[4];
	int len, payload_length;

	GP_DEBUG ("canon_int_set_file_attributes() "
		  "called for '%s' '%s', attributes 0x%x", dir, file, attrs);

	attr[0] = attr[1] = attr[2] = 0;
	attr[3] = attrs;

	switch (camera->port->type) {
		case GP_PORT_USB:
			if ((4 + strlen (dir) + 1 + strlen (file) + 1) > sizeof (payload)) {
				GP_DEBUG ("canon_int_set_file_attributes: "
					  "dir '%s' + file '%s' too long, "
					  "won't fit in payload buffer.", dir, file);
				return GP_ERROR_BAD_PARAMETERS;
			}
			/* create payload (yes, path and filename are two different strings
			 * and not meant to be concatenated)
			 */
			memset (payload, 0, sizeof (payload));
			memcpy (payload, attr, 4);
			memcpy (payload + 4, dir, strlen (dir) + 1);
			memcpy (payload + 4 + strlen (dir) + 1, file, strlen (file) + 1);
			payload_length = 4 + strlen (dir) + 1 + strlen (file) + 1;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_SET_ATTR, &len,
						  payload, payload_length);
			if (len == 4) {
				/* XXX check camera return value (not canon_usb_dialogue return value
				 * but the bytes in the packet returned)
				 */
				gp_debug_printf (GP_DEBUG_LOW, "canon",
						 "canon_int_set_file_attributes: "
						 "returned four bytes as expected, "
						 "we should check if they indicate "
						 "error or not. Returned data :");
				gp_log_data ("canon", msg, 4);
			} else {
				gp_debug_printf (GP_DEBUG_LOW, "canon",
						 "canon_int_set_file_attributes: "
						 "setting attribute failed!");
				return GP_ERROR;
			}

			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0xe, 0x11, &len, attr, 4, dir,
						     strlen (dir) + 1, file, strlen (file) + 1,
						     NULL);
			break;
		OTHER_PORT_TYPE;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return GP_ERROR;
	}

	return GP_OK;
}

/**
 * canon_int_set_owner_name:
 * @camera: the camera to set the owner name of
 * @name: owner name to set the camera to
 *
 * Sets the camera owner name. The string should not be more than 30
 * characters long. We call #get_owner_name afterwards in order to
 * check that everything went fine.
 **/
int
canon_int_set_owner_name (Camera *camera, const char *name)
{
	unsigned char *msg;
	int len;

	gp_debug_printf (GP_DEBUG_LOW, "canon", "canon_int_set_owner_name() "
			 "called, name = '%s'", name);
	if (strlen (name) > 30) {
		gp_debug_printf (GP_DEBUG_LOW, "canon",
				 "canon_int_set_owner_name: Name too long (%i chars), "
				 "max is 30 characters!", strlen (name));
		gp_camera_status (camera, _("Name too long, max is 30 characters!"));
		return 0;
	}

	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_CAMERA_CHOWN,
						  &len, name, strlen (name) + 1);
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x05, 0x12, &len, name,
						     strlen (name) + 1, NULL);
			break;
		OTHER_PORT_TYPE;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return GP_ERROR;
	}
	return canon_int_identify_camera (camera);
}


/**
 * canon_int_get_time:
 * @camera: camera to get the current time of
 * @Returns: time of camera (local time)
 *
 * Get camera's current time.
 *
 * The camera gives time in little endian format, therefore we need
 * to swap the 4 bytes on big-endian machines.
 *
 * Note: the time returned is not GMT but local time. Therefore,
 * if you use functions like "ctime", it will be translated to local
 * time _a second time_, and the result will be wrong. Only use functions
 * that don't translate the date into localtime, like "gmtime".
 **/
time_t
canon_int_get_time (Camera *camera)
{
	unsigned char *msg;
	int len;
	int t;
	time_t date;

	GP_DEBUG ("canon_int_get_time()");

	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_GET_TIME, &len,
						  NULL, 0);
			if (!msg)
				return GP_ERROR;
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x03, 0x12, &len, NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return GP_ERROR;
			}
			break;
		OTHER_PORT_TYPE;
	}

	if (len != 0x10)
		return GP_ERROR;

	/* XXX will fail when sizeof(int) != 4. Should use u_int32_t or
	 * something instead. Investigate portability issues.
	 */
	memcpy (&t, msg + 4, 4);

	date = (time_t) byteswap32 (t);

	/* XXX should strip \n at the end of asctime() return data */
	GP_DEBUG ("Camera time: %s ", asctime (gmtime (&date)));
	return date;
}


int
canon_int_set_time (Camera *camera)
{
	unsigned char *msg;
	int len, i;
	time_t date;
	char pcdate[4];

	date = time (NULL);
	for (i = 0; i < 4; i++)
		pcdate[i] = (date >> (8 * i)) & 0xff;

	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_SET_TIME, &len,
						  NULL, 0);
			if (!msg)
				return GP_ERROR;
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x04, 0x12, &len, pcdate,
						     sizeof (pcdate),
						     "\x00\x00\x00\x00\x00\x00\x00\x00", 8,
						     NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return GP_ERROR;
			}

			break;
		OTHER_PORT_TYPE;
	}

	if (len != 0x10)
		return GP_ERROR;

	return GP_OK;
}

/**
 * canon_int_ready:
 * @camera: camera to get ready
 * @Returns: gphoto2 error code
 *
 * Switches the camera on, detects the model and sets its speed. This
 * function must be called before doing anything on the camera. This
 * function should be called before every action on the camera for
 * RS232 connections, as these tend to be unreliable.
 **/
int
canon_int_ready (Camera *camera)
{
	int res;

	GP_DEBUG ("canon_int_ready()");

	switch (camera->port->type) {
		case GP_PORT_USB:
			res = canon_usb_ready (camera);
			break;
		case GP_PORT_SERIAL:
			res = canon_serial_ready (camera);
			break;
		OTHER_PORT_TYPE;
	}

	return (res);
}

/**
 * canon_int_get_disk_name:
 * @camera: camera to ask for disk drive
 * @Returns: name of disk
 *
 * Ask the camera for the name of the flash storage
 * device. Usually "D:" or something like that.
 **/
char *
canon_int_get_disk_name (Camera *camera)
{
	unsigned char *msg;
	int len, res;

	GP_DEBUG ("canon_int_get_disk_name()");

	switch (camera->port->type) {
		case GP_PORT_USB:
			res = canon_usb_long_dialogue (camera,
						       CANON_USB_FUNCTION_FLASH_DEVICE_IDENT,
						       &msg, &len, 1024, NULL, 0, 0);
			if (res != GP_OK) {
				GP_DEBUG ("canon_int_get_disk_name: canon_usb_long_dialogue "
					  "failed! returned %i", res);
				return NULL;
			}
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x0a, 0x11, &len, NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return NULL;
			}
			break;
		OTHER_PORT_TYPE_RETURN(NULL);
	}

	switch (camera->port->type) {
		case GP_PORT_SERIAL:
			/* this is correct even though it looks a bit funny. canon_serial_dialogue()
			 * has a static buffer, strdup() part of that buffer and return to our caller.
			 */
			msg = strdup ((char *) msg + 4);	/* @@@ should check length */
			if (!msg) {
				GP_DEBUG ("canon_int_get_disk_name: could not allocate %i "
					  "bytes of memory to hold response",
					  strlen ((char *) msg + 4));
				return NULL;
			}
			break;
		case GP_PORT_USB:
			break;
		OTHER_PORT_TYPE_RETURN(NULL);
	}

	GP_DEBUG ("canon_int_get_disk_name: disk '%s'", msg);

	return msg;
}

/**
 * canon_int_get_disk_name_info:
 * @camera: camera to ask about disk
 * @name: name of the disk
 * @capacity: returned maximum disk capacity
 * @available: returned currently available disk capacity
 * @Returns: boolean value denoting success (FIXME: ATTENTION!)
 *
 * Gets available room and max capacity of a disk given by @name.
 **/
int
canon_int_get_disk_name_info (Camera *camera, const char *name, int *capacity, int *available)
{
	unsigned char *msg;
	int len, cap, ava;

	GP_DEBUG ("canon_int_get_disk_name_info() name '%s'", name);

	if (name == NULL) {
		gp_camera_set_error (camera, "NULL name in canon_int_get_disk_name_info");
		return GP_ERROR_BAD_PARAMETERS;
	}
	if (capacity == NULL) {
		gp_camera_set_error (camera, "NULL capacity in canon_int_get_disk_name_info");
		return GP_ERROR_BAD_PARAMETERS;
	}
	if (available == NULL) {
		gp_camera_set_error (camera, "NULL available in canon_int_get_disk_name_info");
		return GP_ERROR_BAD_PARAMETERS;
	}

	switch (camera->port->type) {
		case GP_PORT_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_DISK_INFO, &len,
						  name, strlen (name) + 1);
			if (!msg)
				return GP_ERROR;
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x09, 0x11, &len, name,
						     strlen (name) + 1, NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return GP_ERROR;
			}
			break;
		OTHER_PORT_TYPE;
	}

	if (len < 12) {
		GP_DEBUG ("ERROR: truncated message");
		return GP_ERROR;
	}
	cap = get_int (msg + 4);
	ava = get_int (msg + 8);
	if (capacity)
		*capacity = cap;
	if (available)
		*available = ava;

	GP_DEBUG ("canon_int_get_disk_name_info: capacity %i kb, available %i kb",
		  cap > 0 ? (cap / 1024) : 0, ava > 0 ? (ava / 1024) : 0);

	return GP_OK;
}


/**
 * gphoto2canonpath:
 * @camera: the camera the path is on (to determine drive letter)
 * @path: gphoto2 path 
 *
 * convert gphoto2 path  (e.g.   "/DCIM/116CANON/IMG_1240.JPG")
 * into canon style path (e.g. "D:\DCIM\116CANON\IMG_1240.JPG")
 * could it be that old (serial) cameras use pathes like
 * "\DCIM\116CANON\IMG_1234.JPG" ?
 **/
const char *
gphoto2canonpath (Camera *camera, const char *path)
{
	static char tmp[2000];
	char *p;

	if (path[0] != '/') {
		return NULL;
	}
	if (camera->pl->cached_drive == NULL) {
		GP_DEBUG ("NULL camera->pl->cached_drive in gphoto2canonpath");
		camera->pl->cached_drive = canon_int_get_disk_name (camera);
		if (camera->pl->cached_drive == NULL) {
			GP_DEBUG ("NULL camera->pl->cached_drive fix failed "
				  "in gphoto2canonpath");
		}
	}
	strcpy (tmp, camera->pl->cached_drive);
	strcat (tmp, path);
	for (p = tmp; *p != '\0'; p++) {
		if (*p == '/')
			*p = '\\';
	}
	/* remove trailing backslash */
	if ((p > tmp) && (*(p - 1) == '\\'))
		*(p - 1) = '\0';
	GP_LOG (GP_LOG_DATA, "gphoto2canonpath: converted '%s' to '%s'", path, tmp);
	return (tmp);
}

/**
 * canon2gphotopath:
 * @camera: the camera the path is on (to determine drive letter)
 * @path: canon style path
 *
 * convert canon style path (e.g. "D:\DCIM\116CANON\IMG_1240.JPG")
 * into gphoto2 path        (e.g.   "/DCIM/116CANON/IMG_1240.JPG")
 **/
const char *
canon2gphotopath (Camera *camera, const char *path)
{
	static char tmp[2000];
	char *p;

	if (!((path[1] == ':') && (path[2] == '\\'))) {
		return NULL;
	}
	// FIXME: just drops the drive letter
	p = strchr (path, ':');
	p++;
	strcpy (tmp, p);
	for (p = tmp; *p != '\0'; p++) {
		if (*p == '\\')
			*p = '/';
	}
	GP_LOG (GP_LOG_DATA, "canon2gphotopath: converted '%s' to '%s'", path, tmp);
	return (tmp);
}


void
debug_fileinfo (CameraFileInfo * info)
{
	GP_DEBUG ("<CameraFileInfo>");
	GP_DEBUG ("  <CameraFileInfoFile>");
	if ((info->file.fields & GP_FILE_INFO_NAME) != 0)
		GP_DEBUG ("    Name:   %s", info->file.name);
	if ((info->file.fields & GP_FILE_INFO_TYPE) != 0)
		GP_DEBUG ("    Type:   %s", info->file.type);
	if ((info->file.fields & GP_FILE_INFO_SIZE) != 0)
		GP_DEBUG ("    Size:   %i", info->file.size);
	if ((info->file.fields & GP_FILE_INFO_WIDTH) != 0)
		GP_DEBUG ("    Width:  %i", info->file.width);
	if ((info->file.fields & GP_FILE_INFO_HEIGHT) != 0)
		GP_DEBUG ("    Height: %i", info->file.height);
	if ((info->file.fields & GP_FILE_INFO_PERMISSIONS) != 0)
		GP_DEBUG ("    Perms:  0x%x", info->file.permissions);
	if ((info->file.fields & GP_FILE_INFO_STATUS) != 0)
		GP_DEBUG ("    Status: %i", info->file.status);
	if ((info->file.fields & GP_FILE_INFO_TIME) != 0) {
		char *p, *time = asctime (gmtime (&info->file.time));

		for (p = time; *p != 0; ++p)
			/* do nothing */ ;
		*(p - 1) = '\0';
		GP_DEBUG ("    Time:   %s (%i)", time, info->file.time);
	}
	GP_DEBUG ("  </CameraFileInfoFile>");
	GP_DEBUG ("</CameraFileInfo>");
}


/**
 * canon_int_list_directory:
 * @camera: the camera we are using
 * @list: the list we should append the direntry names to
 * @folder: the gphoto2 style path of the folder we are to list
 * @flags: determines what to list: CANON_LIST_FILES or CANON_LIST_FOLDERS
 *
 * List all files within a given folder, append their names to the
 * given @filelist and set the file info using
 * #gp_filesystem_set_info_noop
 *
 * This method is really long and should probably reorganized and
 * separated into multiple methods.
 **/
int
canon_int_list_directory (Camera *camera, const char *folder, CameraList *list,
			  const int flags)
{
	CameraFileInfo info;
	int res;
	unsigned int dirents_length;
	unsigned char *dirent_data = NULL;
	unsigned char *end_of_data, *temp_ch, *pos;
#ifdef CANON_FLATTEN
	const char *canonfolder = gphoto2canonpath (camera, (camera->pl->flatten_folders?"/":folder));
#else
	const char *canonfolder = gphoto2canonpath (camera, folder);	
#endif
	int list_files = ((flags & CANON_LIST_FILES) != 0);
	int list_folders = ((flags & CANON_LIST_FOLDERS) != 0);

	canon_dirent *dirent = NULL;	/* current directory entry */

	GP_DEBUG ("BEGIN can_int_list_dir() folder '%s' aka '%s' (%s, %s)",
		  folder, canonfolder,
		  list_files ? "files" : "no files", list_folders ? "folders" : "no folders");

	/* Fetch all directory entrys from the camera */
	switch (camera->port->type) {
		case GP_PORT_USB:
			res = canon_usb_get_dirents (camera, &dirent_data, &dirents_length,
						     canonfolder);
			break;
		case GP_PORT_SERIAL:
			res = canon_serial_get_dirents (camera, &dirent_data, &dirents_length,
							canonfolder);
			break;
		OTHER_PORT_TYPE;
	}
	if (res != GP_OK)
		return res;

	end_of_data = dirent_data + dirents_length;

	if (dirents_length < CANON_MINIMUM_DIRENT_SIZE) {
		gp_camera_set_error (camera, "can_int_list_dir: ERROR: "
				     "initial message too short (%i < minimum %i)",
				     dirents_length, CANON_MINIMUM_DIRENT_SIZE);
		free (dirent_data);
		return GP_ERROR;
	}

	/* The first data we have got here is the dirent for the
	 * directory we are reading. Skip over 10 bytes
	 * (2 for attributes, 4 date and 4 size) and then go find
	 * the end of the directory name so that we get to the next
	 * dirent which is actually the first one we are interested
	 * in
	 */
	dirent = (canon_dirent *) dirent_data;

	GP_DEBUG ("can_int_list_dir: "
		  "Camera directory listing for directory '%s'", dirent->name);

	for (pos = dirent->name; pos < end_of_data && *pos != 0; pos++)
		/* do nothing */ ;
	if (pos == end_of_data || *pos != 0) {
		gp_camera_set_error (camera, "can_int_list_dir: "
				     "Reached end of packet while "
				     "examining the first dirent");
		free (dirent_data);
		return GP_ERROR;
	}
	pos++;			/* skip NULL byte terminating directory name */

	/* we are now positioned at the first interesting dirent */

	/* This is the main loop, for every directory entry returned */
	while (pos < end_of_data) {
		int is_dir, is_file;
		unsigned int direntnamelen;	/* length of dirent->name */
		unsigned int direntsize;	/* size of dirent in octets */

		dirent = (canon_dirent *) pos;
		is_dir = ((dirent->attrs & CANON_ATTR_NON_RECURS_ENT_DIR) != 0)
			|| ((dirent->attrs & CANON_ATTR_RECURS_ENT_DIR) != 0);
		is_file = !is_dir;

		GP_LOG (GP_LOG_DATA, "can_int_list_dir: "
			"reading dirent at position %i of %i (0x%x of 0x%x)",
			(pos - dirent_data), (end_of_data - dirent_data),
			(pos - dirent_data), (end_of_data - dirent_data)
			);

		if (pos + sizeof (canon_dirent) > end_of_data) {
			/* handle (possible) error case */
			if (camera->port->type == GP_PORT_SERIAL) {
				/* check to see if it is only NULL bytes left,
				 * that is not an error for serial cameras
				 * (at least the A50 adds five zero bytes at the end)
				 */
				for (temp_ch = pos; temp_ch < end_of_data && *temp_ch; temp_ch++)
					;	/* do nothing */

				if (temp_ch == end_of_data) {
					GP_DEBUG ("can_int_list_dir: "
						  "the last %i bytes were all 0 - ignoring.",
						  temp_ch - pos);
					break;
				} else {
					GP_DEBUG ("can_int_list_dir: "
						  "byte[%i=0x%x] == %i=0x%x", temp_ch - pos,
						  temp_ch - pos, *temp_ch, *temp_ch);
					GP_DEBUG ("can_int_list_dir: "
						  "pos is 0x%x, end_of_data is 0x%x, temp_ch is 0x%x - diff is 0x%x",
						  pos, end_of_data, temp_ch, temp_ch - pos);
				}
			}
			GP_DEBUG ("can_int_list_dir: "
				  "dirent at position %i=0x%x of %i=0x%x is too small, "
				  "minimum dirent is %i bytes",
				  (pos - dirent_data), (pos - dirent_data),
				  (end_of_data - dirent_data), (end_of_data - dirent_data),
				  CANON_MINIMUM_DIRENT_SIZE);
			gp_camera_set_error (camera,
					     "can_int_list_dir: "
					     "truncated directory entry encountered");
			free (dirent_data);
			return GP_ERROR;
		}

		/* Check end of this dirent, 10 is to skip over
		 * 2    attributes + 0x00
		 * 4    file date (UNIX localtime)
		 * 4    file size
		 * to where the direntry name begins.
		 */
		for (temp_ch = dirent->name; temp_ch < end_of_data && *temp_ch != 0;
		     temp_ch++) ;

		if (temp_ch == end_of_data || *temp_ch != 0) {
			GP_DEBUG ("can_int_list_dir: "
				  "dirent at position %i of %i has invalid name in it."
				  "bailing out with what we've got.",
				  (pos - dirent_data), (end_of_data - dirent_data));
			break;
		}
		direntnamelen = strlen (dirent->name);
		direntsize = sizeof (canon_dirent) + direntnamelen;

		/* check that length of name in this dirent is not of unreasonable size.
		 * 256 was picked out of the blue
		 */
		if (direntnamelen > 256) {
			GP_DEBUG ("can_int_list_dir: "
				  "dirent at position %i of %i has too long name in it (%i bytes)."
				  "bailing out with what we've got.",
				  (pos - dirent_data), (end_of_data - dirent_data),
				  direntnamelen);
			break;
		}

		/* 10 bytes of attributes, size and date, a name and a NULL terminating byte */
		/* don't use GP_DEBUG since we log this with GP_LOG_DATA */
		GP_LOG (GP_LOG_DATA, "can_int_list_dir: "
			"dirent determined to be %i=0x%x bytes :", direntsize, direntsize);
		gp_log_data ("canon", pos, direntsize);
		if (direntnamelen) {
			if ((list_folders && is_dir) || (list_files && is_file)) {

				/* we're going to fill out the info structure
				   in this block */
				memset (&info, 0, sizeof (info));

				/* we start with nothing and continously add stuff */
				info.file.fields = GP_FILE_INFO_NONE;

				/* OK, this directory entry has a name in it. */
				strncpy (info.file.name, dirent->name, sizeof (info.file.name));
				info.file.fields |= GP_FILE_INFO_NAME;

				/* the date is located at offset 6 and is 4
				 * bytes long, re-order little/big endian */
				info.file.time = byteswap32 (dirent->datetime);
				if (info.file.time != 0)
					info.file.fields |= GP_FILE_INFO_TIME;

				if (is_file) {
					/* determine file type based on file name
					 * this stuff only makes sense for files, not for folders
					 */

					strncpy (info.file.type, filename2mimetype (info.file.name),
						 sizeof (info.file.type));
					info.file.fields |= GP_FILE_INFO_TYPE;

					if ((dirent->attrs & CANON_ATTR_DOWNLOADED) == 0)
						info.file.status = GP_FILE_STATUS_DOWNLOADED;
					else
						info.file.status = GP_FILE_STATUS_NOT_DOWNLOADED;
					info.file.fields |= GP_FILE_INFO_STATUS;

					/* the size is located at offset 2 and is 4
					 * bytes long, re-order little/big endian */
					info.file.size = byteswap32 (dirent->size);
					info.file.fields |= GP_FILE_INFO_SIZE;

					/* file access modes */
					if ((dirent->attrs & CANON_ATTR_WRITE_PROTECTED) == 0)
						info.file.permissions = GP_FILE_PERM_READ |
							GP_FILE_PERM_DELETE;
					else
						info.file.permissions = GP_FILE_PERM_READ;
					info.file.fields |= GP_FILE_INFO_PERMISSIONS;
				}

				/* print dirent as text */
				GP_DEBUG ("Raw info: name=%s is_dir=%i, is_file=%i, attrs=0x%x",
					  dirent->name, is_dir, is_file, dirent->attrs);
				debug_fileinfo (&info);

				if (is_file) {
					/*
					 * Append directly to the filesystem instead of to the list,
					 * because we have additional information.
					 */
					if (!camera->pl->list_all_files && 
					    !is_image(info.file.name) &&
					    !is_movie(info.file.name) ) {
						/* do nothing */
						GP_DEBUG ("Ignored %s/%s", folder, info.file.name);
					} else {
						GP_DEBUG ("Added file %s/%s", folder, info.file.name);
						gp_filesystem_append (camera->fs, folder, info.file.name);
						gp_filesystem_set_info_noop (camera->fs, folder, info);
					}
				}
				if (is_dir) {
					gp_list_append (list, info.file.name, NULL);
				}
			} else {
				/* this case could mean that this was the last dirent */
				GP_DEBUG ("can_int_list_dir: "
					  "dirent at position %i of %i has NULL name, skipping.",
					  (pos - dirent_data), (end_of_data - dirent_data));
			}
		}
		
		/* make 'pos' point to next dirent in packet.
		 * first we skip 10 bytes of attribute, size and date,
		 * then we skip the name plus 1 for the NULL
		 * termination bytes.
		 */
		pos += direntsize;
	}
	free (dirent_data);

	GP_DEBUG ("<FILESYSTEM-DUMP>");
	gp_filesystem_dump (camera->fs);
	GP_DEBUG ("</FILESYSTEM-DUMP>");

	GP_DEBUG ("END can_int_list_dir() folder '%s' aka '%s'", folder, canonfolder);

	return GP_OK;
}

/**
 * canon_int_get_file:
 * @camera:
 * @name: canon style path of file
 * @data: pointer to data pointer
 * @length: pointer to size of data
 *
 * Just a wrapper for the port specific routines #canon_usb_get_file
 * and #canon_serial_get_file
 **/

int
canon_int_get_file (Camera *camera, const char *name, unsigned char **data, int *length)
{
	switch (camera->port->type) {
		case GP_PORT_USB:
		        return canon_usb_get_file (camera, name, data, length);
			break;
		case GP_PORT_SERIAL:
 		        return canon_serial_get_file (camera, name, data, length);
			break;
		OTHER_PORT_TYPE;
	}
	/* never reached */
	return GP_ERROR;
}

/**
 * canon_int_handle_jfif_thumb:
 *
 * extract thumbnail from JFIF image (A70)
 * just extracted the code from the old #canon_int_get_thumbnail
 **/

static int
canon_int_handle_jfif_thumb(const unsigned int total, unsigned char **data)
{
	int i, j, in;
	unsigned char *thumb;
	ASSERT_PARAM(data != NULL);
	*data = NULL;
	/* pictures are JFIF files */
	/* we skip the first FF D8 */
	i = 3;
	j = 0;
	in = 0;

	/* we want to drop the header to get the thumbnail */

	thumb = malloc (total);
	if (!thumb) {
		perror ("malloc");
		return GP_ERROR_NO_MEMORY;
	}

	while (i < total) {
		if (*data[i] == JPEG_ESC) {
			if (*data[i + 1] == JPEG_BEG &&
			    ((*data[i + 3] == JPEG_SOS)
			     || (*data[i + 3] == JPEG_A50_SOS))) {
				in = 1;
			} else if (*data[i + 1] == JPEG_END) {
				in = 0;
				thumb[j++] = *data[i];
				thumb[j] = *data[i + 1];
				*data = thumb;
				return GP_OK;
			}
		}

		if (in == 1)
			thumb[j++] = *data[i];
		i++;

	}
	return GP_ERROR;
}

/**
 * canon_int_handle_exif_thumb:
 *
 * Get information and thumbnail data from EXIF thumbnail.
 * just extracted the code from the old #canon_int_get_thumbnail
 **/
static int
canon_int_handle_exif_thumb (unsigned char *data, const char *name, 
			     const unsigned int length, unsigned char **retdata) {
	exifparser exifdat;

	ASSERT_PARAM(data != NULL);
        ASSERT_PARAM(retdata != NULL);

	exifdat.header = data;
	exifdat.data = data + 12;

	GP_DEBUG ("Got thumbnail, extracting it with the " "EXIF lib.");
	if (exif_parse_data (&exifdat) > 0) {
		GP_DEBUG ("Parsed exif data.");
		data = exif_get_thumbnail (&exifdat);	// Extract Thumbnail
		if (data == NULL) {
			int f;
			char filename[255];

			if (rindex (name, '\\') != NULL)
				snprintf (filename, sizeof (filename) - 1,
					  "canon-death-dump.dat-%s",
					  rindex (name, '\\') + 1);
			else
				snprintf (filename, sizeof (filename) - 1,
					  "canon-death-dump.dat-%s", name);
			filename[sizeof (filename) - 1] = '\0';

			GP_DEBUG ("canon_int_handle_exif_thumb: "
				  "Thumbnail conversion error, saving "
				  "%i bytes to file '%s'", 
				  length, filename);
			/* create with O_EXCL and 0600 for security */
			if ((f =
			     open (filename, 
				   O_CREAT | O_EXCL | O_RDWR,
				   0600)) == -1) {
				/* XXX Is %m portable to non-glibc
				 * systems? */
				GP_DEBUG ("canon_int_handle_exif_thumb: "
					  "error creating file '%s': %m",
					  filename);
				return GP_ERROR;
			}
			if (write (f, data, length) == -1) {
				GP_DEBUG ("canon_int_handle_exif_thumb: "
					  "error writing to file '%s': %m",
					  filename);
			}

			close (f);
			return GP_ERROR;
		}
		*retdata = data;
		return GP_OK;
	}
	GP_DEBUG ("couldn't parse exif thumbnail data");
	return GP_ERROR;
}

/**
 * canon_int_get_thumbnail:
 * @camera: camera to work with
 * @name: image to get thumbnail of
 * @length: length of data returned
 *
 * Returns the thumbnail data of the picture designated by @name.
 **/
int 
canon_int_get_thumbnail (Camera *camera, const char *name, unsigned char **retdata, int *length)
{
	int res;
	unsigned char *data = NULL;

	GP_DEBUG ("canon_int_get_thumbnail() called for file '%s'", name);
	ASSERT_PARAM(retdata != NULL);
	ASSERT_PARAM(length != NULL);

	gp_camera_progress (camera, 0);
	switch (camera->port->type) {
		case GP_PORT_USB:
			res = canon_usb_get_thumbnail (camera, name, &data, length);
			break;
		case GP_PORT_SERIAL:
			res = canon_serial_get_thumbnail (camera, name, &data, length);
			break;
		OTHER_PORT_TYPE;
	}
	if (res != GP_OK) {
		GP_DEBUG ("canon_port_get_thumbnail() failed, "
			  "returned %i", res);
		return res;
	}

	switch (camera->pl->model) {
		/* We should decide this not on model base. Better use
		 * capabilities or just have a look at the data itself
		 */
		case CANON_PS_A70:
			res = canon_int_handle_jfif_thumb(*length, &data);
			break;

		default:
			res = canon_int_handle_exif_thumb (data, name, *length, retdata);
			break;
	}

	free (data);
	return res;
}

/**
 * canon_int_filename2thumbname:
 * @filename: file name on camera
 * @Returns: file name of corresponding thumbnail if it exists, NULL else
 *
 * Determine name of corresponding thumbnail file.
 *
 * XXX We should use information about the camera type to
 * determine for what kinds of files the thumbnail is located
 * in an extra file. Until then, we just replace .XXX by .THM
 * and return that string.
 **/

/* simulate capabilities */
#define extra_file_for_thumb_of_jpeg (0 == 1)
#define extra_file_for_thumb_of_crw (0 == 0)

const char *
canon_int_filename2thumbname (Camera *camera, const char *filename)
{
	static char buf[1024];
	char *p;

	/* First handle cases where we shouldn't try to get extra .THM
	 * file but use the special get_thumbnail_of_xxx function.
	 */
	if (!extra_file_for_thumb_of_jpeg && is_jpeg (filename))
		return NULL;
	if (!extra_file_for_thumb_of_crw  && is_crw (filename))
		return NULL;

	/* We use the thumbnail file itself as the thumbnail of the
	 * thumbnail file. In short thumbfile = thumbnail(thumbfile)
	 */
	if (is_thumbnail(filename))
		return filename;

	/* We just replace file ending by .THM and assume this is the
	 * name of the thumbnail file.
	 */
	if (strncpy (buf, filename, sizeof(buf)) < 0) {
		GP_DEBUG ("Buffer too small in %s line %i.", 
			  __FILE__, __LINE__);
		return NULL;
	}
	if ((p = strrchr(buf, '.')) == NULL) {
		GP_DEBUG ("No '.' found in filename '%s' in %s line %i.",
			  filename, __FILE__, __LINE__);
		return NULL;
	}
	if (((p - buf) < sizeof(buf) - 4) && strncpy (p, ".THM", 4)) {
		GP_DEBUG ("Thumbnail name for '%s' is '%s'",
			  filename, buf);
		return buf;
	} else {
		GP_DEBUG ("Thumbnail name for filename '%s' doesnt fit in %s line %i.",
			  filename, __FILE__, __LINE__);
		return NULL;
	}
	/* never reached */
	return NULL;
}

/**
 * canon_int_delete_file:
 * @camera: the camera to work on
 * @folder: the canon path of the folder where the file to delete is located
 * @filename: the name of the file in the folder
 *
 * Deletes the file named @filename in the folder @folder.
 **/
int
canon_int_delete_file (Camera *camera, const char *folder, const char *filename)
{
	unsigned char payload[300];
	unsigned char *msg;
	int len, payload_length;

	switch (camera->port->type) {
		case GP_PORT_USB:
			memccpy (payload, folder, strlen (folder) + 1, sizeof(payload));
			memccpy (payload + strlen(folder) + 1, filename, 
				 strlen (filename) + 1, sizeof(payload) - strlen(folder) - 1);
			payload_length = strlen (folder) + strlen(filename) + 2;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_DELETE_FILE, &len,
						  payload, payload_length);
			if (! msg)
				return GP_ERROR;
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0xd, 0x11, &len,
						     folder, strlen (folder) + 1,
						     filename, strlen (filename) + 1,
						     NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return GP_ERROR;
			}
			break;
		OTHER_PORT_TYPE;
	}
	
	if (len != 4) {
		/* XXX should mark folder as dirty since we can't be sure if the file
		 * got deleted or not
		 */
		return GP_ERROR;
	}
	
	if (msg[0] == 0x29) {
		gp_camera_message (camera, _("File protected"));
		return GP_ERROR;
	}

	/* XXX we should mark folder as dirty, re-read it and check if the file
	 * is gone or not.
	 */
	return GP_OK;
}

/**
 * canon_int_put_file:
 *
 * Upload a file to the camera. Just a wrapper for port specific functions.
 */
int
canon_int_put_file (Camera *camera, CameraFile *file, char *destname, char *destpath)
{

	switch (camera->port->type) {
		case GP_PORT_USB:
			return canon_usb_put_file (camera, file, destname, destpath);
			break;
		case GP_PORT_SERIAL:
			return canon_serial_put_file (camera, file, destname, destpath);
			break;
		OTHER_PORT_TYPE;
	}
}

/*
 * Local Variables:
 * c-file-style:"linux"
 * indent-tabs-mode:t
 * End:
 */
