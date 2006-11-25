/* gphoto2-port.h
 *
 * Copyright � 2001 Lutz M�ller <lutz@users.sf.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GPHOTO2_PORT_H__
#define __GPHOTO2_PORT_H__

#include <gphoto2/gphoto2-port-info-list.h>

/* For portability */
#include <gphoto2/gphoto2-port-portability.h>
#ifdef OS2
#include <gphoto2/gphoto2-port-portability-os2.h>
#include <os2.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef TRUE
#define TRUE (0==0)
#endif

#ifndef FALSE
#define FALSE (1==0)
#endif

/**
 * Use Parity. Enable/Disable, and Odd/Even.
 */

typedef enum _GPPortSerialParity
{
    GP_PORT_SERIAL_PARITY_OFF = 0,
    GP_PORT_SERIAL_PARITY_EVEN,
    GP_PORT_SERIAL_PARITY_ODD
} GPPortSerialParity;


#define GP_PORT_MAX_BUF_LEN 4096             /* max length of receive buffer */

typedef struct _GPPortSettingsSerial GPPortSettingsSerial;
struct _GPPortSettingsSerial {
	char port[128];		/** The portname (/dev/ttyX)*/
	int speed;		/** The baudrate of the device. */
	int bits;		/** How many bits data. */
	GPPortSerialParity parity;	/** parity data, see GP_PORT_SERIAL_PARITY_ 
				  defines */
	int stopbits;		/** How many stop bits are used. */
};

typedef struct _GPPortSettingsUSB GPPortSettingsUSB;
struct _GPPortSettingsUSB {
	int inep, outep, intep;
	int config;
	int interface;
	int altsetting;

	/* must be last to avoid binary incompatibility.
	 * luckily we just need to make sure this struct does not 
	 * get larger than _GPPortSettingsSerial. */
	char port[64];
};

typedef struct _GPPortSettingsDisk GPPortSettingsDisk;
struct _GPPortSettingsDisk {
	char mountpoint[128];
};

typedef union _GPPortSettings GPPortSettings;
union _GPPortSettings {
	GPPortSettingsSerial serial;
	GPPortSettingsUSB usb;
	GPPortSettingsDisk disk;
};

enum {
        GP_PORT_USB_ENDPOINT_IN,
        GP_PORT_USB_ENDPOINT_OUT,
        GP_PORT_USB_ENDPOINT_INT
};

typedef struct _GPPortPrivateLibrary GPPortPrivateLibrary;
typedef struct _GPPortPrivateCore    GPPortPrivateCore;

typedef struct _GPPort           GPPort;
struct _GPPort {

	/* For your convenience */
	GPPortType type;

        GPPortSettings settings;
        GPPortSettings settings_pending;

        int timeout; /* in milliseconds */

	GPPortPrivateLibrary *pl;
	GPPortPrivateCore    *pc;
};

int gp_port_new         (GPPort **port);
int gp_port_free        (GPPort *port);

int gp_port_set_info    (GPPort *port, GPPortInfo  info);
int gp_port_get_info    (GPPort *port, GPPortInfo *info);

int gp_port_open        (GPPort *port);
int gp_port_close       (GPPort *port);

int gp_port_write       (GPPort *port, const char *data, int size);
int gp_port_read        (GPPort *port,       char *data, int size);
int gp_port_check_int   (GPPort *port,       char *data, int size);
int gp_port_check_int_fast (GPPort *port,    char *data, int size);

int gp_port_get_timeout  (GPPort *port, int *timeout);
int gp_port_set_timeout  (GPPort *port, int  timeout);

int gp_port_set_settings (GPPort *port, GPPortSettings  settings);
int gp_port_get_settings (GPPort *port, GPPortSettings *settings);

enum _GPPin {
	GP_PIN_RTS,
	GP_PIN_DTR,
	GP_PIN_CTS,
	GP_PIN_DSR,
	GP_PIN_CD,
	GP_PIN_RING
};
typedef enum _GPPin GPPin;

enum _GPLevel {
	GP_LEVEL_LOW  = 0,
	GP_LEVEL_HIGH = 1
};
typedef enum _GPLevel GPLevel;

int gp_port_get_pin   (GPPort *port, GPPin pin, GPLevel *level);
int gp_port_set_pin   (GPPort *port, GPPin pin, GPLevel level);

int gp_port_send_break (GPPort *port, int duration);
int gp_port_flush      (GPPort *port, int direction);

int gp_port_usb_find_device (GPPort *port, int idvendor, int idproduct);
int gp_port_usb_find_device_by_class (GPPort *port, int mainclass, int subclass, int protocol);
int gp_port_usb_clear_halt  (GPPort *port, int ep);
int gp_port_usb_msg_write   (GPPort *port, int request, int value,
			     int index, char *bytes, int size);
int gp_port_usb_msg_read    (GPPort *port, int request, int value,
			     int index, char *bytes, int size);
int gp_port_usb_msg_interface_write    (GPPort *port, int request, 
			    int value, int index, char *bytes, int size);
int gp_port_usb_msg_interface_read    (GPPort *port, int request, 
			    int value, int index, char *bytes, int size);

/* Error reporting */
int         gp_port_set_error (GPPort *port, const char *format, ...)
#ifdef __GNUC__
	__attribute__((__format__(printf,2,3)))
#endif
;
const char *gp_port_get_error (GPPort *port);

/* DEPRECATED */
typedef GPPort gp_port;
typedef GPPortSettings gp_port_settings;
#define PIN_CTS GP_PIN_CTS

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GPHOTO2_PORT_H__ */


