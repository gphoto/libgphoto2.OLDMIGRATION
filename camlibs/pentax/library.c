/* Pentax K series library
 *
 * Copyright (c) 2011 Marcus Meissner <meissner@suse.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "config.h"

#define GP_MODULE "pentax"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <gphoto2/gphoto2-library.h>
#include <gphoto2/gphoto2-result.h>
#include <gphoto2/gphoto2-port.h>
#include <gphoto2/gphoto2-setting.h>
#include "pslr.h"

#ifdef ENABLE_NLS
#  include <libintl.h>
#  undef _
#  define _(String) dgettext (GETTEXT_PACKAGE, String)
#  ifdef gettext_noop
#    define N_(String) gettext_noop (String)
#  else
#    define N_(String) (String)
#  endif
#else
#  define _(String) (String)
#  define N_(String) (String)
#endif

int
camera_id (CameraText *id)
{
	strcpy (id->text, "pentax");
	return GP_OK;
}

int
camera_abilities (CameraAbilitiesList *list)
{
	CameraAbilities a;

	memset (&a, 0, sizeof(a));
	strcpy (a.model, "Pentax:K20D");
	a.status		= GP_DRIVER_STATUS_EXPERIMENTAL;
	a.port			= GP_PORT_USB_SCSI;
	a.speed[0]		= 0;
	a.usb_vendor		= 0x0a17;
	a.usb_product		= 0x0091;
	a.operations 		= GP_OPERATION_CAPTURE_IMAGE | GP_OPERATION_CONFIG;
	a.folder_operations	= GP_FOLDER_OPERATION_NONE;
	a.file_operations	= GP_FILE_OPERATION_DELETE;
	return gp_abilities_list_append (list, a);
}

static int
camera_summary (Camera *camera, CameraText *summary, GPContext *context)
{
	sprintf (summary->text, _(
		"Pentax K DSLR capture driver.\n"
		"Based in pkremote by Pontus Lidman.\n"
	));
	return GP_OK;
}

static int
get_file_func (CameraFilesystem *fs, const char *folder, const char *filename,
	       CameraFileType type, CameraFile *file, void *data,
	       GPContext *context)
{
	return GP_ERROR_NOT_SUPPORTED;
}

static int
file_list_func (CameraFilesystem *fs, const char *folder, CameraList *list,
		void *data, GPContext *context)
{
	return GP_OK;
}

static int
delete_file_func (CameraFilesystem *fs, const char *folder,
                        const char *filename, void *data, GPContext *context)
{
	/* virtual file created by Penta capture */
	if (!strncmp (filename, "capt", 4))
		return GP_OK;
	return GP_ERROR_NOT_SUPPORTED;
}


static CameraFilesystemFuncs fsfuncs = {
	.file_list_func = file_list_func,
	.get_file_func = get_file_func,
	.del_file_func = delete_file_func
};

static int
save_buffer(pslr_handle_t camhandle, int bufno, CameraFile *file, pslr_status *status)
{
	int imagetype;
	uint8_t buf[65536];
	uint32_t length;
	uint32_t current;

	if (status->image_format != PSLR_IMAGE_FORMAT_JPEG) {       
		gp_log (GP_LOG_ERROR, "pentax", "Sorry, don't know how to make capture work with RAW format yet :(\n");
		return GP_ERROR_NOT_SUPPORTED;
	}
	imagetype = status->jpeg_quality + 1;
	GP_DEBUG("get buffer %d type %d res %d\n", bufno, imagetype, status->jpeg_resolution);

	if ( pslr_buffer_open(camhandle, bufno, imagetype, status->jpeg_resolution) != PSLR_OK)
		return GP_ERROR;

	length = pslr_buffer_get_size(camhandle);
	current = 0;
	while (1) {
		uint32_t bytes;
		bytes = pslr_buffer_read(camhandle, buf, sizeof(buf));
		if (bytes == 0)
			break;
		gp_file_append (file, (char*)buf, bytes);
		current += bytes;
	}
	pslr_buffer_close(camhandle);
	return current;
}


static int
camera_capture (Camera *camera, CameraCaptureType type, CameraFilePath *path,
                GPContext *context)
{
	pslr_handle_t		p = camera->pl;
	pslr_status		status;
	int			ret, length;
	CameraFile		*file = NULL;
	CameraFileInfo		info;
	static int 		capcnt = 0;

	pslr_get_status (p, &status);
	pslr_shutter (p);

	strcpy (path->folder, "/");
	sprintf (path->name, "capt%04d.jpg", capcnt++);

	ret = gp_file_new(&file);
	if (ret!=GP_OK) return ret;
	gp_file_set_mtime (file, time(NULL));
	gp_file_set_mime_type (file, GP_MIME_JPEG);

	while (1) {
		length = save_buffer( p, (int)0, file, &status);
		if (length == GP_ERROR_NOT_SUPPORTED) return length;
		if (length >= GP_OK)
			break;
		usleep(100000);
	}
	pslr_delete_buffer(p, (int)0 );

	gp_log (GP_LOG_DEBUG, "pentax", "append image to fs");
	ret = gp_filesystem_append(camera->fs, path->folder, path->name, context);
        if (ret != GP_OK) {
		gp_file_free (file);
		return ret;
	}
	gp_log (GP_LOG_DEBUG, "pentax", "adding filedata to fs");
	ret = gp_filesystem_set_file_noop(camera->fs, path->folder, path->name, GP_FILE_TYPE_NORMAL, file, context);
        if (ret != GP_OK) {
		gp_file_free (file);
		return ret;
	}

	/* We have now handed over the file, disclaim responsibility by unref. */
	gp_file_unref (file);

	/* we also get the fs info for free, so just set it */
	info.file.fields = GP_FILE_INFO_TYPE |
			GP_FILE_INFO_SIZE | GP_FILE_INFO_MTIME;
	strcpy (info.file.type, GP_MIME_JPEG);
	info.file.size		= length;
	info.file.mtime		= time(NULL);

	info.preview.fields = 0;
	gp_log (GP_LOG_DEBUG, "pentax", "setting fileinfo in fs");
	ret = gp_filesystem_set_info_noop(camera->fs, path->folder, path->name, info, context);
	return ret;
}

static int
camera_get_config (Camera *camera, CameraWidget **window, GPContext *context)
{
	CameraWidget *child;

	GP_DEBUG ("*** camera_get_config");

	return GP_OK;
}

static int
camera_set_config (Camera *camera, CameraWidget *window, GPContext *context)
{
	CameraWidget *child;
	int ret;

	GP_DEBUG ("*** camera_set_config");

	return GP_OK;
}

static int
camera_exit (Camera *camera, GPContext *context) 
{
	pslr_disconnect (camera->pl);
	free (camera->pl);
	return GP_OK;
}

int
camera_init (Camera *camera, GPContext *context) 
{
	camera->pl = pslr_init (camera->port);
	if (camera->pl == NULL) return GP_ERROR_NO_MEMORY;
	pslr_connect (camera->pl);

	camera->functions->exit = camera_exit;
	camera->functions->summary = camera_summary;
	camera->functions->get_config = camera_get_config;
	camera->functions->set_config = camera_set_config;
	camera->functions->capture = camera_capture;
	return gp_filesystem_set_funcs (camera->fs, &fsfuncs, camera);
}
