/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/** \file
 * 
 * \author Copyright 2001 Lutz M�ller <lutz@users.sf.net>
 * \author Copyright 1999 Scott Fritzinger <scottf@unr.edu>
 *
 * \par License
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * \par
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * \par
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ltdl.h>

#include <gphoto2/gphoto2-port-result.h>
#include <gphoto2/gphoto2-port-library.h>
#include <gphoto2/gphoto2-port-log.h>

#include "gphoto2-port-info.h"

#ifdef ENABLE_NLS
#  include <libintl.h>
#  undef _
#  define _(String) dgettext (GETTEXT_PACKAGE, String)
#  ifdef gettext_noop
#      define N_(String) gettext_noop (String)
#  else
#      define N_(String) (String)
#  endif
#else
#  define _(String) (String)
#  define N_(String) (String)
#  define ngettext(String1,String2,Count) ((Count==1)?String1:String2)
#endif

#define CHECK_RESULT(result) {int r=(result); if (r<0) return (r);}
#define CHECK_NULL(m) {if (!(m)) return (GP_ERROR_BAD_PARAMETERS);}
#define CHECK_SUPP(p,t,o) {if (!(o)) {gp_port_set_error ((p), _("The operation '%s' is not supported by this device"), (t)); return (GP_ERROR_NOT_SUPPORTED);}}
#define CHECK_INIT(p) {if (!(p)->pc->ops) {gp_port_set_error ((p), _("The port has not yet been initialized")); return (GP_ERROR_BAD_PARAMETERS);}}

/**
 * \brief Internal private libgphoto2_port data.
 * This structure contains private data.
 **/
struct _GPPortPrivateCore {
	char error[2048];	/**< Internal kept error message. */

	struct _GPPortInfo info;	/**< Internal port information of this port. */
	GPPortOperations *ops;	/**< Internal port operations. */
	lt_dlhandle lh;		/**< Internal libtool library handle. */
};

/**
 * \brief Create new GPPort
 *
 * Allocate and initialize the memory for a new #GPPort.
 *
 * After you called this function, 
 * you probably want to call #gp_port_set_info in order to make the newly
 * created port functional.
 *
 * \param port Pointer the #GPPort* pointer
 * \return a gphoto2 error code
 **/
int
gp_port_new (GPPort **port)
{
	CHECK_NULL (port);

        gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Creating new device..."));

	*port = malloc (sizeof (GPPort));
        if (!(*port))
		return (GP_ERROR_NO_MEMORY);
        memset (*port, 0, sizeof (GPPort));

	(*port)->pc = malloc (sizeof (GPPortPrivateCore));
	if (!(*port)->pc) {
		gp_port_free (*port);
		return (GP_ERROR_NO_MEMORY);
	}
	memset ((*port)->pc, 0, sizeof (GPPortPrivateCore));

        return (GP_OK);
}

static int
gp_port_init (GPPort *port)
{
	CHECK_NULL (port);
	CHECK_INIT (port);

	if (port->pc->ops->init)
		CHECK_RESULT (port->pc->ops->init (port));

	return (GP_OK);
}

static int
gp_port_exit (GPPort *port)
{
	CHECK_NULL (port);
	CHECK_INIT (port);

	if (port->pc->ops->exit)
		CHECK_RESULT (port->pc->ops->exit (port));

	return (GP_OK);
}

/**
 * \brief Configure a port
 *
 * Makes a port functional by passing in the necessary path
 * information (from the serial:/dev/ttyS0 or similar variables).
 * After calling this function, you can access the port using for
 * example gp_port_open().
 * 
 * \param port a GPPort
 * \param info the GPPortInfo to set
 *
 * \return a gphoto2 error code
 **/
