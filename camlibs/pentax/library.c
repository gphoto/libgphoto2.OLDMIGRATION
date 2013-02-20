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

#define _BSD_SOURCE

#include "config.h"

#define GP_MODULE "pentax"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
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
	int ret;

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
	if (GP_OK != (ret = gp_abilities_list_append (list, a)))
		return ret;
	strcpy (a.model, "Pentax:K10D");
	a.usb_product		= 0x006e;
	if (GP_OK != (ret = gp_abilities_list_append (list, a)))
		return ret;
	strcpy (a.model, "Pentax:K100D");
	a.usb_product		= 0x0070;
	if (GP_OK != (ret = gp_abilities_list_append (list, a)))
		return ret;

	strcpy (a.model, "Pentax:K200D");
	a.usb_product		= 0x0093;
	if (GP_OK != (ret = gp_abilities_list_append (list, a)))
		return ret;

	/* weird usb vendor? */
	strcpy (a.model, "Pentax:K5D");
	a.usb_vendor		= 0x25fb;
	a.usb_product		= 0x0102;
	if (GP_OK != (ret = gp_abilities_list_append (list, a)))
		return ret;
	return GP_OK;
}

static int
camera_summary (Camera *camera, CameraText *summary, GPContext *context)
{
	sprintf (summary->text, _(
		"Pentax K DSLR capture driver.\n"
		"Based on pkremote by Pontus Lidman.\n"
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
	int length;
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

static int 		capcnt = 0;

static int
camera_capture (Camera *camera, CameraCaptureType type, CameraFilePath *path,
		GPContext *context)
{
	pslr_handle_t		p = camera->pl;
	pslr_status		status;
	int			ret, length;
	CameraFile		*file = NULL;
	CameraFileInfo		info;

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
_timeout_passed(struct timeval *start, int timeout) {
	struct timeval curtime;

	gettimeofday (&curtime, NULL);
	return ((curtime.tv_sec - start->tv_sec)*1000)+((curtime.tv_usec - start->tv_usec)/1000) >= timeout;
}


static int
camera_wait_for_event (Camera *camera, int timeout,
                       CameraEventType *eventtype, void **eventdata,
                       GPContext *context) {
        struct timeval  event_start;
	CameraFilePath	*path;
	pslr_handle_t	p = camera->pl;
	int		ret, length;
	CameraFile	*file = NULL;
	CameraFileInfo	info;

	*eventtype = GP_EVENT_TIMEOUT;
	*eventdata = NULL;

	gettimeofday (&event_start, 0);
	while (1) {
		pslr_status	status;
		int bufno;

		if (PSLR_OK != pslr_get_status (camera->pl, &status))
			break;

		if (status.bufmask == 0)
			goto next;
		/* New image on camera! */
		for (bufno=0;bufno<16;bufno++)
			if (status.bufmask & (1<<bufno))
				break;
		if (bufno == 16) goto next;

		path = malloc(sizeof(CameraFilePath));
		strcpy (path->folder, "/");
		sprintf (path->name, "capt%04d.jpg", capcnt++);

		ret = gp_file_new(&file);
		if (ret!=GP_OK) return ret;
		gp_file_set_mtime (file, time(NULL));
		gp_file_set_mime_type (file, GP_MIME_JPEG);

		while (1) {
			length = save_buffer( p, bufno, file, &status);
			if (length == GP_ERROR_NOT_SUPPORTED) return length;
			if (length >= GP_OK)
				break;
			usleep(100000);
		}
		pslr_delete_buffer(p, bufno );

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
		*eventtype = GP_EVENT_FILE_ADDED;
		*eventdata = path;
		return GP_OK;

next:
		if (_timeout_passed (&event_start, timeout))
			break;
		usleep(100*1000); /* 100 ms */
	}
	return GP_OK;
}

static int
camera_get_config (Camera *camera, CameraWidget **window, GPContext *context)
{
	CameraWidget	*t, *section;
	const char	*model;
	pslr_status	status;
	char		buf[20];

	pslr_get_status (camera->pl, &status);

	model = pslr_camera_name (camera->pl);

	GP_DEBUG ("*** camera_get_config");

	gp_widget_new (GP_WIDGET_WINDOW, _("Camera and Driver Configuration"), window);
	gp_widget_set_name (*window, "main");

	gp_widget_new (GP_WIDGET_SECTION, _("Camera Settings"), &section);
	gp_widget_set_name (section, "settings");
	gp_widget_append (*window, section);

	gp_widget_new (GP_WIDGET_TEXT, _("Model"), &t);
	gp_widget_set_name (t, "model");
	gp_widget_set_value (t, model);
	gp_widget_set_readonly (t, 1);
	gp_widget_append (section, t);


	gp_widget_new (GP_WIDGET_RADIO, _("Image Size"), &t);
	gp_widget_set_name (t, "imgsize");
	gp_widget_add_choice (t, "14");
	gp_widget_add_choice (t, "10");
	gp_widget_add_choice (t, "6");
	gp_widget_add_choice (t, "2");

	switch (status.jpeg_resolution) {
	case PSLR_JPEG_RESOLUTION_14M:	gp_widget_set_value (t, "14");break;
	case PSLR_JPEG_RESOLUTION_10M:	gp_widget_set_value (t, "10");break;
	case PSLR_JPEG_RESOLUTION_6M:	gp_widget_set_value (t, "6");break;
	case PSLR_JPEG_RESOLUTION_2M:	gp_widget_set_value (t, "2");break;
	default: 			gp_widget_set_value (t, _("Unknown"));break;
	}
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_RADIO, _("Image Quality"), &t);
	gp_widget_set_name (t, "imgquality");
	gp_widget_add_choice (t, "4");
	gp_widget_add_choice (t, "3");
	gp_widget_add_choice (t, "2");
	gp_widget_add_choice (t, "1");
	sprintf (buf,"%d",status.jpeg_quality);
	gp_widget_set_value (t, buf);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_RADIO, _("ISO"), &t);
	gp_widget_set_name (t, "iso");
	gp_widget_add_choice (t, "100");
	gp_widget_add_choice (t, "200");
	gp_widget_add_choice (t, "400");
	gp_widget_add_choice (t, "800");
	gp_widget_add_choice (t, "1600");
	gp_widget_add_choice (t, "3200");
	sprintf(buf,"%d",status.current_iso);
	gp_widget_set_value (t, buf);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Shutter Speed"), &t);
	gp_widget_set_name (t, "shutterspeed");
	sprintf(buf,"%d/%d",status.current_shutter_speed.nom,status.current_shutter_speed.denom);
	gp_widget_set_value (t, buf);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Aperture"), &t);
	gp_widget_set_name (t, "aperture");
	if (status.current_aperture.denom == 1) {
		sprintf(buf,"%d",status.current_aperture.nom);
	} else if (status.current_aperture.denom == 10) {
		if (status.current_aperture.nom % 10 == 0)
			sprintf(buf,"%d",status.current_aperture.nom/10);
		else
			sprintf(buf,"%d.%d",status.current_aperture.nom/10,status.current_aperture.nom%10);
	} else {
		sprintf(buf,"%d/%d",status.current_aperture.nom,status.current_aperture.denom);
	}
	gp_widget_set_value (t, buf);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Aperture at Lens Minimum Focal Length"), &t);
	gp_widget_set_name (t, "apertureatminfocallength");
	if (status.lens_min_aperture.denom == 1) {
		sprintf(buf,"%d",status.lens_min_aperture.nom);
	} else if (status.lens_min_aperture.denom == 10) {
		if (status.lens_min_aperture.nom % 10 == 0)
			sprintf(buf,"%d",status.lens_min_aperture.nom/10);
		else
			sprintf(buf,"%d.%d",status.lens_min_aperture.nom/10,status.lens_min_aperture.nom%10);
	} else {
		sprintf(buf,"%d/%d",status.lens_min_aperture.nom,status.lens_min_aperture.denom);
	}
	gp_widget_set_value (t, buf);
	gp_widget_set_readonly (t, 1);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Aperture at Lens Maximum Focal Length"), &t);
	gp_widget_set_name (t, "apertureatmaxfocallength");
	if (status.lens_max_aperture.denom == 1) {
		sprintf(buf,"%d",status.lens_max_aperture.nom);
	} else if (status.lens_max_aperture.denom == 10) {
		if (status.lens_max_aperture.nom % 10 == 0)
			sprintf(buf,"%d",status.lens_max_aperture.nom/10);
		else
			sprintf(buf,"%d.%d",status.lens_max_aperture.nom/10,status.lens_max_aperture.nom%10);
	} else {
		sprintf(buf,"%d/%d",status.lens_max_aperture.nom,status.lens_max_aperture.denom);
	}
	gp_widget_set_value (t, buf);
	gp_widget_set_readonly (t, 1);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("Zoom"), &t);
	gp_widget_set_name (t, "zoom");
	sprintf(buf,"%d/%d",status.zoom.nom,status.zoom.denom);
	gp_widget_set_value (t, buf);
	gp_widget_set_readonly (t, 1);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_TEXT, _("EC"), &t);
	gp_widget_set_name (t, "ec");
	sprintf(buf,"%d/%d",status.ec.nom,status.ec.denom);
	gp_widget_set_value (t, buf);
	gp_widget_set_readonly (t, 1);
	gp_widget_append (section, t);

	gp_widget_new (GP_WIDGET_RADIO, _("Shooting Mode"), &t);
	gp_widget_set_name (t, "shootingmode");
	gp_widget_add_choice (t, _("GREEN"));
	gp_widget_add_choice (t, _("P"));
	gp_widget_add_choice (t, _("SV"));
	gp_widget_add_choice (t, _("TV"));
	gp_widget_add_choice (t, _("AV"));
	gp_widget_add_choice (t, _("TAV"));
	gp_widget_add_choice (t, _("M"));
	gp_widget_add_choice (t, _("B"));
	gp_widget_add_choice (t, _("X"));
	switch (status.exposure_mode) {
	case PSLR_EXPOSURE_MODE_GREEN:	gp_widget_set_value (t, _("GREEN"));break;
	case PSLR_EXPOSURE_MODE_M:	gp_widget_set_value (t, _("M"));break;
	case PSLR_EXPOSURE_MODE_P:	gp_widget_set_value (t, _("P"));break;
	case PSLR_EXPOSURE_MODE_AV:	gp_widget_set_value (t, _("AV"));break;
	case PSLR_EXPOSURE_MODE_TV:	gp_widget_set_value (t, _("TV"));break;
	case PSLR_EXPOSURE_MODE_SV:	gp_widget_set_value (t, _("SV"));break;
	case PSLR_EXPOSURE_MODE_TAV:	gp_widget_set_value (t, _("TAV"));break;
	case PSLR_EXPOSURE_MODE_X:	gp_widget_set_value (t, _("X"));break;
	case PSLR_EXPOSURE_MODE_B:	gp_widget_set_value (t, _("B"));break;
	default:
		sprintf(buf, _("Unknown mode %d"), status.exposure_mode);
		gp_widget_set_value (t, buf);
		break;
	}
	gp_widget_append (section, t);

	return GP_OK;
}

