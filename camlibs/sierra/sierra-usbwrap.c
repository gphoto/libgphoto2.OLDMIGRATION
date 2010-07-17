/* sierra_usbwrap.c
 *
 * Copyright � 2002 Lutz M�ller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
 * 
 *
 * Olympus C-3040Z (and possibly also the C-2040Z and others) have
 * a USB PC Control mode in which "Sierra" protocol packets are tunneled
 * inside another protocol.  This file implements the wrapper protocol.
 * The (ab)use of USB clear halt is not needed for this protocol.
 *
 * IMPORTANT: In order to use this mode, the camera must be switched
 * _out_ of "USB Mass Storage" mode and into "USB PC control mode".
 * The images will not be accessable as a mass-storage/disk device in
 * this mode, but you can control the camera, tell it to take pictures
 * and download images using the protocol implemented in sierra.c.
 *
 * To get to the menu for switching modes, open the memory card
 * access door (the camera senses this) and then press and hold
 * both of the menu buttons until the camera control menu appears.
 * Set it to ON.  This disables the USB mass-storage support.
 */

#include "config.h"
#include "sierra-usbwrap.h"
#include "sierra.h"

#include <string.h>
#include <stdlib.h>

#include <_stdint.h>

#include <gphoto2/gphoto2-result.h>
#include <gphoto2/gphoto2-port-log.h>

#define GP_MODULE "sierra"

#define CR(result) {int r = (result); if (r < 0) return (r);}

/*
 * The following things are the way the are just to ensure that USB
 * wrapper packets have the correct byte-order on all types of machines.
 * Intel byte order is used instead of "network" byte order :-(
 */
typedef struct
{ unsigned char c1, c2, c3, c4; } uw32_t; /* A type for 32-bit integers */
typedef struct
{ unsigned char c1, c2, c3, c4; } uw4c_t; /* A type for 4-byte things */
typedef struct
{ unsigned char c1, c2; } uw2c_t; /* A type for 2-byte things? */
static uw32_t uw_value(unsigned int value) /* Convert from host-integer to uw32_t */
{
   uw32_t ret;
   ret.c1 = (value       & 0x000000ffUL);
   ret.c2 = (value >> 8  & 0x000000ffUL);
   ret.c3 = (value >> 16 & 0x000000ffUL);
   ret.c4 = (value >> 24 & 0x000000ffUL);
   return ret;
}

static unsigned char
cmdbyte (unsigned int type, unsigned char nr) {
	switch (type & SIERRA_WRAP_USB_MASK) {
	case SIERRA_WRAP_USB_OLYMPUS:
		return nr | 0xc0;
	case SIERRA_WRAP_USB_NIKON:
		return nr | 0xe0;
	case SIERRA_WRAP_USB_PENTAX:
		return nr | 0xd8;
	default: /* should not get here */
		return 0xff;
	}
}

/* Test for equality between two uw32_t's or two uw4c_t's. */
#define UW_EQUAL(a, b) \
((a).c1==(b).c1 && (a).c2==(b).c2 && (a).c3==(b).c3 && (a).c4==(b).c4)

/*
 * USB wrapper packets start with ASCII bytes "USBC".
 * The responses back from the camera start with "USBS".
 */
#define UW_MAGIC_OUT ((uw4c_t){ 'U','S','B','C' })
#define UW_MAGIC_IN  ((uw4c_t){ 'U','S','B','S' })

static void
make_uw_request(uw4c_t *req,
	unsigned char inout, unsigned char a,
	unsigned char len, unsigned char cmd
) {
	req->c1 = inout;
	req->c2 = a;
	req->c3 = len;
	req->c4 = cmd;
}

#define MAKE_UW_REQUEST_OUT(req, a, b) make_uw_request (req, 0x00, 0x00, a, b) /* write to camera */
#define MAKE_UW_REQUEST_IN(req, a, b) make_uw_request (req, 0x80, 0x00, a, b)  /* read from camera */
/* Move REQUEST_STAT calls to write_packet %%% */