int
gp_port_set_info (GPPort *port, GPPortInfo info)
{
	GPPortLibraryOperations ops_func;

	CHECK_NULL (port);

	if (port->pc->info.name) free (port->pc->info.name);
	port->pc->info.name = strdup (info->name);
	if (port->pc->info.path) free (port->pc->info.path);
	port->pc->info.path = strdup (info->path);
	port->pc->info.type = info->type;
	if (port->pc->info.library_filename) free (port->pc->info.library_filename);
	port->pc->info.library_filename = strdup (info->library_filename);

	port->type = info->type;

	/* Clean up */
	if (port->pc->ops) {
		gp_port_exit (port);
		free (port->pc->ops);
		port->pc->ops = NULL;
	}
	if (port->pc->lh) {
		lt_dlclose (port->pc->lh);
		lt_dlexit ();
	}

	lt_dlinit ();
	port->pc->lh = lt_dlopenext (info->library_filename);
	if (!port->pc->lh) {
		gp_log (GP_LOG_ERROR, "gphoto2-port", _("Could not load "
			"'%s' ('%s')."), info->library_filename,
			lt_dlerror ());
		lt_dlexit ();
		return (GP_ERROR_LIBRARY);
	}

	/* Load the operations */
	ops_func = lt_dlsym (port->pc->lh, "gp_port_library_operations");
	if (!ops_func) {
		gp_log (GP_LOG_ERROR, "gphoto2-port", _("Could not find "
			"'gp_port_library_operations' in '%s' ('%s')"),
			info->library_filename, lt_dlerror ());
		lt_dlclose (port->pc->lh);
		lt_dlexit ();
		port->pc->lh = NULL;
		return (GP_ERROR_LIBRARY);
	}
	port->pc->ops = ops_func ();
	gp_port_init (port);

	/* Initialize the settings to some default ones */
	switch (info->type) {
	case GP_PORT_SERIAL:
		port->settings.serial.speed = 0;
		port->settings.serial.bits = 8;
		port->settings.serial.parity = 0;
		port->settings.serial.stopbits = 1;
		gp_port_set_timeout (port, 500);
		break;
	case GP_PORT_USB:
		strncpy (port->settings.usb.port, info->path,
			 sizeof (port->settings.usb.port));
		port->settings.usb.inep = -1;
		port->settings.usb.outep = -1;
		port->settings.usb.config = -1;
		port->settings.usb.interface = 0;
		port->settings.usb.altsetting = -1;
		gp_port_set_timeout (port, 5000);
		break;
	case GP_PORT_USB_DISK_DIRECT:
		snprintf(port->settings.usbdiskdirect.path,
			 sizeof(port->settings.usbdiskdirect.path), "%s",
			 strchr(info->path, ':') + 1);
		break;
	case GP_PORT_USB_SCSI:
		snprintf(port->settings.usbscsi.path,
			 sizeof(port->settings.usbscsi.path), "%s",
			 strchr(info->path, ':') + 1);
		break;
	default:
		/* Nothing in here */
		break;
	}
	gp_port_set_settings (port, port->settings);

	return (GP_OK);
}

/**
 * \brief Retreives information about the port.
 *
 * Retrieves the informations set by gp_port_set_info().
 *
 * \param port a GPPort
 * \param info GPPortInfo
 * \return a gphoto2 error code
 **/
int
gp_port_get_info (GPPort *port, GPPortInfo *info)
{
	CHECK_NULL (port && info);

	*info = &port->pc->info;
	return (GP_OK);
}

/**
 * \brief Open a port.
 * \param port a #GPPort
 *
 * Opens a port which should have been created with #gp_port_new and 
 * configured with #gp_port_set_info and #gp_port_set_settings
 *
 * \return a gphoto2 error code
 **/
int
gp_port_open (GPPort *port)
{
	CHECK_NULL (port);
	CHECK_INIT (port);

	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Opening %s port..."),
		port->type == GP_PORT_SERIAL ? "SERIAL" : 
			(port->type == GP_PORT_USB ? "USB" : ""));

	CHECK_SUPP (port, "open", port->pc->ops->open);
	CHECK_RESULT (port->pc->ops->open (port));

	return GP_OK;
}