static int
camera_set_config (Camera *camera, CameraWidget *window, GPContext *context)
{
	CameraWidget *w;
	char *sval;
	pslr_status		status;

	pslr_get_status(camera->pl, &status);

	GP_DEBUG ("*** camera_set_config");
	gp_widget_get_child_by_label (window, _("Image Size"), &w);
	if (gp_widget_changed (w)) {
		pslr_jpeg_resolution_t resolution;

		gp_widget_get_value (w, &sval);
		resolution = PSLR_JPEG_RESOLUTION_MAX;
		if (!strcmp(sval,"14"))	resolution = PSLR_JPEG_RESOLUTION_14M;
		if (!strcmp(sval,"10"))	resolution = PSLR_JPEG_RESOLUTION_10M;
		if (!strcmp(sval,"6"))	resolution = PSLR_JPEG_RESOLUTION_6M;
		if (!strcmp(sval,"2"))	resolution = PSLR_JPEG_RESOLUTION_2M;
	
		if (resolution != PSLR_JPEG_RESOLUTION_MAX) {
			pslr_set_jpeg_resolution(camera->pl, resolution);
			pslr_get_status(camera->pl, &status);
		} else {
			gp_log (GP_LOG_ERROR, "pentax", "Could not decode image size %s", sval);
		}
	}

	gp_widget_get_child_by_label (window, _("Shooting Mode"), &w);
	if (gp_widget_changed (w)) {
		pslr_exposure_mode_t exposuremode;
		gp_widget_get_value (w, &sval);

		exposuremode = PSLR_EXPOSURE_MODE_MAX;
		if (!strcmp(sval,_("GREEN")))	exposuremode = PSLR_EXPOSURE_MODE_GREEN;
		if (!strcmp(sval,_("M")))	exposuremode = PSLR_EXPOSURE_MODE_M;
		if (!strcmp(sval,_("B")))	exposuremode = PSLR_EXPOSURE_MODE_B;
		if (!strcmp(sval,_("P")))	exposuremode = PSLR_EXPOSURE_MODE_P;
		if (!strcmp(sval,_("SV")))	exposuremode = PSLR_EXPOSURE_MODE_SV;
		if (!strcmp(sval,_("TV")))	exposuremode = PSLR_EXPOSURE_MODE_TV;
		if (!strcmp(sval,_("AV")))	exposuremode = PSLR_EXPOSURE_MODE_AV;
		if (!strcmp(sval,_("TAV")))	exposuremode = PSLR_EXPOSURE_MODE_TAV;
		if (!strcmp(sval,_("X")))	exposuremode = PSLR_EXPOSURE_MODE_TAV;
		if (exposuremode != PSLR_EXPOSURE_MODE_MAX) {
			pslr_set_exposure_mode(camera->pl, exposuremode);
			pslr_get_status(camera->pl, &status);
		} else {
		}
			gp_log (GP_LOG_ERROR, "pentax", "Could not decode exposuremode %s", sval);
	}

	gp_widget_get_child_by_label (window, _("ISO"), &w);
	if (gp_widget_changed (w)) {
		int iso;
		gp_widget_get_value (w, &sval);
		if (sscanf(sval, "%d", &iso)) {
			pslr_set_iso(camera->pl, iso);
			pslr_get_status(camera->pl, &status);
		} else
			gp_log (GP_LOG_ERROR, "pentax", "Could not decode iso %s", sval);
	}

	gp_widget_get_child_by_label (window, _("Image Quality"), &w);
	if (gp_widget_changed (w)) {
		int qual;

		/* FIXME: decoding is strange. the UI shows number of starts
		 * on k20d: 4 stars = 3, 3 stars = 0, 2 stars = 1, 1 star = 2 
		 */
		gp_widget_get_value (w, &sval);
		if (sscanf(sval, "%d", &qual)) {
			pslr_set_jpeg_quality (camera->pl, qual);
			pslr_get_status (camera->pl, &status);
		} else
			gp_log (GP_LOG_ERROR, "pentax", "Could not decode image quality %s", sval);
	}

	gp_widget_get_child_by_label (window, _("Shutter Speed"), &w);
	if (gp_widget_changed (w)) {
		pslr_rational_t speed;

		gp_widget_get_value (w, &sval);
		if (sscanf(sval, "%d/%d", &speed.nom, &speed.denom)) {
			pslr_set_shutter(camera->pl, speed);
			pslr_get_status(camera->pl, &status);
		} else {
			char c;
			if (sscanf(sval, "%d%c", &speed.nom, &c) && (c=='s')) {
				speed.denom = 1;
				pslr_set_shutter(camera->pl, speed);
				pslr_get_status(camera->pl, &status);
			} else {
				gp_log (GP_LOG_ERROR, "pentax", "Could not decode shutterspeed %s", sval);
			}
		}
		/* parse more? */
	}

	gp_widget_get_child_by_label (window, _("Aperture"), &w);
	if (gp_widget_changed (w)) {
		pslr_rational_t aperture;
		int apt1,apt2;

		gp_widget_get_value (w, &sval);
		if (sscanf(sval, "%d.%d", &apt1, &apt2)) {
			if (apt1<11) {
				aperture.nom = apt1*10+apt2;
				aperture.denom = 10;
			} else {
				/* apt2 not in use */
				aperture.nom = apt1;
				aperture.denom = 1;
			}
			pslr_set_aperture(camera->pl, aperture);
			pslr_get_status(camera->pl, &status);
		} else if (sscanf(sval, "%d", &apt1)) {
			if (apt1<11) {
				aperture.nom = apt1*10;
				aperture.denom = 10;
			} else {
				aperture.nom = apt1;
				aperture.denom = 1;
			}
			pslr_set_aperture(camera->pl, aperture);
			pslr_get_status(camera->pl, &status);
		} else {
			gp_log (GP_LOG_ERROR, "pentax", "Could not decode aperture %s", sval);
		}
	}

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
	const char *model;
	camera->pl = pslr_init (camera->port);
	if (camera->pl == NULL) return GP_ERROR_NO_MEMORY;
	pslr_connect (camera->pl);

	camera->functions->exit = camera_exit;
	camera->functions->summary = camera_summary;
	camera->functions->get_config = camera_get_config;
	camera->functions->set_config = camera_set_config;
	camera->functions->capture = camera_capture;
	camera->functions->wait_for_event = camera_wait_for_event;
	model = pslr_camera_name (camera->pl);
	gp_log (GP_LOG_DEBUG, "pentax", "reported camera model is %s\n", model);
	return gp_filesystem_set_funcs (camera->fs, &fsfuncs, camera);
}