/*
 * Each of the following request types begins with USBC+SessionID and
 * ends with a response from the camera of USBS+SessionID (OK).
 *
 * These requests always appear in a specific order:
 *
 * usb_wrap_write_packet: RDY(OK),              CMND(OK), STAT(OK)
 * usb_wrap_read_packet:  RDY(OK), GETSIZE(OK), DATA(OK), STAT(OK)
 */
/* Test if camera ready */
#define MAKE_UW_REQUEST_RDY(req,type)	MAKE_UW_REQUEST_OUT(req,0x0c,cmdbyte(type, 0))
/* Send command to camera */
#define MAKE_UW_REQUEST_CMND(req,type)	MAKE_UW_REQUEST_OUT(req,0x0c,cmdbyte(type, 1))
/* Read data from camera */
#define MAKE_UW_REQUEST_DATA(req,type)	MAKE_UW_REQUEST_IN(req,0x0c,cmdbyte(type, 2))
/* Get last CMND/DATA rslt */
#define MAKE_UW_REQUEST_STAT(req,type)	MAKE_UW_REQUEST_IN(req,0x0c,cmdbyte(type, 3))
/* Get size of next DATA */
#define MAKE_UW_REQUEST_SIZE(req,type)	MAKE_UW_REQUEST_IN(req,0x0c,cmdbyte(type, 4))
/* Get camera information */
#define MAKE_UW_REQUEST_ID(req,type)	MAKE_UW_REQUEST_IN(req,0x06,0x12)

#define UW_PACKET_RDY  ((uw4c_t){ 0x01, 0x00, 0xff, 0x9f })
#define UW_PACKET_DATA ((uw4c_t){ 0x02, 0x00, 0xff, 0x9f })
#define UW_PACKET_STAT ((uw4c_t){ 0x03, 0x00, 0xff, 0x9f })

#pragma pack(1)
/*
 * The rest of the USB wrapper packet looks like this:
 */
typedef struct
{
      uw4c_t magic;    /* The letters U S B C for packets sent to camera */
      uw32_t sessionid;        /* Any 32-bit handle the host wants (maybe not 0?) */
      uw32_t rw_length;        /* Length of data to be read or written next */
      uw4c_t request_type;     /* One of the UW_REQUEST_* defines */
      char   zero[3];          /* 00 00 00 ? */
      char   req_camid_len;    /* 00 (or 0x22 if camera id request) */
      char   zero2[4];         /* 00 00 00 ? */
      uw32_t length;           /* Length of transaction repeated? */
      char      zero3[3];      /* 00 00 00 */
} uw_header_t;

/*
 * Data packet sent along with a UW_REQUEST_RDY:
 */
typedef struct
{
      uw32_t length;   /* Length of this structure, sizeof(uw_rdy_t) */
      uw4c_t packet_type;      /* Set this to UW_PACKET_RDY. */
      char   zero[8];          /* 00 00 00 00 00 00 00 00 ? */
} uw_pkout_rdy_t;

/*
 * Data packet to send along with a UW_REQUEST_DATA:
 */
typedef struct
{
      uw32_t length;   /* sizeof(uw_data_t) *plus* sierra pckt after it */
      uw4c_t packet_type;      /* Set this to UW_PACKET_DATA */
      char   zero[56];         /* ? */
} uw_pkout_sierra_hdr_t;

/*
 * Expected response to a UW_REQUEST_STAT:
 */
typedef struct
{
      uw32_t length;   /* Length of this structure, sizeof(uw_stat_t) */
      uw4c_t packet_type;      /* Compare with UW_PACKET_STAT here. */
      char   zero[6];          /* 00 00 00 00 00 00 ? */
} uw_stat_t;

/*
 * Expected response to a get size request, UW_REQUEST_SIZE:
 */