/**
 * \brief Close a port.
 * \param port a #GPPort
 *
 * Closes a port temporarily. It can afterwards be reopened using
 * #gp_port_open.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_close (GPPort *port)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Closing port..."));

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "close", port->pc->ops->close);
        CHECK_RESULT (port->pc->ops->close(port));

	return (GP_OK);
}

/**
 * \brief Free the port structure
 * \param port a #GPPort
 *
 * Closes the port and frees the memory.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_free (GPPort *port)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Freeing port..."));

	CHECK_NULL (port);

	if (port->pc) {
		if (port->pc->ops) {

			/* We don't care for return values */
			gp_port_close (port);
			gp_port_exit (port);

			free (port->pc->ops);
			port->pc->ops = NULL;
		}

		if (port->pc->lh) {
			lt_dlclose (port->pc->lh);
			lt_dlexit ();
			port->pc->lh = NULL;
		}

		if (port->pc->info.name) free (port->pc->info.name);
		if (port->pc->info.path) free (port->pc->info.path);
		if (port->pc->info.library_filename) free (port->pc->info.library_filename);

		free (port->pc);
		port->pc = NULL;
	}

        free (port);

        return GP_OK;
}

/**
 * \brief Writes a specified amount of data to a port.

 * \param port a #GPPort
 * \param data the data to write to the port
 * \param size the number of bytes to write to the port
 *
 * Writes data to the port. On non-serial ports the amount of data
 * written is returned (and not just GP_OK).
 *
 * \return a negative gphoto2 error code or the amount of data written.
 **/
int
gp_port_write (GPPort *port, const char *data, int size)
{
	int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Writing %i=0x%x byte(s) "
		"to port..."), size, size);

	CHECK_NULL (port && data);
	CHECK_INIT (port);

	gp_log_data ("gphoto2-port", data, size);

	/* Check if we wrote all bytes */
	CHECK_SUPP (port, "write", port->pc->ops->write);
	retval = port->pc->ops->write (port, data, size);
	CHECK_RESULT (retval);
	if ((port->type != GP_PORT_SERIAL) && (retval != size))
		gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext("Could only write %i out of %i byte","Could only write %i out of %i bytes",size), retval, size);

	return (retval);
}

/**
 * \brief Read data from port
 *
 * \param port a #GPPort
 * \param data a pointer to an allocated buffer
 * \param size the number of bytes that should be read
 *
 * Reads a specified number of bytes from the port into the supplied buffer.
 * It returns the number of bytes read or a negative error code.
 *
 * \return a gphoto2 error code or the amount of data read
 **/
int
gp_port_read (GPPort *port, char *data, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext("Reading %i=0x%x byte from port...","Reading %i=0x%x bytes from port...", size),
		size, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	/* Check if we read as many bytes as expected */
	CHECK_SUPP (port, "read", port->pc->ops->read);
	retval = port->pc->ops->read (port, data, size);
	CHECK_RESULT (retval);
	if (retval != size)
		gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext(
			"Could only read %i out of %i byte",
			"Could only read %i out of %i byte(s)", size), retval, size);

	gp_log_data ("gphoto2-port", data, retval);

	return (retval);
}

/**
 * \brief Check for intterupt.
 *
 * \param port a GPPort
 * \param data a pointer to an allocated buffer
 * \param size the number of bytes that should be read
 *
 * Reads a specified number of bytes from the interrupt endpoint
 * into the supplied buffer.
 * Function waits port->timeout miliseconds for data on interrupt endpoint.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_check_int (GPPort *port, char *data, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port",
		ngettext(
	"Reading %i=0x%x byte from interrupt endpoint...",
	"Reading %i=0x%x bytes from interrupt endpoint...",
	size), size, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	/* Check if we read as many bytes as expected */
	CHECK_SUPP (port, "check_int", port->pc->ops->check_int);
	retval = port->pc->ops->check_int (port, data, size, port->timeout);
	CHECK_RESULT (retval);
	if (retval != size)
		gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Could only read %i "
			"out of %i byte(s)"), retval, size);

	gp_log_data ("gphoto2-port", data, retval);

	return (retval);
}

