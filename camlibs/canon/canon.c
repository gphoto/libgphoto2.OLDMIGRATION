/*
 * canon.c - Canon protocol "native" operations.
 *
 * Written 1999 by Wolfgang G. Reissnegger and Werner Almesberger
 * Additions 2000 by Philippe Marzouk and Edouard Lafargue
 * USB support, 2000, by Mikael Nystr�m
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
	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			msg = canon_usb_dialogue (camera, canon_usb_funct, &len, path,
						  strlen (path) + 1);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, type, 0x11, &len, path,
						     strlen (path) + 1, NULL);
			break;
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

	gp_debug_printf (GP_DEBUG_LOW, "canon", "canon_int_identify_camera() called");

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			len = 0x4c;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_IDENTIFY_CAMERA,
						  &len, NULL, 0);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0x01, 0x12, &len, NULL);
			break;

	}

	if (!msg) {
		gp_debug_printf (GP_DEBUG_LOW, "canon",
				 "canon_int_identify_camera: " "msg error");
		canon_serial_error_type (camera);
		return GP_ERROR;
	}

	/* Store these values in our "camera" structure: */
	memcpy (camera->pl->firmwrev, (char *) msg + 8, 4);
	strncpy (camera->pl->ident, (char *) msg + 12, 30);
	strncpy (camera->pl->owner, (char *) msg + 44, 30);
	gp_debug_printf (GP_DEBUG_HIGH, "canon", "canon_int_identify_camera: "
			 "ident '%s' owner '%s'", camera->pl->ident, camera->pl->owner);

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

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			len = 0x8;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_POWER_STATUS,
						  &len, NULL, 0);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0x0a, 0x12, &len, NULL);
			break;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return GP_ERROR;
	}

	if (pwr_status)
		*pwr_status = msg[4];
	if (pwr_source)
		*pwr_source = msg[7];
	gp_debug_printf (GP_DEBUG_LOW, "canon", "Status: %i / Source: %i\n", *pwr_status,
			 *pwr_source);
	return GP_OK;
}


/**
 * canon_int_set_file_attributes:
 * @camera: camera to work with
 * @file: file to work on
 *�@dir: directory to work in
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

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
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
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0xe, 0x11, &len, attr, 4, dir,
						     strlen (dir) + 1, file, strlen (file) + 1,
						     NULL);
			break;
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

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_CAMERA_CHOWN,
						  &len, name, strlen (name) + 1);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0x05, 0x12, &len, name,
						     strlen (name) + 1, NULL);
			break;
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
 * Nota: the time returned is not GMT but local time. Therefore,
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

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			len = 0x10;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_GET_TIME, &len,
						  NULL, 0);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0x03, 0x12, &len, NULL);
			break;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return GP_ERROR;
	}

	/* XXX will fail when sizeof(int) != 4. Should use u_int32_t or
	 * something instead. Investigate portability issues.
	 */
	memcpy (&t, msg + 4, 4);

	date = (time_t) byteswap32 (t);

	/* XXX should strip \n at the end of asctime() return data */
	gp_debug_printf (GP_DEBUG_HIGH, "canon", "Camera time: %s ", asctime (gmtime (&date)));
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

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			len = 0x10;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_SET_TIME, &len,
						  NULL, 0);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0x04, 0x12, &len, pcdate,
						     sizeof (pcdate),
						     "\x00\x00\x00\x00\x00\x00\x00\x00", 8,
						     NULL);
			break;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return 0;
	}

	return 1;
}

/**
 * canon_int_serial_ready:
 * @camera: camera to get ready
 * @Returns: gphoto2 error code
 *
 * serial part of canon_int_ready
 **/