typedef struct
{
      uw32_t length;   /* Length of this structure, sizeof(uw_size_t) */
      uw4c_t packet_type;      /* Compare with UW_PACKET_DATA here. */
      char   zero[4];          /* 00 00 00 00 ? */
      uw32_t size;             /* The size of data waiting to be sent by camera */
} uw_size_t;

/*
 * The end of the response from the camera looks like this:
 */
typedef struct
{
      uw4c_t magic;    /* The letters U S B S for packets from camera */
      uw32_t sessionid;        /* A copy of whatever value the host made up */
      char   zero[5];  /* 00 00 00 00 00 */
} uw_response_t;
#pragma pack()

/*
 * This routine is called after every UW_REQUEST_XXX to get an OK
 * with a matching session ID.
 */
static int
usb_wrap_OK (GPPort *dev, uw_header_t *hdr, unsigned int type)
{
   uw_response_t rsp;
   int ret;
   memset(&rsp, 0, sizeof(rsp));

   GP_DEBUG( "usb_wrap_OK" );

   if ((ret = gp_port_read(dev, (char*)&rsp, sizeof(rsp))) != sizeof(rsp))
   {
      gp_log (GP_LOG_DEBUG, GP_MODULE, "gp_port_read *** FAILED (%d vs %d bytes)", (int)sizeof(rsp), ret );
      if (ret < GP_OK)
	return ret;
      return GP_ERROR;
   }

   if (!UW_EQUAL(rsp.magic, UW_MAGIC_IN) ||
       !UW_EQUAL(rsp.sessionid, hdr->sessionid))
   {
      GP_DEBUG( "usb_wrap_OK wrong session *** FAILED" );
      return GP_ERROR;
   }

   /*
    * No idea what these bytes really mean.  Normally they are always 0's
    * when things are in a happy state.
    */
   if (rsp.zero[0] != 0 ||
       rsp.zero[1] != 0 ||
       rsp.zero[2] != 0 ||
       rsp.zero[3] != 0 ||
       rsp.zero[4] != 0)
      {
        GP_DEBUG( "error: ****  usb_wrap_OK failed - not all expected zero bytes are 0" );
        return GP_ERROR;
      }
   return GP_OK;
}

static int
usb_wrap_RDY(gp_port* dev, unsigned int type)
{
   uw_header_t hdr;
   uw_pkout_rdy_t msg;
   int ret, retries = 3;

   GP_DEBUG( "usb_wrap_RDY" );

try_rdy_again:
   memset(&hdr, 0, sizeof(hdr));
   memset(&msg, 0, sizeof(msg));

   hdr.magic     = UW_MAGIC_OUT;
   hdr.sessionid = uw_value(getpid());
   hdr.rw_length = uw_value(sizeof(msg));
   hdr.length    = uw_value(sizeof(msg));
   msg.length    = uw_value(sizeof(msg));
   MAKE_UW_REQUEST_RDY (&hdr.request_type, type);
   msg.packet_type = UW_PACKET_RDY;
  
   if ((ret=gp_port_write(dev, (char*)&hdr, sizeof(hdr))) < GP_OK ||
       (ret=gp_port_write(dev, (char*)&msg, sizeof(msg))) < GP_OK)
   {
      GP_DEBUG( "usb_wrap_RDY *** FAILED" );
      return ret;
   }

   if ((ret=usb_wrap_OK(dev, &hdr, type)) != GP_OK)
   {
      if (!retries--)
      {
        GP_DEBUG( "usb_wrap_RDY GIVING UP" );
        return ret;
      }
      GP_DEBUG( "usb_wrap_RDY *** RETRYING" );
      goto try_rdy_again;
   }
   return GP_OK;
}