/** The timeout in milliseconds for fast interrupt reads. */
#define FAST_TIMEOUT	50
/**
 * \brief Check for interrupt without wait
 * \param port a GPPort
 * \param data a pointer to an allocated buffer
 * \param size the number of bytes that should be read
 *
 * Reads a specified number of bytes from the inerrupt endpoint
 * into the supplied buffer.
 * Function waits 50 miliseconds for data on interrupt endpoint.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_check_int_fast (GPPort *port, char *data, int size)
{
        int retval;

	CHECK_NULL (port);
	CHECK_INIT (port);

	/* Check if we read as many bytes as expected */
	CHECK_SUPP (port, "check_int", port->pc->ops->check_int);
	retval = port->pc->ops->check_int (port, data, size, FAST_TIMEOUT);
	CHECK_RESULT (retval);

#ifdef IGNORE_EMPTY_INTR_READS
	if (retval != size && retval != 0 )
#else
	if (retval != size )
#endif
		gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext(
		"Could only read %i out of %i byte",
		"Could only read %i out of %i bytes",
		size
		), retval, size);

#ifdef IGNORE_EMPTY_INTR_READS
	if ( retval != 0 ) {
#endif
		/* For Canon cameras, we will make lots of
		   reads that will return zero length. Don't
		   bother to log them as errors. */
		gp_log (GP_LOG_DEBUG, "gphoto2-port",
			ngettext(
			"Reading %i=0x%x byte from interrupt endpoint (fast)...",
			"Reading %i=0x%x bytes from interrupt endpoint (fast)...",
			size
			),
			size, size);
		gp_log_data ("gphoto2-port", data, retval);
#ifdef IGNORE_EMPTY_INTR_READS
	}
#endif

	return (retval);
}


/**
 * \brief Set timeout of port 
 * \param port a #GPPort
 * \param timeout the timeout
 *
 * Sets the timeout of a port. #gp_port_read will wait timeout milliseconds
 * for data. If no data will be received in that period, %GP_ERROR_TIMEOUT
 * will be returned.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_set_timeout (GPPort *port, int timeout)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Setting timeout to %i "
		"millisecond(s)..."), timeout);

	CHECK_NULL (port);

        port->timeout = timeout;

        return GP_OK;
}

/** Deprecated */
int gp_port_timeout_set (GPPort *, int);
int gp_port_timeout_set (GPPort *port, int timeout)
{
	return (gp_port_set_timeout (port, timeout));
}

/** Deprecated */
int gp_port_timeout_get (GPPort *, int *);
int gp_port_timeout_get (GPPort *port, int *timeout)
{
	return (gp_port_get_timeout (port, timeout));
}

/**
 * \brief Get the current port timeout.
 * \param port a #GPPort
 * \param timeout pointer to timeout
 *
 * Retreives the current timeout of the port.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_get_timeout (GPPort *port, int *timeout)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Getting timeout..."));

	CHECK_NULL (port);

	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Current timeout: %i "
		"milliseconds"), port->timeout);

        *timeout = port->timeout;

        return GP_OK;
}

/**
 * \brief Set port settings
 * \param port a #GPPort
 * \param settings the #GPPortSettings to be set
 *
 * Adjusts the settings of a port. You should always call
 * #gp_port_get_settings, adjust the values depending on the type of the port,
 * and then call #gp_port_set_settings.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_set_settings (GPPort *port, GPPortSettings settings)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Setting settings..."));

	CHECK_NULL (port);
	CHECK_INIT (port);

        /*
	 * We copy the settings to settings_pending and call update on the 
	 * port.
	 */
        memcpy (&port->settings_pending, &settings,
		sizeof (port->settings_pending));
	CHECK_SUPP (port, "update", port->pc->ops->update);
        CHECK_RESULT (port->pc->ops->update (port));

        return (GP_OK);
}

/** Deprecated */
int gp_port_settings_get (GPPort *, GPPortSettings *);
int gp_port_settings_get (GPPort *port, GPPortSettings *settings)
{
	return (gp_port_get_settings (port, settings));
}
/** Deprecated */
int gp_port_settings_set (GPPort *, GPPortSettings);
int gp_port_settings_set (GPPort *port, GPPortSettings settings)
{
	return (gp_port_set_settings (port, settings));
}