int
canon_int_serial_ready (Camera *camera)
{
	unsigned char type, seq;
	int good_ack, speed, try, len;
	char *pkt;
	int res;

	GP_DEBUG ("canon_int_ready()");

	serial_set_timeout (camera->port, 900);	// 1 second is the delay for awakening the camera
	serial_flush_input (camera->port);
	serial_flush_output (camera->port);

	camera->pl->receive_error = NOERROR;

	/* First of all, we must check if the camera is already on */
	/*      cts=canon_serial_get_cts();
	   gp_debug_printf(GP_DEBUG_LOW,"canon","cts : %i\n",cts);
	   if (cts==32) {  CTS == 32  when the camera is connected. */
	if (camera->pl->first_init == 0) {
		/* FIXME: There was
		   && camera->pl->cached_ready == 1) {
		   here once */

		/* First case, the serial speed of the camera is the same as
		 * ours, so let's try to send a ping packet : */
		if (!canon_serial_send_packet
		    (camera, PKT_EOT, camera->pl->seq_tx,
		     camera->pl->psa50_eot + PKT_HDR_LEN, 0))
			return GP_ERROR;
		good_ack = canon_serial_wait_for_ack (camera);
		gp_debug_printf (GP_DEBUG_LOW, "canon", "good_ack = %i\n", good_ack);
		if (good_ack == 0) {
			/* no answer from the camera, let's try
			 * at the speed saved in the settings... */
			speed = camera->pl->speed;
			if (speed != 9600) {
				if (!canon_serial_change_speed (camera->port, speed)) {
					gp_camera_status (camera, _("Error changing speed."));
					gp_debug_printf (GP_DEBUG_LOW, "canon",
							 "speed changed.\n");
				}
			}
			if (!canon_serial_send_packet
			    (camera, PKT_EOT, camera->pl->seq_tx,
			     camera->pl->psa50_eot + PKT_HDR_LEN, 0))
				return GP_ERROR;
			good_ack = canon_serial_wait_for_ack (camera);
			if (good_ack == 0) {
				gp_camera_status (camera, _("Resetting protocol..."));
				canon_serial_off (camera);
				sleep (3);	/* The camera takes a while to switch off */
				return canon_int_ready (camera);
			}
			if (good_ack == -1) {
				gp_debug_printf (GP_DEBUG_LOW, "canon", "Received a NACK !\n");
				return GP_ERROR;
			}
			gp_camera_status (camera, _("Camera OK.\n"));
			return 1;
		}
		if (good_ack == -1) {
			gp_debug_printf (GP_DEBUG_LOW, "canon", "Received a NACK !\n");
			return GP_ERROR;
		}
		gp_debug_printf (GP_DEBUG_LOW, "canon", "Camera replied to ping, proceed.\n");
		return GP_OK;
	}

	/* Camera was off... */

	gp_camera_status (camera, _("Looking for camera ..."));
	gp_camera_progress (camera, 0);
	if (camera->pl->receive_error == FATAL_ERROR) {
		/* we try to recover from an error
		   we go back to 9600bps */
		if (!canon_serial_change_speed (camera->port, 9600)) {
			gp_debug_printf (GP_DEBUG_LOW, "canon", "ERROR: Error changing speed");
			return GP_ERROR;
		}
		camera->pl->receive_error = NOERROR;
	}
	for (try = 1; try < MAX_TRIES; try++) {
		gp_camera_progress (camera, (try / (float) MAX_TRIES));
		if (canon_serial_send
		    (camera, "\x55\x55\x55\x55\x55\x55\x55\x55", 8, USLEEP1) < 0) {
			gp_camera_status (camera, _("Communication error 1"));
			return GP_ERROR;
		}
		pkt = canon_serial_recv_frame (camera, &len);
		if (pkt)
			break;
	}
	if (try == MAX_TRIES) {
		gp_camera_status (camera, _("No response from camera"));
		return GP_ERROR;
	}
	if (!pkt) {
		gp_camera_status (camera, _("No response from camera"));
		return GP_ERROR;
	}
	if (len < 40 && strncmp (pkt + 26, "Canon", 5)) {
		gp_camera_status (camera, _("Unrecognized response"));
		return GP_ERROR;
	}
	strncpy (camera->pl->psa50_id, pkt + 26, sizeof (camera->pl->psa50_id) - 1);

	GP_DEBUG ("psa50_id : '%s'", camera->pl->psa50_id);

	camera->pl->first_init = 0;

	if (!strcmp ("DE300 Canon Inc.", camera->pl->psa50_id)) {
		gp_camera_status (camera, "PowerShot A5");
		camera->pl->model = CANON_PS_A5;
		if (camera->pl->speed > 57600)
			camera->pl->slow_send = 1;
		camera->pl->A5 = 1;
	} else if (!strcmp ("Canon PowerShot A5 Zoom", camera->pl->psa50_id)) {
		gp_camera_status (camera, "PowerShot A5 Zoom");
		camera->pl->model = CANON_PS_A5_ZOOM;
		if (camera->pl->speed > 57600)
			camera->pl->slow_send = 1;
		camera->pl->A5 = 1;
	} else if (!strcmp ("Canon PowerShot A50", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot A50");
		camera->pl->model = CANON_PS_A50;
		if (camera->pl->speed > 57600)
			camera->pl->slow_send = 1;
	} else if (!strcmp ("Canon PowerShot S20", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot S20");
		camera->pl->model = CANON_PS_S20;
	} else if (!strcmp ("Canon PowerShot G1", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot G1");
		camera->pl->model = CANON_PS_G1;
	} else if (!strcmp ("Canon PowerShot A10", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot A10");
		camera->pl->model = CANON_PS_A10;
	} else if (!strcmp ("Canon PowerShot A20", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot A20");
		camera->pl->model = CANON_PS_A20;
	} else if (!strcmp ("Canon EOS D30", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a EOS D30");
		camera->pl->model = CANON_EOS_D30;
	} else if (!strcmp ("Canon PowerShot Pro90 IS", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot Pro90 IS");
		camera->pl->model = CANON_PS_PRO90_IS;
	} else if (!strcmp ("Canon PowerShot Pro70", camera->pl->psa50_id)) {
		gp_camera_status (camera, "Detected a PowerShot Pro70");
		camera->pl->model = CANON_PS_A70;
	} else if ((!strcmp ("Canon DIGITAL IXUS", camera->pl->psa50_id))
		   || (!strcmp ("Canon IXY DIGITAL", camera->pl->psa50_id))
		   || (!strcmp ("Canon PowerShot S100", camera->pl->psa50_id))
		   || (!strcmp ("Canon DIGITAL IXUS v", camera->pl->psa50_id))) {
		gp_camera_status (camera,
				  "Detected a Digital IXUS series / IXY DIGITAL / PowerShot S100 series");
		camera->pl->model = CANON_PS_S100;
	} else if ((!strcmp ("Canon DIGITAL IXUS 300", camera->pl->psa50_id))
		   || (!strcmp ("Canon IXY DIGITAL 300", camera->pl->psa50_id))
		   || (!strcmp ("Canon PowerShot S300", camera->pl->psa50_id))) {
		gp_camera_status (camera,
				  "Detected a Digital IXUS 300 / IXY DIGITAL 300 / PowerShot S300");
		camera->pl->model = CANON_PS_S300;
	} else {
		gp_camera_status (camera, "Detected a PowerShot S10");
		camera->pl->model = CANON_PS_S10;
	}

	//  5 seconds  delay should  be enough for   big flash cards.   By
	// experience, one or two seconds is too  little, as a large flash
	// card needs more access time.
	serial_set_timeout (camera->port, 5000);
	(void) canon_serial_recv_packet (camera, &type, &seq, NULL);
	if (type != PKT_EOT || seq) {
		gp_camera_status (camera, _("Bad EOT"));
		return GP_ERROR;
	}
	camera->pl->seq_tx = 0;
	camera->pl->seq_rx = 1;
	if (!canon_serial_send_frame (camera, "\x00\x05\x00\x00\x00\x00\xdb\xd1", 8)) {
		gp_camera_status (camera, _("Communication error 2"));
		return GP_ERROR;
	}
	res = 0;
	switch (camera->pl->speed) {
		case 9600:
			res = canon_serial_send_frame (camera, SPEED_9600, 12);
			break;
		case 19200:
			res = canon_serial_send_frame (camera, SPEED_19200, 12);
			break;
		case 38400:
			res = canon_serial_send_frame (camera, SPEED_38400, 12);
			break;
		case 57600:
			res = canon_serial_send_frame (camera, SPEED_57600, 12);
			break;
		case 115200:
			res = canon_serial_send_frame (camera, SPEED_115200, 12);
			break;
	}

	if (!res || !canon_serial_send_frame (camera, "\x00\x04\x01\x00\x00\x00\x24\xc6", 8)) {
		gp_camera_status (camera, _("Communication error 3"));
		return GP_ERROR;
	}
	speed = camera->pl->speed;
	gp_camera_status (camera, _("Changing speed... wait..."));
	if (!canon_serial_wait_for_ack (camera))
		return GP_ERROR;
	if (speed != 9600) {
		if (!canon_serial_change_speed (camera->port, speed)) {
			gp_camera_status (camera, _("Error changing speed"));
			gp_debug_printf (GP_DEBUG_LOW, "canon", "ERROR: Error changing speed");
		} else {
			gp_debug_printf (GP_DEBUG_LOW, "canon", "speed changed\n");
		}

	}
	for (try = 1; try < MAX_TRIES; try++) {
		canon_serial_send_packet (camera, PKT_EOT, camera->pl->seq_tx,
					  camera->pl->psa50_eot + PKT_HDR_LEN, 0);
		if (!canon_serial_wait_for_ack (camera)) {
			gp_camera_status (camera,
					  _
					  ("Error waiting ACK during initialization retrying"));
		} else
			break;
	}

	if (try == MAX_TRIES) {
		gp_camera_status (camera, _("Error waiting ACK during initialization"));
		return GP_ERROR;
	}

	gp_camera_status (camera, _("Connected to camera"));
	/* Now is a good time to ask the camera for its owner
	 * name (and Model String as well)  */
	canon_int_identify_camera (camera);
	canon_int_get_time (camera);

	return GP_OK;
}

/**
 * canon_int_usb_ready:
 * @camera: camera to get ready
 * @Returns: gphoto2 error code
 *
 * USB part of canon_int_ready
 **/
int
canon_int_usb_ready (Camera *camera)
{
	int res;

	GP_DEBUG ("canon_int_usb_ready()");

	res = canon_int_identify_camera (camera);
	if (res != GP_OK) {
		gp_camera_set_error (camera, "Camera not ready, "
				     "identify camera request failed (returned %i)", res);
		return GP_ERROR;
	}
	if (!strcmp ("Canon PowerShot S20", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot S20");
		camera->pl->model = CANON_PS_S20;
	} else if (!strcmp ("Canon PowerShot S10", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot S10");
		camera->pl->model = CANON_PS_S10;
	} else if (!strcmp ("Canon PowerShot S30", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot S30");
		camera->pl->model = CANON_PS_S30;
	} else if (!strcmp ("Canon PowerShot S40", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot S40");
		camera->pl->model = CANON_PS_S40;
	} else if (!strcmp ("Canon PowerShot G1", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot G1");
		camera->pl->model = CANON_PS_G1;
	} else if (!strcmp ("Canon PowerShot G2", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot G2");
		camera->pl->model = CANON_PS_G2;
	} else if ((!strcmp ("Canon DIGITAL IXUS", camera->pl->ident))
		   || (!strcmp ("Canon IXY DIGITAL", camera->pl->ident))
		   || (!strcmp ("Canon PowerShot S110", camera->pl->ident))
		   || (!strcmp ("Canon PowerShot S100", camera->pl->ident))
		   || (!strcmp ("Canon DIGITAL IXUS v", camera->pl->ident))) {
		gp_camera_status (camera,
				  "Detected a Digital IXUS series / IXY DIGITAL / PowerShot S100 series");
		camera->pl->model = CANON_PS_S100;
	} else if ((!strcmp ("Canon DIGITAL IXUS 300", camera->pl->ident))
		   || (!strcmp ("Canon IXY DIGITAL 300", camera->pl->ident))
		   || (!strcmp ("Canon PowerShot S300", camera->pl->ident))) {
		gp_camera_status (camera,
				  "Detected a Digital IXUS 300 / IXY DIGITAL 300 / PowerShot S300");
		camera->pl->model = CANON_PS_S300;
	} else if (!strcmp ("Canon PowerShot A10", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot A10");
		camera->pl->model = CANON_PS_A10;
	} else if (!strcmp ("Canon PowerShot A20", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot A20");
		camera->pl->model = CANON_PS_A20;
	} else if (!strcmp ("Canon EOS D30", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a EOS D30");
		camera->pl->model = CANON_EOS_D30;
	} else if (!strcmp ("Canon PowerShot Pro90 IS", camera->pl->ident)) {
		gp_camera_status (camera, "Detected a PowerShot Pro90 IS");
		camera->pl->model = CANON_PS_PRO90_IS;
	} else {
		gp_camera_set_error (camera, "Unknown camera! (%s)", camera->pl->ident);
		return GP_ERROR;
	}

	res = canon_usb_keylock (camera);
	if (res != GP_OK) {
		gp_camera_set_error (camera, "Camera not ready, "
				     "could not lock camera keys (returned %i)", res);
		return res;
	}

	res = canon_int_get_time (camera);
	if (res == GP_ERROR) {
		gp_camera_set_error (camera, "Camera not ready, "
				     "get time request failed (returned %i)", res);
		return GP_ERROR;
	}

	gp_camera_status (camera, _("Connected to camera"));

	return GP_OK;
}

/**
 * canon_int_ready:
 * @camera: camera to get ready
 * @Returns: gphoto2 error code
 *
 * Switches the camera on, detects the model and sets its speed.
 **/
int
canon_int_ready (Camera *camera)
{
	int res;

	GP_DEBUG ("canon_int_ready()");

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			res = canon_int_usb_ready (camera);
			break;
		case CANON_SERIAL_RS232:
			res = canon_int_serial_ready (camera);
			break;
		default:
			gp_camera_set_error (camera,
					     "Unknown canon_comm_method in canon_int_ready()");
			res = GP_ERROR;
			break;
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

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			res = canon_usb_long_dialogue (camera,
						       CANON_USB_FUNCTION_FLASH_DEVICE_IDENT,
						       &msg, &len, 1024, NULL, 0, 0);
			if (res != GP_OK) {
				GP_DEBUG ("canon_int_get_disk_name: canon_usb_long_dialogue "
					  "failed! returned %i", res);
				return NULL;
			}
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0x0a, 0x11, &len, NULL);
			break;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return NULL;
	}
	if (camera->pl->canon_comm_method == CANON_SERIAL_RS232) {
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
			break;
		case GP_PORT_SERIAL:
			msg = canon_serial_dialogue (camera, 0x09, 0x11, &len, name,
						     strlen (name) + 1, NULL);
			break;
		default:
			gp_camera_set_error (camera, "Don't know how to handle "
					     "camera->port->type value %i "
					     "in %s line %i.", camera->port->type,
					     __FILE__, __LINE__);
			return GP_ERROR_BAD_PARAMETERS;
	}

	if (!msg) {
		canon_serial_error_type (camera);
		return 0;	/* FALSE */
	}
	if (len < 12) {
		GP_DEBUG ("ERROR: truncated message");
		return 0;	/* FALSE */
	}
	cap = get_int (msg + 4);
	ava = get_int (msg + 8);
	if (capacity)
		*capacity = cap;
	if (available)
		*available = ava;

	GP_DEBUG ("canon_int_get_disk_name_info: capacity %i kb, available %i kb",
		  cap > 0 ? (cap / 1024) : 0, ava > 0 ? (ava / 1024) : 0);

	return 1;		/* TRUE */
}


/**
 * gphoto2canonpath:
 * @path: gphoto2 path 
 *
 * convert gphoto2 path  (e.g.   "/DCIM/116CANON/IMG_1240.JPG")
 * into canon style path (e.g. "D:\DCIM\116CANON\IMG_1240.JPG")
 */
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
 * @path: canon style path
 *
 * convert canon style path (e.g. "D:\DCIM\116CANON\IMG_1240.JPG")
 * into gphoto2 path        (e.g.   "/DCIM/116CANON/IMG_1240.JPG")
 */
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
 */
int
canon_int_list_directory (Camera *camera, const char *folder, CameraList *list,
			  const int flags)
{
	CameraFileInfo info;
	int res;
	unsigned int dirents_length;
	unsigned char *dirent_data = NULL;
	unsigned char *end_of_data, *temp_ch, *pos;
	const char *canonfolder = gphoto2canonpath (camera, folder);
	int list_files = ((flags & CANON_LIST_FILES) != 0);
	int list_folders = ((flags & CANON_LIST_FOLDERS) != 0);

	canon_dirent *dirent = NULL;	/* current directory entry */
	unsigned int direntnamelen;	/* length of dirent->name */
	unsigned int direntsize;	/* size of dirent in octets */

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
		default:
			gp_camera_set_error (camera, "can_int_list_dir: "
					     "called for a camera->port->type we do not "
					     "know how to handle");
			return GP_ERROR_BAD_PARAMETERS;
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

		dirent = (canon_dirent *) pos;
		is_dir = ((dirent->attrs & CANON_ATTR_NON_RECURS_ENT_DIR) != 0)
			|| ((dirent->attrs & CANON_ATTR_RECURS_ENT_DIR) != 0);
		is_file = !is_dir;

		GP_LOG (GP_LOG_DATA, "can_int_list_dir: "
			"reading dirent at position %i of %i (0x%x of 0x%x)",
			(pos - dirent_data), (end_of_data - dirent_data),
			(pos - dirent_data), (end_of_data - dirent_data)
			);

		if (pos + sizeof (canon_dirent) + 1 > end_of_data) {
			/* handle (possible) error case */
			if (camera->port->type == GP_PORT_SERIAL) {
				/* check to see if it is only NULL bytes left,
				 * that is not an error for serial cameras
				 * (at least the A50 adds five zero bytes at the end)
				 */
				for (temp_ch = pos; temp_ch < end_of_data && *temp_ch; temp_ch++)	/* do nothing */
					;

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
		direntsize = sizeof (canon_dirent) + direntnamelen + 1;

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
		if (direntnamelen && ((list_folders && is_dir) || (list_files && is_file))
			) {
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
				GP_DEBUG ("Doing gp_filesystem_append for %s", info.file.name);
				gp_filesystem_append (camera->fs, folder, info.file.name);
				GP_DEBUG ("Doing gp_filesystem_set_info_noop for %s",
					  info.file.name);
				gp_filesystem_set_info_noop (camera->fs, folder, info);
			}
			if (is_dir) {
				GP_DEBUG ("Doing gp_list_append for %s", info.file.name);
				gp_list_append (list, info.file.name, NULL);
			}
		} else {
			/* this case could mean that this was the last dirent */
			GP_DEBUG ("can_int_list_dir: "
				  "dirent at position %i of %i has NULL name, skipping.",
				  (pos - dirent_data), (end_of_data - dirent_data));
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
 * Get the directory tree of a given flash device.
 */
#if OBSOLETE
int
_obsolete_canon_int_list_directory (Camera *camera, struct canon_dir **result_dir,
				    const char *path)
{
	struct canon_dir *dir = NULL;
	int entrys = 0, res, is_dir = 0;
	unsigned int dirents_length;
	char filedate_str[32];
	unsigned char *dirent_data = NULL, *end_of_data, *temp_ch, *dirent_name, *pos;

	gp_log (GP_LOG_DEBUG, "canon", "canon_list_directory() path '%s'", path);

	/* set return value to NULL in case something fails */
	*result_dir = NULL;

	/* Fetch all directory entrys from the camera */
	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			res = canon_usb_get_dirents (camera, &dirent_data, &dirents_length,
						     path);
			break;
		case CANON_SERIAL_RS232:
			res = canon_serial_get_dirents (camera, &dirent_data, &dirents_length,
							path);
			break;
		default:
			gp_camera_set_error (camera, "canon_list_directory: "
					     "called for something we do not "
					     "know how to handle");
			return GP_ERROR_BAD_PARAMETERS;
	}
	if (res != GP_OK)
		return res;

	end_of_data = dirent_data + dirents_length;

	if (dirents_length < CANON_MINIMUM_DIRENT_SIZE) {
		gp_camera_set_error (camera, "canon_list_directory: ERROR: "
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
	dirent_name = dirent_data + 10;

	GP_DEBUG ("canon_list_directory: "
		  "Camera directory listing for directory '%s'", dirent_name);

	for (pos = dirent_name; pos < end_of_data && *pos != 0; pos++) ;
	if (pos == end_of_data || *pos != 0) {
		gp_camera_set_error (camera, "canon_list_directory: "
				     "Reached end of packet while "
				     "examining the first dirent");
		free (dirent_data);
		return GP_ERROR;
	}
	pos++;			/* skip NULL byte terminating directory name */

	/* we are now positioned at the first interesting dirent */

	/* This is the main loop, for every directory entry returned */
	while (pos < end_of_data) {
		/* don't use GP_DEBUG since we log this with GP_LOG_DATA */
		gp_log (GP_LOG_DATA, "canon", "canon_list_directory: "
			"reading dirent at position %i of %i", (pos - dirent_data),
			(end_of_data - dirent_data));

		if (pos + CANON_MINIMUM_DIRENT_SIZE > end_of_data) {
			if (camera->pl->canon_comm_method == CANON_SERIAL_RS232) {
				/* check to see if it is only NULL bytes left,
				 * that is not an error for serial cameras
				 * (at least the A50 adds five zero bytes at the end)
				 */
				for (temp_ch = pos; temp_ch < end_of_data && *temp_ch;
				     temp_ch++) ;

				if (temp_ch == end_of_data) {
					GP_DEBUG ("canon_list_directory: "
						  "the last %i bytes were all 0 - ignoring.",
						  temp_ch - pos);
					break;
				} else {
					GP_DEBUG ("canon_list_directory: "
						  "byte[%i=0x%x] == %i=0x%x", temp_ch - pos,
						  temp_ch - pos, *temp_ch, *temp_ch);
					GP_DEBUG ("canon_list_directory: "
						  "pos is 0x%x, end_of_data is 0x%x, temp_ch is 0x%x - diff is 0x%x",
						  pos, end_of_data, temp_ch, temp_ch - pos);
				}
			}
			GP_DEBUG ("canon_list_directory: "
				  "dirent at position %i=0x%x of %i=0x%x is too small, "
				  "minimum dirent is %i bytes",
				  (pos - dirent_data), (pos - dirent_data),
				  (end_of_data - dirent_data), (end_of_data - dirent_data),
				  CANON_MINIMUM_DIRENT_SIZE);
			gp_camera_set_error (camera,
					     "canon_list_directory: "
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
		dirent_name = pos + 10;
		for (temp_ch = dirent_name; temp_ch < end_of_data && *temp_ch != 0;
		     temp_ch++) ;

		if (temp_ch == end_of_data || *temp_ch != 0) {
			GP_DEBUG ("canon_list_directory: "
				  "dirent at position %i of %i has invalid name in it."
				  "bailing out with what we've got.",
				  (pos - dirent_data), (end_of_data - dirent_data));
			break;
		}

		/* check that length of name in this dirent is not of unreasonable size.
		 * 256 was picked out of the blue
		 */
		if (strlen (dirent_name) > 256) {
			GP_DEBUG ("canon_list_directory: "
				  "dirent at position %i of %i has too long name in it (%i bytes)."
				  "bailing out with what we've got.",
				  (pos - dirent_data), (end_of_data - dirent_data),
				  strlen (pos + 10));
			break;
		}

		/* 10 bytes of attributes, size and date, a name and a NULL terminating byte */
		/* don't use GP_DEBUG since we log this with GP_LOG_DATA */
		gp_log (GP_LOG_DATA, "canon", "canon_list_directory: "
			"dirent determined to be %i=0x%x bytes :",
			10 + strlen (dirent_name) + 1, 10 + strlen (dirent_name) + 1);
		gp_log_data ("canon", pos, 10 + strlen (dirent_name) + 1);

		if (strlen (dirent_name)) {
			/* OK, this directory entry has a name in it. */

			temp_ch = realloc (dir, sizeof (struct canon_dir) * (entrys + 1));
			if (temp_ch == NULL) {
				gp_camera_set_error (camera, "canon_list_directory: "
						     "Could not resize canon_dir buffer to %i bytes",
						     sizeof (*dir) * (entrys + 1));
				if (dir)
					canon_int_free_dir (camera, dir);

				free (dirent_data);
				return GP_ERROR_NO_MEMORY;
			}
			dir = (struct canon_dir *) temp_ch;

			dir[entrys].name = strdup (dirent_name);
			if (!dir[entrys].name) {
				gp_camera_set_error (camera, "canon_list_directory: "
						     "Could not duplicate string of %i bytes",
						     strlen (dirent_name));
				if (dir)
					canon_int_free_dir (camera, dir);

				free (dirent_data);
				return GP_ERROR_NO_MEMORY;
			}

			dir[entrys].attrs = *pos;
			/* is_dir is set to the 'real' value, used when printing the
			 * debug output later on.
			 */
			is_dir = ((dir[entrys].attrs & CANON_ATTR_NON_RECURS_ENT_DIR) != 0x0)
				|| ((dir[entrys].attrs & CANON_ATTR_RECURS_ENT_DIR) != 0x0);
			/* dir[entrys].is_file is really 'is_not_recursively_entered_directory' */
			dir[entrys].is_file =
				!((dir[entrys].attrs & CANON_ATTR_NON_RECURS_ENT_DIR) != 0x0);

			/* the size is located at offset 2 and is 4 bytes long */
			memcpy ((unsigned char *) &dir[entrys].size, pos + 2, 4);
			dir[entrys].size = byteswap32 (dir[entrys].size);	/* re-order little/big endian */

			/* the date is located at offset 6 and is 4 bytes long */
			memcpy ((unsigned char *) &dir[entrys].date, pos + 6, 4);
			dir[entrys].date = byteswap32 (dir[entrys].date);	/* re-order little/big endian */

			/* if there is a date, make filedate_str be the ascii representation
			 * without newline at the end
			 */
			if (dir[entrys].date != 0) {
				snprintf (filedate_str, sizeof (filedate_str), "%s",
					  asctime (gmtime (&dir[entrys].date)));
				if (filedate_str[strlen (filedate_str) - 1] == '\n')
					filedate_str[strlen (filedate_str) - 1] = 0;
			} else {
				strcpy (filedate_str, "           -");
			}

			/* This produces ls(1) like output, one line per file :
			 * XXX ADD EXAMPLE HERE
			 */
			/* *INDENT-OFF* */
			GP_DEBUG ("dirent: %c%s  %-5s  (attrs:0x%02x%s%s%s%s)   %10i %-24s %s\n",
				(is_dir) ? 'd' : '-',
				(dir[entrys].attrs & CANON_ATTR_WRITE_PROTECTED) == 0x0 ? "rw" : "r-",
				(dir[entrys].attrs & CANON_ATTR_DOWNLOADED) == 0x0 ? "saved" : "new",
				(unsigned char) dir[entrys].attrs,
				(dir[entrys].attrs & CANON_ATTR_UNKNOWN_2) == 0x0 ? "" : " u2",
				(dir[entrys].attrs & CANON_ATTR_UNKNOWN_4) == 0x0 ? "" : " u4",
				(dir[entrys].attrs & CANON_ATTR_UNKNOWN_8) == 0x0 ? "" : " u8",
				(dir[entrys].attrs & CANON_ATTR_UNKNOWN_40) == 0x0 ? "" : " u40", dir[entrys].size,
				filedate_str,
				dir[entrys].name);
			/* *INDENT-ON* */

			entrys++;
		} else {
			GP_DEBUG ("canon_list_directory: "
				  "dirent at position %i of %i has NULL name, skipping.",
				  (pos - dirent_data), (end_of_data - dirent_data));
		}

		/* make 'p' point to next dirent in packet.
		 * first we skip 10 bytes of attribute, size and date,
		 * then we skip the name plus 1 for the NULL
		 * termination bytes.
		 */
		pos += 10 + strlen (dirent_name) + 1;
	}
	free (dirent_data);

	if (dir) {
		/* allocate one more dirent */
		temp_ch = realloc (dir, sizeof (struct canon_dir) * (entrys + 1));
		if (temp_ch == NULL) {
			gp_camera_set_error (camera, "canon_list_directory: "
					     "could not realloc() %i bytes of memory",
					     sizeof (*dir) * (entrys + 1));
			if (dir)
				canon_int_free_dir (camera, dir);

			return GP_ERROR_NO_MEMORY;
		}

		dir = (struct canon_dir *) temp_ch;
		/* show that this is the last record by setting the name to NULL */
		dir[entrys].name = NULL;

		gp_log (GP_LOG_DEBUG, "canon", "canon_list_directory: "
			"Returning %i directory entrys", entrys);

		*result_dir = dir;
	}

	return GP_OK;
}
#endif // OBSOLETE

int
canon_int_get_file (Camera *camera, const char *name, unsigned char **data, int *length)
{
	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			return canon_usb_get_file (camera, name, data, length);
			break;
		case CANON_SERIAL_RS232:
		default:
			*data = canon_serial_get_file (camera, name, length);
			if (*data)
				return GP_OK;
			return GP_ERROR;
			break;
	}
}

/**
 * canon_int_get_thumbnail:
 * @camera: camera to work with
 * @name: image to get thumbnail of
 * @length: length of data returned
 *
 * Returns the thumbnail data of the picture designated by @name.
 **/
unsigned char *
canon_int_get_thumbnail (Camera *camera, const char *name, int *length)
{
	unsigned char *data = NULL;
	unsigned char *msg;
	exifparser exifdat;
	unsigned int total = 0, expect = 0, size, payload_length, total_file_size;
	int i, j, in;
	unsigned char *thumb;

	GP_DEBUG ("canon_int_get_thumbnail() called for file '%s'", name);

	gp_camera_progress (camera, 0);
	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			i = canon_usb_get_thumbnail (camera, name, &data, length);
			if (i != GP_OK) {
				GP_DEBUG ("canon_usb_get_thumbnail() failed, "
					  "returned %i", i);
				return NULL;	// XXX for now
			}
			break;
		case CANON_SERIAL_RS232:
		default:
			if (camera->pl->receive_error == FATAL_ERROR) {
				GP_DEBUG ("ERROR: can't continue a fatal "
					  "error condition detected");
				return NULL;
			}

			payload_length = strlen (name) + 1;
			msg = canon_serial_dialogue (camera, 0x1, 0x11, &total_file_size,
						     "\x01\x00\x00\x00\x00", 5,
						     &payload_length, 1, "\x00", 2,
						     name, strlen (name) + 1, NULL);
			if (!msg) {
				canon_serial_error_type (camera);
				return NULL;
			}

			total = get_int (msg + 4);
			if (total > 2000000) {	/* 2 MB thumbnails ? unlikely ... */
				GP_DEBUG ("ERROR: %d is too big", total);
				return NULL;
			}
			data = malloc (total);
			if (!data) {
				perror ("malloc");
				return NULL;
			}
			if (length)
				*length = total;

			while (msg) {
				if (total_file_size < 20 || get_int (msg)) {
					return NULL;
				}
				size = get_int (msg + 12);
				if (get_int (msg + 8) != expect || expect + size > total
				    || size > total_file_size - 20) {
					GP_DEBUG ("ERROR: doesn't fit");
					return NULL;
				}
				memcpy (data + expect, msg + 20, size);
				expect += size;
				gp_camera_progress (camera,
						    total ? (expect / (float) total) : 1.);
				if ((expect == total) != get_int (msg + 16)) {
					GP_DEBUG ("ERROR: end mark != end of data");
					return NULL;
				}
				if (expect == total) {
					/* We finished receiving the file. Parse the header and
					   return just the thumbnail */
					break;
				}
				msg = canon_serial_recv_msg (camera, 0x1, 0x21,
							     &total_file_size);
			}
			break;
	}

	switch (camera->pl->model) {
		case CANON_PS_A70:	/* pictures are JFIF files */
			/* we skip the first FF D8 */
			i = 3;
			j = 0;
			in = 0;

			/* we want to drop the header to get the thumbnail */

			thumb = malloc (total);
			if (!thumb) {
				perror ("malloc");
				break;
			}

			while (i < total) {
				if (data[i] == JPEG_ESC) {
					if (data[i + 1] == JPEG_BEG &&
					    ((data[i + 3] == JPEG_SOS)
					     || (data[i + 3] == JPEG_A50_SOS))) {
						in = 1;
					} else if (data[i + 1] == JPEG_END) {
						in = 0;
						thumb[j++] = data[i];
						thumb[j] = data[i + 1];
						return thumb;
					}
				}

				if (in == 1)
					thumb[j++] = data[i];
				i++;

			}
			return NULL;
			break;

		default:	/* Camera supports EXIF */
			exifdat.header = data;
			exifdat.data = data + 12;

			GP_DEBUG ("Got thumbnail, extracting it with the " "EXIF lib.");
			if (exif_parse_data (&exifdat) > 0) {
				GP_DEBUG ("Parsed exif data.");
				data = exif_get_thumbnail (&exifdat);	// Extract Thumbnail
				if (data == NULL) {
					int f;
					char fn[255];

					if (rindex (name, '\\') != NULL)
						snprintf (fn, sizeof (fn) - 1,
							  "canon-death-dump.dat-%s",
							  rindex (name, '\\') + 1);
					else
						snprintf (fn, sizeof (fn) - 1,
							  "canon-death-dump.dat-%s", name);
					fn[sizeof (fn) - 1] = 0;

					gp_debug_printf (GP_DEBUG_LOW, "canon",
							 "canon_int_get_thumbnail: "
							 "Thumbnail conversion error, saving "
							 "%i bytes to '%s'", total, fn);
					/* create with O_EXCL and 0600 for security */
					if ((f =
					     open (fn, O_CREAT | O_EXCL | O_RDWR,
						   0600)) == -1) {
						gp_debug_printf (GP_DEBUG_LOW, "canon",
								 "canon_int_get_thumbnail: "
								 "error creating '%s': %m",
								 fn);
						break;
					}
					if (write (f, data, total) == -1) {
						gp_debug_printf (GP_DEBUG_LOW, "canon",
								 "canon_int_get_thumbnail: "
								 "error writing to file '%s': %m",
								 fn);
					}

					close (f);
					break;
				}
				return data;
			}
			break;
	}

	free (data);
	return NULL;
}

int
canon_int_delete_file (Camera *camera, const char *name, const char *dir)
{
	unsigned char payload[300];
	unsigned char *msg;
	int len, payload_length;

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			memcpy (payload, dir, strlen (dir) + 1);
			memcpy (payload + strlen (dir) + 1, name, strlen (name) + 1);
			payload_length = strlen (dir) + strlen (name) + 2;
			len = 0x4;
			msg = canon_usb_dialogue (camera, CANON_USB_FUNCTION_DELETE_FILE, &len,
						  payload, payload_length);
			break;
		case CANON_SERIAL_RS232:
		default:
			msg = canon_serial_dialogue (camera, 0xd, 0x11, &len, dir,
						     strlen (dir) + 1, name, strlen (name) + 1,
						     NULL);
			break;
	}
	if (!msg) {
		canon_serial_error_type (camera);
		return -1;
	}
	if (msg[0] == 0x29) {
		gp_camera_message (camera, _("File protected"));
		return -1;
	}

	return 0;
}

/*
 * Upload a file to the camera
 *
 */
int
canon_int_put_file (Camera *camera, CameraFile *file, char *destname, char *destpath)
{

	switch (camera->pl->canon_comm_method) {
		case CANON_USB:
			return canon_usb_put_file (camera, file, destname, destpath);
			break;
		case CANON_SERIAL_RS232:
		default:
			return canon_serial_put_file (camera, file, destname, destpath);
			break;
	}
}