static int
usb_wrap_STAT(gp_port* dev, unsigned int type)
{
   uw_header_t hdr;
   uw_stat_t msg;
   int ret;

   GP_DEBUG( "usb_wrap_STAT" );

   memset(&hdr, 0, sizeof(hdr));
   memset(&msg, 0, sizeof(msg));

   hdr.magic = UW_MAGIC_OUT;
   hdr.sessionid = uw_value(getpid());
   hdr.rw_length = uw_value(sizeof(msg));
   hdr.length    = uw_value(sizeof(msg));
   MAKE_UW_REQUEST_STAT (&hdr.request_type, type);
  
   if ((ret=gp_port_write(dev, (char*)&hdr, sizeof(hdr))) < GP_OK ||
       (ret=gp_port_read(dev, (char*)&msg, sizeof(msg))) != sizeof(msg))
   {
      GP_DEBUG( "usb_wrap_STAT *** FAILED" );
      if (ret < GP_OK)
	return ret;
      return GP_ERROR;
   }

   if (!UW_EQUAL(msg.length, hdr.length) || !UW_EQUAL(msg.packet_type, UW_PACKET_STAT))
   {
      GP_DEBUG( "usb_wrap_STAT got bad packet *** FAILED" );
      return GP_ERROR;
   }
   
   if (msg.zero[0] != 0 ||
       msg.zero[1] != 0 ||
       msg.zero[2] != 0 ||
       msg.zero[3] != 0 ||
       msg.zero[4] != 0 ||
       msg.zero[5] != 0)
   {
      GP_DEBUG( "warning: usb_wrap_STAT found non-zero bytes (ignoring)" );
   }

   return usb_wrap_OK(dev, &hdr, type);
}

static int
usb_wrap_CMND(gp_port* dev, unsigned int type, char* sierra_msg, int sierra_len)
{
   uw_header_t hdr;
   uw_pkout_sierra_hdr_t* msg;
   int ret, msg_len = sizeof(*msg) + sierra_len;
   
   GP_DEBUG( "usb_wrap_CMND" );
   
   msg = (uw_pkout_sierra_hdr_t*)malloc(msg_len);
   memset(&hdr, 0, sizeof(hdr));
   memset(msg,  0, msg_len);
   
   hdr.magic = UW_MAGIC_OUT;
   hdr.sessionid = uw_value(getpid());
   hdr.rw_length = uw_value(msg_len);
   hdr.length    = uw_value(msg_len);
   msg->length   = uw_value(msg_len);
   MAKE_UW_REQUEST_CMND (&hdr.request_type, type);
   msg->packet_type = UW_PACKET_DATA;
   memcpy((char*)msg + sizeof(*msg), sierra_msg, sierra_len);
   GP_DEBUG( "usb_wrap_CMND writing %i + %i",
                   (int)sizeof(hdr), msg_len);
   
   if ((ret=gp_port_write(dev, (char*)&hdr, sizeof(hdr))) < GP_OK ||
       (ret=gp_port_write(dev, (char*)msg, msg_len)) < GP_OK)
   {
      GP_DEBUG( "usb_wrap_CMND ** WRITE FAILED");
      free(msg);
      return ret;
   }
   free(msg);

   return usb_wrap_OK(dev, &hdr, type);
}