/**
 * \brief Get the current port settings.
 * \param port a #GPPort
 * \param settings pointer to the retrieved settings
 *
 * Retreives the current settings of a port.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_get_settings (GPPort *port, GPPortSettings *settings)
{
	CHECK_NULL (port);

        memcpy (settings, &(port->settings), sizeof (gp_port_settings));

        return GP_OK;
}

/**
 * \brief Get setting of specific serial PIN
 *
 * \param port a GPPort
 * \param pin the serial pin to be retrieved
 * \param level the setting of the pin
 * 
 * \return a gphoto2 error code
 */
int
gp_port_get_pin (GPPort *port, GPPin pin, GPLevel *level)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Getting level of pin %i..."),
		pin);

	CHECK_NULL (port && level);
	CHECK_INIT (port);

	CHECK_SUPP (port, "get_pin", port->pc->ops->get_pin);
        CHECK_RESULT (port->pc->ops->get_pin (port, pin, level));

	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Level of pin %i: %i"),
		pin, *level);

	return (GP_OK);
}

static struct {
	GPPin pin;
	unsigned char number;
	const char *description_short;
	const char *description_long;
} PinTable[] = {
	/* we do not translate these technical terms ... for now */
	{GP_PIN_RTS , 7, "RTS" , "Request To Send"    },
	{GP_PIN_DTR , 4, "DTR" , "Data Terminal Ready"},
	{GP_PIN_CTS , 8, "CTS" , "Clear To Send"      },
	{GP_PIN_DSR , 6, "DSR" , "Data Set Ready"     },
	{GP_PIN_CD  , 1, "CD"  , "Carrier Detect"     },
	{GP_PIN_RING, 9, "RING", "Ring Indicator"     },
	{0, 0, NULL, NULL}
};

static struct {
	GPLevel level;
	const char *description;
} LevelTable[] = {
	{GP_LEVEL_LOW, N_("low")},
	{GP_LEVEL_HIGH, N_("high")},
	{0, NULL}
};

/**
 * \brief Set specified serial PIN to value
 *
 * \param port a GPPort
 * \param pin the serial pin to be retrieved
 * \param level the setting of the pin
 *
 * Pulls the specified pin of a serial port to the specified level.
 *
 * \return a gphoto2 error code
 */
int
gp_port_set_pin (GPPort *port, GPPin pin, GPLevel level)
{
	unsigned int i, j;

	for (i = 0; PinTable[i].description_short; i++)
		if (PinTable[i].pin == pin)
			break;
	for (j = 0; LevelTable[j].description; j++)
		if (LevelTable[j].level == level)
			break;
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Setting pin %i "
		"(%s: '%s') to '%s'..."), 
		PinTable[i].number, PinTable[i].description_short,
		PinTable[i].description_long, _(LevelTable[j].description));

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "set_pin", port->pc->ops->set_pin);
	CHECK_RESULT (port->pc->ops->set_pin (port, pin, level));

	return (GP_OK);
}

/**
 * \brief Send a break over a serial port
 *
 * \param port a GPPort
 * \param duration duration of break in milliseconds
 *
 * Sends a break with the specified duration in milliseconds.
 *
 * \return a gphoto2 error code
 */
int
gp_port_send_break (GPPort *port, int duration)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Sending break (%i "
		"milliseconds)..."), duration);

	CHECK_NULL (port);
	CHECK_INIT (port);

        CHECK_SUPP (port, "send_break", port->pc->ops->send_break);
        CHECK_RESULT (port->pc->ops->send_break (port, duration));

	return (GP_OK);
}

/**
 * \brief Flush data on serial port
 *
 * \param port a GPPort
 * \param direction the direction of the flush
 *
 * Flushes the serial output or input (depending on direction)
 * of the serial port.
 *
 * \return a gphoto2 error code
 */
int
gp_port_flush (GPPort *port, int direction)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Flushing port..."));

	CHECK_NULL (port);

	CHECK_SUPP (port, "flush", port->pc->ops->flush);
	CHECK_RESULT (port->pc->ops->flush (port, direction));

        return (GP_OK);
}


/* USB-specific functions */
/* ------------------------------------------------------------------ */

/**
 * \brief Find USB device by vendor/product
 *
 * \param port a GPPort
 * \param idvendor USB vendor id
 * \param idproduct USB product id
 *
 * Find the USB device with the specified vendor:product id pair.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_find_device (GPPort *port, int idvendor, int idproduct)
{
	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "find_device", port->pc->ops->find_device);
	CHECK_RESULT (port->pc->ops->find_device (port, idvendor, idproduct));

        return (GP_OK);
}

/**
 * \brief Find USB device by interface class
 *
 * \param port a GPPort
 * \param mainclass the USB interface class
 * \param subclass the USB interface subclass
 * \param protocol the USB interface protocol
 *
 * Find the USB device with the specified vendor:product id pair.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_find_device_by_class (GPPort *port, int mainclass, int subclass, int protocol)
{
	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "find_device_by_class", port->pc->ops->find_device_by_class);
	CHECK_RESULT (port->pc->ops->find_device_by_class (port, mainclass, subclass, protocol));

        return (GP_OK);
}

/**
 * \brief Clear USB endpoint HALT condition
 *
 * \param port a GPPort
 * \param ep endpoint to clear HALT
 *
 * Clears the HALT (stall?) endpoint condition of the specified endpoint.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_clear_halt (GPPort *port, int ep)
{
	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Clear halt..."));

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "clear_halt", port->pc->ops->clear_halt);
        CHECK_RESULT (port->pc->ops->clear_halt (port, ep));

        return (GP_OK);
}

/**
 * \brief Send a USB control message with output data
 *
 * \param port a GPPort
 * \param request control request code
 * \param value control value
 * \param index control index
 * \param bytes pointer to data
 * \param size size of the data
 *
 * Sends a specific USB control command and write associated data.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_msg_write (GPPort *port, int request, int value, int index,
	char *bytes, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Writing message "
		"(request=0x%x value=0x%x index=0x%x size=%i=0x%x)..."),
		request, value, index, size, size);
	gp_log_data ("gphoto2-port", bytes, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "msg_write", port->pc->ops->msg_write);
        retval = port->pc->ops->msg_write(port, request, value, index, bytes, size);
	CHECK_RESULT (retval);

        return (retval);
}

/**
 * \brief Send a USB control message with input data
 *
 * \param port a GPPort
 * \param request control request code
 * \param value control value
 * \param index control index
 * \param bytes pointer to data
 * \param size size of the data
 *
 * Sends a specific USB interface control command and read associated data.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_msg_read (GPPort *port, int request, int value, int index,
	char *bytes, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", _("Reading message "
		"(request=0x%x value=0x%x index=0x%x size=%i=0x%x)..."),
		request, value, index, size, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "msg_read", port->pc->ops->msg_read);
        retval = port->pc->ops->msg_read (port, request, value, index, bytes, size);
	CHECK_RESULT (retval);

	if (retval != size)
		gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext(
			"Could only read %i out of %i byte",
			"Could only read %i out of %i bytes",
			size
		), retval, size);

	gp_log_data ("gphoto2-port", bytes, retval);
        return (retval);
}

/* 
 * The next two functions handle the request types 0x41 for write 
 * and 0xc1 for read.
 */
/**
 * \brief Send a USB interface control message with output data
 *
 * \param port a GPPort
 * \param request control request code
 * \param value control value
 * \param index control index
 * \param bytes pointer to data
 * \param size size of the data
 *
 * Sends a specific USB interface control command and write associated data.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_msg_interface_write (GPPort *port, int request, 
	int value, int index, char *bytes, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Writing message "
		"(request=0x%x value=0x%x index=0x%x size=%i=0x%x)...",
		request, value, index, size, size);
	gp_log_data ("gphoto2-port", bytes, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "msg_build", port->pc->ops->msg_interface_write);
        retval = port->pc->ops->msg_interface_write(port, request, 
        		value, index, bytes, size);
	CHECK_RESULT (retval);

        return (retval);
}


/**
 * \brief Send a USB interface control message with input data
 *
 * \param port a GPPort
 * \param request control request code
 * \param value control value
 * \param index control index
 * \param bytes pointer to data
 * \param size size of the data
 *
 * Sends a specific USB control command and read associated data.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_msg_interface_read (GPPort *port, int request, int value, int index,
	char *bytes, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Reading message "
		"(request=0x%x value=0x%x index=0x%x size=%i=0x%x)...",
		request, value, index, size, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "msg_read", port->pc->ops->msg_interface_read);
        retval = port->pc->ops->msg_interface_read (port, request, 
        		value, index, bytes, size);
	CHECK_RESULT (retval);

	if (retval != size)
		gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext(
			"Could only read %i out of %i byte",
			"Could only read %i out of %i bytes",
			size), retval, size);

	gp_log_data ("gphoto2-port", bytes, retval);

        return (retval);
}


/* 
 * The next two functions handle the request types 0x21 for write 
 * and 0xa1 for read.
 */