static int
usb_wrap_SIZE(gp_port* dev, unsigned int type, uw32_t* size)
{
   uw_header_t hdr;
   uw_size_t msg;
   int ret;
  
   GP_DEBUG( "usb_wrap_SIZE" );
   memset(&hdr, 0, sizeof(hdr));
   memset(&msg, 0, sizeof(msg));

   hdr.magic = UW_MAGIC_OUT;
   hdr.sessionid = uw_value(getpid());
   hdr.rw_length = uw_value(sizeof(msg));
   hdr.length    = uw_value(sizeof(msg));
   MAKE_UW_REQUEST_SIZE (&hdr.request_type, type);
   if ((ret=gp_port_write(dev, (char*)&hdr, sizeof(hdr))) < GP_OK ||
       (ret=gp_port_read(dev, (char*)&msg, sizeof(msg))) != sizeof(msg))
   {
      GP_DEBUG( "usb_wrap_SIZE *** FAILED" );
      if (ret < GP_OK)
	return ret;
      return GP_ERROR;
   }

   if (!UW_EQUAL(msg.length, hdr.length) || !UW_EQUAL(msg.packet_type, UW_PACKET_DATA))
   {
      GP_DEBUG( "usb_wrap_SIZE got bad packet *** FAILED" );
      return GP_ERROR;
   }

   if (msg.zero[0] != 0 ||
       msg.zero[1] != 0 ||
       msg.zero[2] != 0 ||
       msg.zero[3] != 0)
   {
      GP_DEBUG( "warning: usb_wrap_SIZE found non-zero bytes (ignoring)" );
   }

   *size = msg.size;

   return usb_wrap_OK(dev, &hdr, type);
}

static int
usb_wrap_DATA (GPPort *dev, unsigned int type, char *sierra_response, int *sierra_len, uw32_t size)
{
   uw_header_t hdr;
   uw_pkout_sierra_hdr_t* msg;
   unsigned int msg_len;
   int ret;

   GP_DEBUG( "usb_wrap_DATA" );

   msg_len = 0;
   msg_len = msg_len * 256 + (unsigned int)(size.c4);
   msg_len = msg_len * 256 + (unsigned int)(size.c3);
   msg_len = msg_len * 256 + (unsigned int)(size.c2);
   msg_len = msg_len * 256 + (unsigned int)(size.c1);

   if (*sierra_len < msg_len - sizeof(*msg))
   {
      GP_DEBUG( "usb_wrap_read_packet buffer too small! (%i < %i) *** FAILED", *sierra_len, msg_len);
      return GP_ERROR;
   }
   *sierra_len = msg_len - sizeof(*msg);

   msg = (uw_pkout_sierra_hdr_t*)malloc(msg_len);
   memset(msg, 0, sizeof(msg));
   memset(&hdr, 0, sizeof(hdr));

   hdr.magic = UW_MAGIC_OUT;
   hdr.sessionid = uw_value(getpid());
   hdr.rw_length = uw_value(msg_len);
   hdr.length    = uw_value(msg_len);
   MAKE_UW_REQUEST_DATA (&hdr.request_type, type);

   if ((ret=gp_port_write(dev, (char*)&hdr, sizeof(hdr))) < GP_OK ||
       (ret=gp_port_read(dev, (char*)msg, msg_len)) != msg_len)
   {
      GP_DEBUG( "usb_wrap_DATA FAILED" );
      free(msg);
      if (ret < GP_OK)
	return ret;
      return GP_ERROR;
   }
   memcpy(sierra_response, (char*)msg + sizeof(*msg), *sierra_len);
   free(msg);

   return usb_wrap_OK(dev, &hdr, type);
}

/*
 * -------------------------------------------------------------------------
 * Here are the two public functions of this C file:
 * -------------------------------------------------------------------------
 */
int
usb_wrap_write_packet (GPPort *dev, unsigned int type, char *sierra_msg, int sierra_len)
{
	GP_DEBUG ("usb_wrap_write_packet");

	CR (usb_wrap_RDY (dev, type));
	CR (usb_wrap_CMND (dev, type, sierra_msg, sierra_len));
	CR (usb_wrap_STAT (dev, type));
	
	return GP_OK;
}

int
usb_wrap_read_packet (GPPort *dev, unsigned int type, char *sierra_response, int sierra_len)
{
	uw32_t uw_size;

	GP_DEBUG ("usb_wrap_read_packet");

	CR (usb_wrap_RDY (dev, type));
	CR (usb_wrap_SIZE (dev, type, &uw_size));
	CR (usb_wrap_DATA (dev, type, sierra_response, &sierra_len, uw_size));
	CR (usb_wrap_STAT (dev, type));
	
	return sierra_len;
}