/**
 * \brief Send a USB class control message with output data
 *
 * \param port a GPPort
 * \param request control request code
 * \param value control value
 * \param index control index
 * \param bytes pointer to data
 * \param size size of the data
 *
 * Sends a specific USB class control command and write associated data.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_msg_class_write (GPPort *port, int request, 
	int value, int index, char *bytes, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Writing message "
		"(request=0x%x value=0x%x index=0x%x size=%i=0x%x)...",
		request, value, index, size, size);
	gp_log_data ("gphoto2-port", bytes, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "msg_build", port->pc->ops->msg_class_write);
        retval = port->pc->ops->msg_class_write(port, request, 
        		value, index, bytes, size);
	CHECK_RESULT (retval);

        return (retval);
}


/**
 * \brief Send a USB class control message with input data
 *
 * \param port a GPPort
 * \param request control request code
 * \param value control value
 * \param index control index
 * \param bytes pointer to data
 * \param size size of the data
 *
 * Sends a specific USB class control command and read associated data.
 *
 * \return a gphoto2 error code
 */
int
gp_port_usb_msg_class_read (GPPort *port, int request, int value, int index,
	char *bytes, int size)
{
        int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Reading message "
		"(request=0x%x value=0x%x index=0x%x size=%i=0x%x)...",
		request, value, index, size, size);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "msg_read", port->pc->ops->msg_class_read);
        retval = port->pc->ops->msg_class_read (port, request, 
        		value, index, bytes, size);
	CHECK_RESULT (retval);

	if (retval != size)
		gp_log (GP_LOG_DEBUG, "gphoto2-port", ngettext(
			"Could only read %i out of %i byte",
			"Could only read %i out of %i bytes",
			size
			), retval, size);

	gp_log_data ("gphoto2-port", bytes, retval);

        return (retval);
}

/**
 * \brief Seek on a port (for usb disk direct ports)
 *
 * \param port a #GPPort
 * \param offset offset to seek to
 * \param whence the underlying lseek call whence parameter
 *
 * Seeks to a specific offset on the usb disk
 *
 * \return a gphoto2 error code
 **/
int
gp_port_seek (GPPort *port, int offset, int whence)
{
	int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Seeking to: %d whence: %d",
		offset, whence);

	CHECK_NULL (port);
	CHECK_INIT (port);

	CHECK_SUPP (port, "seek", port->pc->ops->seek);
	retval = port->pc->ops->seek (port, offset, whence);

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Seek result: %d", retval);

	return retval;
}

/**
 * \brief Send a SCSI command to a port (for usb scsi ports)
 *
 * \param port a #GPPort
 * \param to_dev data direction, set to 1 for a scsi cmd which sends
 *        data to the device, set to 0 for cmds which read data from the dev.
 * \param cmd buffer holding the command to send
 * \param cmd_size sizeof cmd buffer
 * \param sense buffer for returning scsi sense information
 * \param sense_size sizeof sense buffer
 * \param data buffer containing informatino to write to the device
 *        (to_dev is 1), or to store data read from the device (to_dev 0).
 *
 * Send a SCSI command to a usb scsi port attached device.
 *
 * \return a gphoto2 error code
 **/
int gp_port_send_scsi_cmd (GPPort *port, int to_dev,
				char *cmd, int cmd_size,
				char *sense, int sense_size,
				char *data, int data_size)
{
	int retval;

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "Sending scsi cmd:");
	gp_log_data ("gphoto2-port", cmd, cmd_size);
	if (to_dev && data_size) {
		gp_log (GP_LOG_DEBUG, "gphoto2-port", "scsi cmd data:");
		gp_log_data ("gphoto2-port", data, data_size);
	}

	CHECK_NULL (port);
	CHECK_INIT (port);

	memset (sense, 0, sense_size);
	CHECK_SUPP (port, "send_scsi_cmd", port->pc->ops->send_scsi_cmd);
	retval = port->pc->ops->send_scsi_cmd (port, to_dev, cmd, cmd_size,
					sense, sense_size, data, data_size);

	gp_log (GP_LOG_DEBUG, "gphoto2-port", "scsi cmd result: %d", retval);

	if (sense[0] != 0) {
		gp_log (GP_LOG_DEBUG, "gphoto2-port", "sense data:");
		gp_log_data ("gphoto2-port", sense, sense_size);
		/* https://secure.wikimedia.org/wikipedia/en/wiki/Key_Code_Qualifier */
		gp_log(GP_LOG_DEBUG, "gphoto2-port","sense decided:");
		if ((sense[0]&0x7f)!=0x70) {
			gp_log(GP_LOG_DEBUG, "gphoto2-port","\tInvalid header.");
		}
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tCurrent command read filemark: %s",(sense[2]&0x80)?"yes":"no");
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tEarly warning passed: %s",(sense[2]&0x40)?"yes":"no");
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tIncorrect blocklengt: %s",(sense[2]&0x20)?"yes":"no");
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tSense Key: %d",sense[2]&0xf);
		if (sense[0]&0x80)
			gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tResidual Length: %d",sense[3]*0x1000000+sense[4]*0x10000+sense[5]*0x100+sense[6]);
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tAdditional Sense Length: %d",sense[7]);
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tAdditional Sense Code: %d",sense[12]);
		gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tAdditional Sense Code Qualifier: %d",sense[13]);
		if (sense[15]&0x80) {
			gp_log(GP_LOG_DEBUG, "gphoto2-port", "\tIllegal Param is in %s",(sense[15]&0x40)?"the CDB":"the Data Out Phase");
			if (sense[15]&0x8) {
				gp_log(GP_LOG_DEBUG, "gphoto2-port", "Pointer at %d, bit %d",sense[16]*256+sense[17],sense[15]&0x7);
			}
		}
	}

	if (!to_dev && data_size) {
		gp_log (GP_LOG_DEBUG, "gphoto2-port", "scsi cmd data:");
		gp_log_data ("gphoto2-port", data, data_size);
	}

	return retval;
}

/**
 * \brief Set verbose port error message
 * \param port a #GPPort
 * \param format printf style format string
 * \param ... variable arguments depending on format string
 *
 * Sets an error message that can later be retrieved using #gp_port_get_error.
 *
 * \return a gphoto2 error code
 **/
int
gp_port_set_error (GPPort *port, const char *format, ...)
{
	va_list args;

	if (!port)
		return (GP_ERROR_BAD_PARAMETERS);

	if (format) {
		va_start (args, format);
		vsnprintf (port->pc->error, sizeof (port->pc->error),
			   _(format), args);
		gp_log (GP_LOG_ERROR, "gphoto2-port", "%s", port->pc->error);
		va_end (args);
	} else
		port->pc->error[0] = '\0';

	return (GP_OK);
}

/**
 * \brief Get verbose port error message
 * \param port a #GPPort
 *
 * Retrieves an error message from a port. If you want to make sure that
 * you get correct error messages, you need to call #gp_port_set_error with
 * an error message of %NULL each time before calling another port-related
 * function of which you want to check the return value.
 *
 * \return a translated error message
 **/
const char *
gp_port_get_error (GPPort *port)
{
	if (port && port->pc && strlen (port->pc->error))
		return (port->pc->error);

	return _("No error description available");
}
