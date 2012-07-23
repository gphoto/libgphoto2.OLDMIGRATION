/* olympus_wrap.c
 *
 * Copyright � 2011 Marcus Meissner  <marcus@jet.franken.de>
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <_stdint.h>

#include <libxml/parser.h>

#include "ptp.h"
#include "ptp-private.h"
#include "ptp-pack.c"
#include "olympus-wrap.h"

#include <gphoto2/gphoto2-library.h>
#include <gphoto2/gphoto2-setting.h>
#include <gphoto2/gphoto2-result.h>
#include <gphoto2/gphoto2-port-log.h>

#define GP_MODULE "ptp2-olympus"

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
cmdbyte (unsigned char nr) {
	return nr | 0xc0;
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
 */
#define MAKE_UW_REQUEST_COMMAND(req)		MAKE_UW_REQUEST_OUT(req,0x0c,cmdbyte(0))
#define MAKE_UW_REQUEST_DATAOUT(req)		MAKE_UW_REQUEST_OUT(req,0x0c,cmdbyte(1))
#define MAKE_UW_REQUEST_DATAIN(req)		MAKE_UW_REQUEST_IN (req,0x0c,cmdbyte(2))
#define MAKE_UW_REQUEST_RESPONSE(req)		MAKE_UW_REQUEST_IN (req,0x0c,cmdbyte(3))
#define MAKE_UW_REQUEST_DATAIN_SIZE(req)	MAKE_UW_REQUEST_IN (req,0x0c,cmdbyte(4))

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
usb_wrap_OK (GPPort *dev, uw_header_t *hdr)
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

/* This is for the unique session id for the UMS command / response
 * It gets incremented by one for every command.
 */
static int ums_sessionid = 0x42424242;

/* Transaction data phase description */
#define PTP_DP_NODATA           0x0000  /* no data phase */
#define PTP_DP_SENDDATA         0x0001  /* sending data */
#define PTP_DP_GETDATA          0x0002  /* receiving data */
#define PTP_DP_DATA_MASK        0x00ff  /* data phase mask */

uint16_t
ums_wrap_sendreq (PTPParams* params, PTPContainer* req) {
	uw_header_t hdr;
	PTPUSBBulkContainer usbreq;
	char buf[64];
	int ret;
	Camera *camera = ((PTPData *)params->data)->camera;

	GP_DEBUG( "ums_wrap_sendreq" );
	/* Build appropriate USB container */
	usbreq.length= htod32(PTP_USB_BULK_REQ_LEN-
		(sizeof(uint32_t)*(5-req->Nparam)));
	usbreq.type = htod16(PTP_USB_CONTAINER_COMMAND);
	usbreq.code = htod16(req->Code);
	usbreq.trans_id = htod32(req->Transaction_ID);
	usbreq.payload.params.param1 = htod32(req->Param1);
	usbreq.payload.params.param2 = htod32(req->Param2);
	usbreq.payload.params.param3 = htod32(req->Param3);
	usbreq.payload.params.param4 = htod32(req->Param4);
	usbreq.payload.params.param5 = htod32(req->Param5);

	memset(buf,0,sizeof(buf));
	memcpy(buf,&usbreq,usbreq.length);

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic     = UW_MAGIC_OUT;
	hdr.sessionid = uw_value(ums_sessionid);
	ums_sessionid++;
	hdr.rw_length = uw_value(sizeof(buf));
	hdr.length    = uw_value(sizeof(buf));
	MAKE_UW_REQUEST_COMMAND (&hdr.request_type);

	/* First step: send PTP command, first UMS header packet, then data blob  */
	if (	(ret=gp_port_write(camera->port, (char*)&hdr, sizeof(hdr))) < GP_OK ||
		(ret=gp_port_write(camera->port, buf, sizeof(buf))) < GP_OK)
	{
		GP_DEBUG( "ums_wrap_sendreq *** FAILED, ret %d", ret );
		return PTP_ERROR_IO;
	}
	if ((ret=usb_wrap_OK(camera->port, &hdr)) != GP_OK) {
		GP_DEBUG( "ums_wrap_sendreq FAILED to send PTP command" );
		return PTP_ERROR_IO;
	}
	return PTP_RC_OK;
}

uint16_t
ums_wrap_senddata (
	PTPParams* params, PTPContainer* ptp, unsigned long sendlen, PTPDataHandler*getter
) {
	uw_header_t hdr;
	PTPUSBBulkContainer usbreq;
	int ret;
	unsigned long gotlen;
	unsigned char *xdata;
	Camera *camera = ((PTPData *)params->data)->camera;

	GP_DEBUG( "ums_wrap_senddata" );
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic     = UW_MAGIC_OUT;
	hdr.sessionid = uw_value(ums_sessionid);
	ums_sessionid++;
	hdr.rw_length = uw_value(sendlen+12);
	hdr.length    = uw_value(sendlen+12);
	MAKE_UW_REQUEST_DATAOUT (&hdr.request_type);

	xdata = malloc(sendlen + 12);
	usbreq.length = htod32(sendlen + 12);
	usbreq.type   = htod16(PTP_USB_CONTAINER_DATA);
	usbreq.code   = htod16(ptp->Code);
	usbreq.trans_id = htod32(ptp->Transaction_ID);
	memcpy (xdata, &usbreq, 12);
	ret = getter->getfunc(params, getter->priv, sendlen, xdata+12, &gotlen);
	if (ret != PTP_RC_OK) {
		GP_DEBUG( "ums_wrap_senddata *** data get from handler FAILED, ret %d", ret );
		return ret;
	}
	if (gotlen != sendlen) {
		GP_DEBUG( "ums_wrap_senddata *** data get from handler got %ld instead of %ld", gotlen, sendlen );
		return PTP_ERROR_IO;
	}

	if ((ret=gp_port_write(camera->port, (char*)&hdr, sizeof(hdr))) < GP_OK ||
	    (ret=gp_port_write(camera->port, (char*)xdata, sendlen+12)) < GP_OK)
	{
		free (xdata);
		GP_DEBUG( "ums_wrap_senddata *** data send FAILED, ret %d", ret );
		return PTP_ERROR_IO;
	}
	free (xdata);
	if ((ret=usb_wrap_OK(camera->port, &hdr)) != GP_OK) {
		GP_DEBUG( "ums_wrap_senddata FAILED to send PTP data, ret %d", ret );
		return PTP_ERROR_IO;
	}
	return PTP_RC_OK;
}

uint16_t
ums_wrap_getresp (PTPParams* params, PTPContainer* resp)
{
	uw_header_t hdr;
	PTPUSBBulkContainer usbresp;
	char buf[64];
	int ret;
	Camera *camera = ((PTPData *)params->data)->camera;

	GP_DEBUG( "ums_wrap_getresp" );
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic     = UW_MAGIC_OUT;
	hdr.sessionid = uw_value(ums_sessionid);
	ums_sessionid++;
	hdr.rw_length = uw_value(sizeof(buf));
	hdr.length    = uw_value(sizeof(buf));
	MAKE_UW_REQUEST_RESPONSE (&hdr.request_type);
	if ((ret=gp_port_write(camera->port, (char*)&hdr, sizeof(hdr))) < GP_OK ||
	    (ret=gp_port_read(camera->port, buf, sizeof(buf))) != sizeof(buf))
	{
		GP_DEBUG( "ums_wrap_getresp *** FAILED to read PTP response, ret %d", ret );
		return PTP_ERROR_IO;
	}
	if ((ret=usb_wrap_OK(camera->port, &hdr)) != GP_OK) {
		GP_DEBUG( "ums_wrap_getresp FAILED to read UMS reply, ret %d", ret );
		return PTP_ERROR_IO;
	}
	memcpy (&usbresp, buf, sizeof(usbresp));
	resp->Code = dtoh16(usbresp.code);
	resp->Nparam = (dtoh32(usbresp.length)-PTP_USB_BULK_REQ_LEN)/sizeof(uint32_t);
	resp->Param1 = dtoh32(usbresp.payload.params.param1);
	resp->Param2 = dtoh32(usbresp.payload.params.param2);
	resp->Param3 = dtoh32(usbresp.payload.params.param3);
	resp->Param4 = dtoh32(usbresp.payload.params.param4);
	resp->Param5 = dtoh32(usbresp.payload.params.param5);
	return PTP_RC_OK;
}

uint16_t
ums_wrap_getdata (PTPParams* params, PTPContainer* ptp, PTPDataHandler *putter)
{
	uw_header_t hdr;
	PTPUSBBulkContainer usbresp;
	char buf[64];
	int ret;
	Camera *camera = ((PTPData *)params->data)->camera;
	unsigned long recvlen, written;
	char *data;

	GP_DEBUG( "ums_wrap_getdata" );
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic     = UW_MAGIC_OUT;
	hdr.sessionid = uw_value(ums_sessionid);
	ums_sessionid++;
	hdr.rw_length = uw_value(sizeof(buf));
	hdr.length    = uw_value(sizeof(buf));
	MAKE_UW_REQUEST_DATAIN_SIZE (&hdr.request_type);
	if (	(ret=gp_port_write(camera->port, (char*)&hdr, sizeof(hdr))) < GP_OK ||
		(ret=gp_port_read(camera->port, buf, sizeof(buf))) != sizeof(buf))
	{
		GP_DEBUG( "ums_wrap_getdata *** FAILED to read PTP data in size" );
		return PTP_ERROR_IO;
	}
	if ((ret=usb_wrap_OK(camera->port, &hdr)) != GP_OK) {
		GP_DEBUG( "ums_wrap_getdata FAILED to read PTP data in size" );
		return PTP_ERROR_IO;
	}
	memcpy (&usbresp, buf, sizeof(usbresp));
	if ((dtoh16(usbresp.code) != ptp->Code) && (dtoh16(usbresp.code) != PTP_RC_OK)) {
		GP_DEBUG( "ums_wrap_getdata *** PTP code %04x during PTP data in size read", dtoh16(usbresp.code));
		/* break; */
	}
	if (dtoh16(usbresp.length) != 16) {
		GP_DEBUG( "ums_wrap_getdata *** PTP size %d during PTP data in size read, expected 16", dtoh16(usbresp.length));
		return PTP_ERROR_IO;
	}
	recvlen = dtoh32(usbresp.payload.params.param1);
	data = malloc (recvlen);
	if (!data) {
		return PTP_ERROR_IO;
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic     = UW_MAGIC_OUT;
	hdr.sessionid = uw_value(ums_sessionid);
	ums_sessionid++;
	hdr.rw_length = uw_value(recvlen);
	hdr.length    = uw_value(recvlen);
	MAKE_UW_REQUEST_DATAIN (&hdr.request_type);
	if (	(ret=gp_port_write(camera->port, (char*)&hdr, sizeof(hdr))) < GP_OK ||
		(ret=gp_port_read(camera->port, (char*)data, recvlen)) != recvlen)
	{
		GP_DEBUG( "ums_wrap_getdata *** FAILED to read PTP response" );
		free (data);
		return PTP_ERROR_IO;
	}
	if ((ret=usb_wrap_OK(camera->port, &hdr)) != GP_OK) {
		GP_DEBUG( "ums_wrap_getdata FAILED to read PTP data in" );
		free (data);
		return PTP_ERROR_IO;
	}
	/* skip away the 12 byte header */
	ret = putter->putfunc ( params, putter->priv, recvlen - PTP_USB_BULK_HDR_LEN, (unsigned char*)data + PTP_USB_BULK_HDR_LEN, &written);
	free (data);
	if (ret != PTP_RC_OK) {
		GP_DEBUG( "ums_wrap_getdata FAILED to push data into put handle, ret %x", ret );
		return PTP_ERROR_IO;
	}
	if (written != recvlen - PTP_USB_BULK_HDR_LEN) {
		GP_DEBUG( "ums_wrap_getdata FAILED to push data into put handle, len %ld vs %ld", written, recvlen );
		return PTP_ERROR_IO;
	}
	return PTP_RC_OK;
}

#if 0
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

#endif


static int
olympus_ptp_transaction (PTPParams *params,
	PTPContainer* ptp, uint16_t flags,
	char *cmdxml, char **inxml
) {
	PTPContainer	ptp2;
	int		res;
	PTPObjectInfo	oi;
	char		*req, *resp;
        unsigned char	*oidata=NULL;
        uint32_t	size, handle, newhandle;
	uint16_t	ret;
	PTPParams	outerparams;

	memcpy(&outerparams, params, sizeof(PTPParams));
	outerparams.sendreq_func	= ums_wrap_sendreq;
	outerparams.getresp_func	= ums_wrap_getresp;
	outerparams.senddata_func	= ums_wrap_senddata;
	outerparams.getdata_func	= ums_wrap_getdata;
	outerparams.getdata_func	= ums_wrap_getdata;
	outerparams.event_check		= ptp_usb_event_check;
	outerparams.event_wait		= ptp_usb_event_wait;

	while (1) {
		memset (&ptp2, 0 , sizeof (ptp2));
		ptp2.Code = PTP_OC_SendObjectInfo;
		ptp2.Nparam = 1;
		ptp2.Param1 = 0x80000001;

		memset (&oi, 0, sizeof (oi));
		oi.ObjectFormat		= PTP_OFC_Script;
		oi.StorageID 		= 0x80000001;
		oi.Filename 		= "HREQUEST.X3C";
		oi.ObjectCompressedSize	= strlen(cmdxml);

		size = ptp_pack_OI(params, &oi, &oidata);
		res = ptp_transaction (&outerparams, &ptp2, PTP_DP_SENDDATA, size, &oidata, NULL); 
		if (res != PTP_RC_OK)
			return res;
		free(oidata);
		handle = ptp2.Param3;

		ptp2.Code = PTP_OC_SendObject;
		ptp2.Nparam = 0;
		res = ptp_transaction(params, &ptp2, PTP_DP_SENDDATA, strlen(req), (unsigned char**)&req, NULL);
		if (res != PTP_RC_OK)
			return res;
		ret = params->event_wait(params, &ptp2);
		if (ret != PTP_RC_OK)
			return ret;
		GP_DEBUG("event: code %04x, p %08x\n", ptp2.Code, ptp2.Param1);
		if (ptp2.Code != PTP_EC_RequestObjectTransfer) 
			return PTP_RC_OK;
		newhandle = ptp2.Param1;
		ret = ptp_getobjectinfo (params, newhandle, &oi);
		if (ret != PTP_RC_OK)
			return ret;
		GP_DEBUG("got new file: %s", oi.Filename);
		ret = ptp_getobject (params, newhandle, (unsigned char**)&resp);
		if (ret != PTP_RC_OK)
			return ret;
		GP_DEBUG("file content: %s", resp);
	}
	return PTP_RC_OK;
}

static int
traverse_tree (int depth, xmlNodePtr node) {
	xmlNodePtr	next;
	xmlChar		*xchar;
	int n;
	char 		*xx;


	if (!node) return FALSE;
	xx = malloc(depth * 4 + 1);
	memset (xx, ' ', depth*4);
	xx[depth*4] = 0;

	n = xmlChildElementCount (node);

	next = node;
	do {
		fprintf(stderr,"%snode %s\n", xx,next->name);
		fprintf(stderr,"%selements %d\n", xx,n);
		xchar = xmlNodeGetContent (next);
		fprintf(stderr,"%scontent %s\n", xx,xchar);
		traverse_tree (depth+1,xmlFirstElementChild (next));
	} while ((next = xmlNextElementSibling (next)));
	return TRUE;
}

static int
parse_9301_cmd_tree (xmlNodePtr node) {
	xmlNodePtr next;

	next = xmlFirstElementChild (node);
	do {
		fprintf (stderr,"9301 cmd: %s\n", next->name);
	} while ((next = xmlNextElementSibling (next)));
	return TRUE;
}

static int
parse_value (const char *str, uint16_t type, PTPPropertyValue *propval) {
	switch (type) {
	case 6: { /*UINT32*/
		unsigned int x;
		if (!sscanf(str,"%08x", &x)) {
			fprintf(stderr,"could not parse uint32 %s\n", str);
			return FALSE;
		}
		propval->u32 = x;
		break;
	}
	case 5: { /*INT32*/
		int x;
		if (!sscanf(str,"%08x", &x)) {
			fprintf(stderr,"could not parse int32 %s\n", str);
			return FALSE;
		}
		propval->i32 = x;
		break;
	}
	case 4: { /*UINT16*/
		unsigned int x;
		if (!sscanf(str,"%04x", &x)) {
			fprintf(stderr,"could not parse uint16 %s\n", str);
			return FALSE;
		}
		propval->u16 = x;
		break;
	}
	case 3: { /*INT16*/
		int x;
		if (!sscanf(str,"%04x", &x)) {
			fprintf(stderr,"could not parse int16 %s\n", str);
			return FALSE;
		}
		propval->i16 = x;
		break;
	}
	case 2: { /*UINT8*/
		unsigned int x;
		if (!sscanf(str,"%02x", &x)) {
			fprintf(stderr,"could not parse uint8 %s\n", str);
			return FALSE;
		}
		propval->u8 = x;
		break;
	}
	case 1: { /*INT8*/
		int x;
		if (!sscanf(str,"%02x", &x)) {
			fprintf(stderr,"could not parse int8 %s\n", str);
			return FALSE;
		} 
		propval->i8 = x;
		break;
	}
	case 65535: { /* string */
		int len;

		/* ascii ptp string, 1 byte length, little endian 16 bit chars */
		if (sscanf(str,"%02x", &len)) {
			int i;
			char *xstr = malloc(len+1);
			for (i=0;i<len;i++) {
				int xc;
				if (sscanf(str+2+i*4,"%04x", &xc)) {
					int cx;

					cx = ((xc>>8) & 0xff) | ((xc & 0xff) << 8);
					xstr[i] = cx;
				}
				xstr[len] = 0;
			}
			fprintf(stderr,"\t%s\n", xstr);
			propval->str = xstr;
			break;
		}
		fprintf(stderr,"string %s not parseable!\n", str);
		return FALSE;
	}
	case 7: /*INT64*/
	case 8: /*UINT64*/
	case 9: /*INT128*/
	case 10: /*UINT128*/
	default:
		fprintf(stderr,"unhandled data type %d!\n", type);
		return FALSE;
	}
	return TRUE;
}

static int
parse_propdesc (xmlNodePtr node, PTPDevicePropDesc *dpd) {
	xmlNodePtr next;
	int type = -1;

	fprintf (stderr,"propdesc: %s\n", node->name);
	dpd->FormFlag	= PTP_DPFF_None;
	dpd->GetSet	= PTP_DPGS_Get;
	next = xmlFirstElementChild (node);
	do {
		if (!strcmp((char*)next->name,"type")) {	/* propdesc.DataType */
			if (!sscanf((char*)xmlNodeGetContent (next), "%04x", &type)) {
				fprintf (stderr,"\ttype %s not parseable\n",xmlNodeGetContent (next));
				return 0;
			}
			dpd->DataType = type;
			continue;
		}
		if (!strcmp((char*)next->name,"attribute")) {	/* propdesc.GetSet */
			int attr;
			if (!sscanf((char*)xmlNodeGetContent (next), "%02x", &attr)) {
				fprintf (stderr,"\tattr %s not parseable\n",xmlNodeGetContent (next));
				return 0;
			}
			dpd->GetSet = attr;
			continue;
		}
		if (!strcmp((char*)next->name,"default")) {	/* propdesc.FactoryDefaultValue */
			parse_value ((char*)xmlNodeGetContent (next), type, &dpd->FactoryDefaultValue);
			continue;
		}
		if (!strcmp((char*)next->name,"value")) {	/* propdesc.CurrentValue */
			parse_value ((char*)xmlNodeGetContent (next), type, &dpd->CurrentValue);
			continue;
		}
		if (!strcmp((char*)next->name,"enum")) {	/* propdesc.FORM.Enum */
			int n,i;
			char *s;

			dpd->FormFlag = PTP_DPFF_Enumeration;
			s = (char*)xmlNodeGetContent (next);
			n = 0;
			do {
				s = strchr(s,' ');
				if (s) s++;
				n++;
			} while (s);
			dpd->FORM.Enum.NumberOfValues = n;
			dpd->FORM.Enum.SupportedValue = malloc (n * sizeof(PTPPropertyValue));
			s = (char*)xmlNodeGetContent (next);
			i = 0;
			do {
				parse_value (s, type, &dpd->FORM.Enum.SupportedValue[i]); /* should turn ' ' into \0? */
				i++;
				s = strchr(s,' ');
				if (s) s++;
			} while (s && (i<n));
			continue;
		}
		if (!strcmp((char*)next->name,"range")) {	/* propdesc.FORM.Enum */
			char *s = (char*)xmlNodeGetContent (next);
			dpd->FormFlag = PTP_DPFF_Range;
			parse_value (s, type, &dpd->FORM.Range.MinimumValue); /* should turn ' ' into \0? */
			s = strchr(s,' ');
			if (!s) continue;
			s++;
			parse_value (s, type, &dpd->FORM.Range.MaximumValue); /* should turn ' ' into \0? */
			s = strchr(s,' ');
			if (!s) continue;
			s++;
			parse_value (s, type, &dpd->FORM.Range.StepSize); /* should turn ' ' into \0? */
			continue;
		}
		fprintf (stderr,"\tpropdescvar: %s\n", next->name);
		traverse_tree (3, next);
	} while ((next = xmlNextElementSibling (next)));
	return 1;
}

static int
parse_9301_prop_tree (xmlNodePtr node) {
	xmlNodePtr next;

	next = xmlFirstElementChild (node);
	do {
		PTPDevicePropDesc	dpd;
		parse_propdesc (next, &dpd);
	} while ((next = xmlNextElementSibling (next)));
	return TRUE;
}

static int
parse_9301_event_tree (xmlNodePtr node) {
	xmlNodePtr next;

	next = xmlFirstElementChild (node);
	do {
		fprintf (stderr,"9301 event: %s\n", next->name);
	} while ((next = xmlNextElementSibling (next)));
	return TRUE;
}

static int
parse_9301_tree (xmlNodePtr node) {
	xmlNodePtr next;

	next = xmlFirstElementChild (node);
	do {
		if (!strcmp ((char*)next->name, "cmd")) {
			parse_9301_cmd_tree (next);
			continue;
		}
		if (!strcmp ((char*)next->name, "prop")) {
			parse_9301_prop_tree (next);
			continue;
		}
		if (!strcmp ((char*)next->name, "event")) {
			parse_9301_event_tree (next);
			continue;
		}
		fprintf (stderr,"9301: unhandled type %s\n", next->name);
	} while ((next = xmlNextElementSibling (next)));
	/*traverse_tree (0, node);*/
	return TRUE;
}

static int
parse_910a_tree (xmlNodePtr node) {
	xmlNodePtr next;

	next = xmlFirstElementChild (node);
	do {
		if (!strcmp ((char*)next->name, "param")) {
			unsigned int x;
			xmlChar *xchar = xmlNodeGetContent (next);
			if (!sscanf((char*)xchar,"%08x", &x)) {
				fprintf(stderr,"could not parse param content %s\n", xchar);
			}
			fprintf(stderr,"param content is 0x%08x\n", x);
			continue;
		}
		fprintf (stderr,"910a: unhandled type %s\n", next->name);
	} while ((next = xmlNextElementSibling (next)));
	/*traverse_tree (0, node);*/
	return TRUE;
}

static int
parse_9302_tree (xmlNodePtr node) {
	xmlNodePtr	next;
	xmlChar		*xchar;
	
	next = xmlFirstElementChild (node);
	do {
		if (!strcmp((char*)next->name, "x3cVersion")) {
			int x3cver;
			xchar = xmlNodeGetContent (next);
			sscanf((char*)xchar, "%04x", &x3cver);
			fprintf(stderr,"x3cVersion %d.%d\n", (x3cver>>8)&0xff, x3cver&0xff);
			continue;
		}
		if (!strcmp((char*)next->name, "productIDs")) {
			char *x, *nextspace;
			int len;
			x = (char*)(xchar = xmlNodeGetContent (next));
			fprintf(stderr,"productIDs:\n");

			do {
				nextspace=strchr(x,' ');
				if (nextspace) nextspace++;

				/* ascii ptp string, 1 byte length, little endian 16 bit chars */
				if (sscanf(x,"%02x", &len)) {
					int i;
					char *str = malloc(len+1);
					for (i=0;i<len;i++) {
						int xc;
						if (sscanf(x+2+i*4,"%04x", &xc)) {
							int cx;

							cx = ((xc>>8) & 0xff) | ((xc & 0xff) << 8);
							str[i] = cx;
						}
						str[len] = 0;
					}
					fprintf(stderr,"\t%s\n", str);
					free (str);
				}
				x = nextspace;
			} while (x);
			continue;
		}
		fprintf (stderr, "unknown node in 9301: %s\n", next->name);
	} while ((next = xmlNextElementSibling (next)));
	return TRUE;
}

static int
parse_1014_tree (xmlNodePtr node) {
	PTPDevicePropDesc	dpd;
	
	return parse_propdesc (xmlFirstElementChild (node), &dpd);
}

static int
parse_1015_tree (xmlNodePtr node, uint16_t type) {
	PTPPropertyValue	propval;
	xmlNodePtr		next;

	next = xmlFirstElementChild (node);
	return parse_value ((char*)xmlNodeGetContent (next), type, &propval);
}

static int
traverse_output_tree (PTPParams *params, xmlNodePtr node, PTPContainer *resp) {
	xmlNodePtr next;
	int cmd;

	if (strcmp((char*)node->name,"output")) {
		fprintf(stderr,"node is not output, but %s\n", node->name);
		return FALSE;
	}
	if (xmlChildElementCount(node) != 2) {
		fprintf(stderr,"output: expected 2 childs, got %ld\n", xmlChildElementCount(node));
		return FALSE;
	}
	next = xmlFirstElementChild (node);
	if (!strcmp((char*)next->name,"result")) {
		int result;
		xmlChar *xchar;
		xchar = xmlNodeGetContent (next);
		if (!sscanf((char*)xchar,"%04x",&result))
			fprintf(stderr,"failed scanning result from %s\n", xchar);
		resp->Code = result;
	
	}
	next = xmlNextElementSibling (next);
	if (!sscanf ((char*)next->name, "c%04x", &cmd)) {
		fprintf(stderr,"expected c<HEX>, have %s\n", next->name);
		return FALSE;
	}
	fprintf(stderr,"cmd is 0x%04x\n", cmd);
	switch (cmd) {
	case 0x9301:
		return parse_9301_tree (next);
	case 0x9302:
		return parse_9302_tree (next);
	case 0x910a:
		return parse_910a_tree (next);
	case 0x1016: /* <output>\n<result>2001</result>\n<c1016>\n<pD135/>\n</c1016>\n</output> */
		/* we could cross check the parameter, but its not strictly necessary */
		return TRUE;
	case 0x1014:
		return parse_1014_tree ( next );
	case 0x1015:
		return parse_1015_tree ( next , PTP_DTC_UINT32);
	default:
		return traverse_tree (0, next);
	}
	return FALSE;
}

static int
traverse_input_tree (PTPParams *params, xmlNodePtr node, PTPContainer *resp) {
	return traverse_tree (0, node);
}

static int
traverse_x3c_tree (PTPParams *params, xmlNodePtr node, PTPContainer *resp) {
	xmlNodePtr	next;

	if (!node)
		return FALSE;
	if (strcmp((char*)node->name,"x3c")) {
		fprintf(stderr,"node is not x3c, but %s\n", node->name);
		return FALSE;
	}
	if (xmlChildElementCount(node) != 1) {
		fprintf(stderr,"x3c: expected 1 child, got %ld\n", xmlChildElementCount(node));
		return FALSE;
	}
	next = xmlFirstElementChild (node);
	if (!strcmp((char*)next->name, "output"))
		return traverse_output_tree (params, next, resp);
	if (!strcmp((char*)next->name, "input"))
		return traverse_input_tree (params, next, resp); /* event */
	fprintf(stderr,"unknown name %s below x3c\n", next->name);
	return FALSE;
}

static int
parse_xml(PTPParams *params, const char *txt, PTPContainer *resp) {
	xmlDocPtr	docin;
	xmlNodePtr	docroot;

	docin = xmlReadMemory (txt, strlen(txt), "http://gphoto.org/", "utf-8", 0);
	if (!docin) return FALSE;
	docroot = xmlDocGetRootElement (docin);
	if (!docroot) return FALSE;
	return traverse_x3c_tree (params, docroot, resp);
}

static void
encode_command (xmlNodePtr inputnode, PTPContainer *ptp, unsigned char *data, int len)
{
	xmlNodePtr	cmdnode;
	char 		code[20];

	sprintf(code,"c%04x", ptp->Code);
	cmdnode 	= xmlNewChild (inputnode, NULL, (xmlChar*)code, NULL);
	if (ptp->Nparam) {
		xmlNodePtr	pnode;

		switch (ptp->Nparam) {
		case 1:
			if (ptp->Param1 >= 0x10000)
				sprintf (code, "p%08x", ptp->Param1);
			else
				sprintf (code, "p%04x", ptp->Param1);
			pnode 	= xmlNewChild (inputnode, NULL, (xmlChar*)code, NULL);
			break;
		case 2:
			if (ptp->Param1 >= 0x10000)
				sprintf (code, "p%08x", ptp->Param1);
			else
				sprintf (code, "p%04x", ptp->Param1);
			pnode 	= xmlNewChild (inputnode, NULL, (xmlChar*)code, NULL);
			if (ptp->Param2 >= 0x10000)
				sprintf (code, "p%08x", ptp->Param2);
			else
				sprintf (code, "p%04x", ptp->Param2);
			pnode 	= xmlNewChild (inputnode, NULL, (xmlChar*)code, NULL);
			break;
		}
	}
	switch (ptp->Code) {
	case 0x1016: {
		/* We can directly byte encode the data we get from the PTP stack */
		int i;
		xmlNodePtr	vnode;
		char *x = malloc (len*2+1);

		for (i=0;i<len;i++)
			sprintf(x+2*i,"%02x",data[i]);
		vnode 	= xmlNewChild (inputnode, NULL, (xmlChar*)"value", (xmlChar*)x);
		free (x);
		break;
	}
	}
}

static char*
generate_xml(PTPParams *params, PTPContainer *ptp, unsigned char *data, int len) {
	xmlDocPtr	docout;
	xmlChar		*output;
	xmlNsPtr	outns;
	xmlNodePtr	x3cnode;
	xmlNodePtr	inputnode;

	docout 		= xmlNewDoc ((xmlChar*)"1.0");
	x3cnode		= xmlNewDocNode (docout, NULL, (xmlChar*)"x3c", NULL);
	outns 		= xmlNewNs (x3cnode,(xmlChar*)"http://www1.olympus-imaging.com/ww/x3c",NULL);
	inputnode 	= xmlNewChild (x3cnode, NULL, (xmlChar*)"input", NULL);

	/* the fun starts in here: */
	encode_command (inputnode, ptp, data, len);

	xmlDocSetRootElement (docout, x3cnode);
	xmlDocDumpMemory (docout, &output, &len);
	fprintf(stderr,"generated xml is:\n%s\n", output);
	return (char*)output;
}

#if 0
int
main(int argc, char **argv) {
	PTPContainer ptp;

	parse_xml("<?xml version=\"1.0\"?> <x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\"> <output> <result>2001</result> <c9302> <x3cVersion>0100</x3cVersion> <productIDs>224F0045003000360034003000300030003000300030003000300030002D00300030003000300031003000300039002D004700370033003500310039003500360033000000 224F004C003000300032003300300031003000300030003000300030002D00300030003000300031003300300035002D003200310033003000340033003600300037000000</productIDs> </c9302> </output> </x3c>");
	parse_xml("\
<?xml version=\"1.0\"?>\
<x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\">\
<output>\
<result>2001</result>\
<c9301>\
<cmd><c1001/>\
<c1002/>\
<c1014/>\
<c1015/>\
<c1016/>\
<c9101/>\
<c9103/>\
<c910C/>\
<c9107/>\
<c910A/>\
<c910B/>\
<c9301/>\
<c9302/>\
<c9501/>\
<c9402/>\
<c9581/>\
<c9482/>\
<c9108/>\
</cmd>\
<event><eC101/>\
<eC102/>\
<eC103/>\
<eC104/>\
</event>\
<prop><p5001>\
<type>0002</type>\
<attribute>00</attribute>\
<default>00</default>\
<value>64</value>\
<range>01 64 01</range>\
</p5001>\
<p5003>\
<type>FFFF</type>\
<attribute>01</attribute>\
<default>0A3400300033003200780033003000320034000000</default>\
<value>0A3400300033003200780033003000320034000000</value>\
<enum>0A3400300033003200780033003000320034000000</enum>\
</p5003>\
<p5007>\
<type>0004</type>\
<attribute>01</attribute>\
<default>017C</default>\
<value>0230</value>\
<enum>017C 0190 01C2 01F4 0230 0276 02C6 0320 0384 03E8 044C 0514 0578 0640 0708 07D0 0898</enum>\
</p5007>\
<p5008>\
<type>0006</type>\
<attribute>00</attribute>\
<default>00002710</default>\
<value>00000D48</value>\
<range>00000AF0 000020D0 00000064</range>\
</p5008>\
<p500A>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>8001</value>\
<enum>0001 0002 8001</enum>\
</p500A>\
<p500B>\
<type>0004</type>\
<attribute>01</attribute>\
<default>8001</default>\
<value>8001</value>\
<enum>0002 0004 8001 8011 8012</enum>\
</p500B>\
<p500C>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0004 8003 8001 8002 0003 0002 9001 9004 9010 9040</enum>\
</p500C>\
<p500E>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0003</value>\
<enum>FFFF 0001 0002 0003 0004 8001 8002 8003 8006 9006 9008 900A 900F 9011 9013 9014</enum>\
</p500E>\
<p500F>\
<type>0004</type>\
<attribute>01</attribute>\
<default>FFFF</default>\
<value>0190</value>\
<enum>FFFF 0064 007D 00A0 00C8 00FA 0140 0190 01F4 0280 0320 03E8 04E2 0640 07D0 09C4 0C80</enum>\
</p500F>\
<p5010>\
<type>0003</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>EC78 EDC6 EF13 F060 F1AE F2FB F448 F596 F6E3 F830 F97E FACB FC18 FD66 FEB3 0000 014D 029A 03E8 0535 0682 07D0 091D 0A6A 0BB8 0D05 0E52 0FA0 10ED 123A 1388</enum>\
</p5010>\
<p5013>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0022 0012</enum>\
</p5013>\
<p5014>\
<type>0002</type>\
<attribute>01</attribute>\
<default>00</default>\
<value>00</value>\
<enum>FE FF 00 01 02</enum>\
</p5014>\
<p5015>\
<type>0002</type>\
<attribute>01</attribute>\
<default>00</default>\
<value>00</value>\
<enum>FE FF 00 01 02</enum>\
</p5015>\
<p5018>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0002</value>\
<range>0001 0001 0001</range>\
</p5018>\
<p501C>\
<type>0004</type>\
<attribute>01</attribute>\
<default>8100</default>\
<value>8100</value>\
<enum>8100 8101</enum>\
</p501C>\
<pD102>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0102</default>\
<value>0020</value>\
<enum>0020 0101 0102 0103 0104 0121 0122 0123 0124</enum>\
</pD102>\
<pD103>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0001</value>\
<enum>0001 0002</enum>\
</pD103>\
<pD104>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0002 0003</enum>\
</pD104>\
<pD105>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0003</value>\
<enum>0001 0002 0003</enum>\
</pD105>\
<pD106>\
<type>0004</type>\
<attribute>01</attribute>\
<default>014D</default>\
<value>014D</value>\
<enum>03E8 01F4 014D</enum>\
</pD106>\
<pD107>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0002 8000 0004</enum>\
</pD107>\
<pD108>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001</enum>\
</pD108>\
<pD109>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0BB8</default>\
<value>0BB8</value>\
<enum>0BB8 0FA0 1194 14B4 157C 1770 19C8 1D4C</enum>\
</pD109>\
<pD10A>\
<type>0003</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>FFF9 FFFA FFFB FFFC FFFD FFFE FFFF 0000 0001 0002 0003 0004 0005 0006 0007</enum>\
</pD10A>\
<pD10B>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001</enum>\
</pD10B>\
<pD10C>\
<type>0004</type>\
<attribute>01</attribute>\
<default>1518</default>\
<value>1518</value>\
<enum>07D0 0802 0834 0866 0898 08CA 08FC 092E 0960 0992 09C4 09F6 0A28 0A5A 0A8C 0ABE 0AF0 0B54 0BB8 0C1C 0C80 0CE4 0D48 0DAC 0E10 0E74 0ED8 0F3C 0FA0 1068 1130 11F8 12C0 1388 1450 1518 15E0 16A8 1770 1838 1900 19C8 1A90 1B58 1CE8 1E78 2008 2198 2328 24B8 2648 2710 2AF8 2EE0 32C8 36B0</enum>\
</pD10C>\
<pD10D>\
<type>0006</type>\
<attribute>01</attribute>\
<default>000100FA</default>\
<value>000A000D</value>\
<enum>0258000A 01F4000A 0190000A 012C000A 00FA000A 00C8000A 0096000A 0082000A 0064000A 0050000A 003C000A 0032000A 0028000A 0020000A 0019000A 0014000A 0010000A 000D000A 000A000A 000A000D 000A0010 000A0014 000A0019 00010003 00010004 00010005 00010006 00010008 0001000A 0001000D 0001000F 00010014 00010019 0001001E 00010028 00010032 0001003C 00010050 00010064 0001007D 000100A0 000100C8 000100FA 00010140 00010190 000101F4 00010280 00010320 000103E8 000104E2 00010640 000107D0 000109C4 00010C80 00010FA0</enum>\
</pD10D>\
<pD10E>\
<type>0006</type>\
<attribute>01</attribute>\
<default>00000000</default>\
<value>00000000</value>\
<enum>00000000 01E00001</enum>\
</pD10E>\
<pD10F>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0000</value>\
<enum>0000 0001 0002 0003 0004 0005 0007 000A 000F 0014 0019 001E</enum>\
</pD10F>\
<pD11A>\
<type>0003</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>F448 F574 F704 F830 F97E FACB FC18 FD66 FEB3 0000 014D 029A 03E8 0535 0682 07D0 08FC 0A8C 0BB8</enum>\
</pD11A>\
<pD11E>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0000 0001</enum>\
</pD11E>\
<pD11F>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0000 0001</enum>\
</pD11F>\
<pD120>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0000 0001</enum>\
</pD120>\
<pD122>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0002</enum>\
</pD122>\
<pD124>\
<type>0003</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>FFFE FFFF 0000 0001 0002</enum>\
</pD124>\
<pD126>\
<type>0004</type>\
<attribute>01</attribute>\
<default>FFFF</default>\
<value>0000</value>\
<enum>FFFF 0000 0001</enum>\
</pD126>\
<pD127>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0000</value>\
<enum>0000 0010 0001 0100</enum>\
</pD127>\
<pD129>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0001</enum>\
</pD129>\
<pD12F>\
<type>0006</type>\
<attribute>01</attribute>\
<default>454E5500</default>\
<value>454E5500</value>\
<enum>454E5500 46524100 44455500 45535000 49544100 4A504E00 4B4F5200 43485300 43485400 52555300 43535900 4E4C4400 44414E00 504C4B00 50544700 53564500 4E4F5200 46494E00 48525600 534C5600 48554E00 454C4C00 534B5900 54524B00 4C564900 45544900 4C544800 554B5200 53524200 42475200 524F4D00 494E4400 4D534C00 54484100</enum>\
</pD12F>\
<pD130>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0005</default>\
<value>0005</value>\
<enum>0000 0001 0002 0003 0004 0005 0006 0007 0008 0009 000A 000B 000C 000D 000E 000F 0010 0011 0012 0013 0014 FFFF</enum>\
</pD130>\
<pD131>\
<type>0004</type>\
<attribute>01</attribute>\
<default>003C</default>\
<value>003C</value>\
<enum>0000 003C 00B4 012C 0258</enum>\
</pD131>\
<pD135>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0002</value>\
<enum>0001 0002</enum>\
</pD135>\
<pD136>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0001</enum>\
</pD136>\
<pD13A>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0001</value>\
<enum>FC18 0000 0001 03E8</enum>\
</pD13A>\
<pD12B>\
<type>0004</type>\
<attribute>01</attribute>\
<default>014D</default>\
<value>014D</value>\
<enum>03E8 014D</enum>\
</pD12B>\
<pD132>\
<type>0003</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>FFF9 FFFA FFFB FFFC FFFD FFFE FFFF 0000 0001 0002 0003 0004 0005 0006 0007</enum>\
</pD132>\
<pD12C>\
<type>0004</type>\
<default>0000</default>\
<value>0000</value>\
<enum>0000</enum>\
</pD12C>\
<pD12D>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000</enum>\
</pD12D>\
<pD13B>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0002</value>\
<enum>0000 0001 0002 8001 8003 8101</enum>\
</pD13B>\
<pD13E>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0002</value>\
<enum>0002</enum>\
</pD13E>\
<pD13D>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0001 0002</enum>\
</pD13D>\
<pD140>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0008</default>\
<value>0008</value>\
<enum>0008 001E 003C FFFF</enum>\
</pD140>\
<pD141>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0002</value>\
<enum>0000 0001 0002 8001 8003</enum>\
</pD141>\
<pD143>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0001</enum>\
</pD143>\
<pD144>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0002 0003</enum>\
</pD144>\
<pD145>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0002 0003 0004</enum>\
</pD145>\
<pD146>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0002</value>\
<enum>0002 0001</enum>\
</pD146>\
<pD147>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0002</default>\
<value>0002</value>\
<enum>0000 0001 0002 0003</enum>\
</pD147>\
<pD148>\
<type>0003</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>EC78 EDC6 EF13 F060 F1AE F2FB F448 F596 F6E3 F830 F97E FACB FC18 FD66 FEB3 0000 014D 029A 03E8 0535 0682 07D0 091D 0A6A 0BB8 0D05 0E52 0FA0 10ED 123A 1388</enum>\
</pD148>\
<pD149>\
<type>0006</type>\
<attribute>01</attribute>\
<default>00010001</default>\
<value>00010001</value>\
<enum>00010001 000A000D 000A0010 00010002 000A0019 00010003 00010004 00010005 00010006 00010008 0001000A 0001000D 00010010 00010014 00010019 00010020 00010028 00010032 00010040 00010050 00010064 00010080</enum>\
</pD149>\
<pD14A>\
<type>0003</type>\
<attribute>01</attribute>\
<default>FFFF</default>\
<value>FFFF</value>\
<enum>FFFF 0000 0001</enum>\
</pD14A>\
<pD14B>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0006</default>\
<value>0006</value>\
<enum>0002 0004 0005 0006 0007 0008 000A</enum>\
</pD14B>\
<pD14E>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0000 0001</enum>\
</pD14E>\
<pD14F>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0003</default>\
<value>0003</value>\
<enum>0001 0002 0003</enum>\
</pD14F>\
<pD150>\
<type>0004</type>\
<attribute>01</attribute>\
<default>00C8</default>\
<value>00C8</value>\
<enum>00C8 00FA 0140 0190 01F4 0280 0320 03E8 04E2 0640 07D0 09C4 0C80</enum>\
</pD150>\
<pD151>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0320</default>\
<value>0320</value>\
<enum>00C8 00FA 0140 0190 01F4 0280 0320 03E8 04E2 0640 07D0 09C4 0C80</enum>\
</pD151>\
<pD152>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0008</default>\
<value>0008</value>\
<enum>0001 0002 0004 0008 000F 0014 0019 001E</enum>\
</pD152>\
<pD153>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0010</enum>\
</pD153>\
<pD154>\
<type>0004</type>\
<attribute>01</attribute>\
<default>015E</default>\
<value>015E</value>\
<range>0001 270F 0001</range>\
</pD154>\
<pD157>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0001</enum>\
</pD157>\
<pD158>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0000 0001</enum>\
</pD158>\
<pD155>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0011 0012 0013 0014 0021 0022 0023 0024 0031 0032 0033 0034 0000</enum>\
</pD155>\
<pD159>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0303 0307 030A</enum>\
</pD159>\
<pD15A>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0314 0328 033C</enum>\
</pD15A>\
<pD15B>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0314 0328 033C</enum>\
</pD15B>\
<pD15C>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0303 0307 030A</enum>\
</pD15C>\
<pD15D>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0303 0307 030A</enum>\
</pD15D>\
<pD15F>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0000</default>\
<value>0000</value>\
<enum>0000 0001</enum>\
</pD15F>\
<pD160>\
<type>0004</type>\
<attribute>01</attribute>\
<default>0001</default>\
<value>0001</value>\
<enum>0001 0002</enum>\
</pD160>\
<pD161>\
<type>0006</type>\
<attribute>01</attribute>\
<default>00040003</default>\
<value>00040003</value>\
<enum>00040003 00030002 00100009 00060006</enum>\
</pD161>\
</prop>\
</c9301>\
</output>\
</x3c>");
	parse_xml ("<?xml version=\"1.0\"?>\n<x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\">\n<output>\n<result>2001</result>\n<c1016>\n<pD135/>\n</c1016>\n</output>\n</x3c>\n");

	parse_xml ("<?xml version=\"1.0\"?><x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\"><output><result>2001</result><c910A><param>00000001</param></c910A></output></x3c>\n");
	parse_xml ("<?xml version=\"1.0\"?><x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\"><output><result>2001</result><c1014><p5007><type>0004</type><attribute>01</attribute><default>017C</default><value>0230</value><enum>017C 0190 01C2 01F4 0230 0276 02C6 0320 0384 03E8 044C 0514 0578 0640 0708 07D0 0898</enum></p5007></c1014></output></x3c>");
	parse_xml ("<?xml version=\"1.0\"?><x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\"> <output> <result>2001</result> <c1015> <pD10E> <value>00000000</value> </pD10E> </c1015> </output> </x3c>");
	parse_xml ("<?xml version=\"1.0\"?><x3c xmlns=\"http://www1.olympus-imaging.com/ww/x3c\"><input><eC102><p5018/><pD126/><p5013/><pD104/><p500C/><pD106/><pD159/><pD103/><pD15C/><pD10E/></eC102></input></x3c>");

	ptp.Code = 0x1016;
	ptp.Nparam = 1;
	ptp.Param1 = 0x5007;
	generate_xml(&ptp, "abcd", 4);
	return;
}
#endif


/* 1014:
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1014><p5007/></c1014></input></x3c>

 */
/* 1016
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1016><p5007><value>0230</value></p5007></c1016></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1016>
<p5007/>
</c1016>
</output>
</x3c>


<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1016><pD10D><value>000A000D</value></pD10D></c1016></input></x3c>
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1016>
<pD10D/>
</c1016>
</output>
</x3c>
*/

/*
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1015><p5001/></c1015></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1015>
<p5001>
<value>64</value>
</p5001>
</c1015>
</output>
</x3c>
 */

/* event:
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><output><result>2001</result><eC102/></output></x3c>

*/

/*
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1015><pD10E/></c1015></input></x3c>

*/

static int
is_outer_operation (PTPParams* params, uint16_t opcode) {
	int i;

	/* Do nothing here, either do stuff in senddata, getdata or getresp,
	 * which will get the PTPContainer req too. */
        for (i=0;i<params->outer_deviceinfo.OperationsSupported_len;i++)
                if (params->outer_deviceinfo.OperationsSupported[i]==opcode)
                        return TRUE;
	return FALSE;
}

uint16_t
ums_wrap2_sendreq (PTPParams* params, PTPContainer* req)
{
	if (is_outer_operation (params,req->Code))
		return ums_wrap_sendreq (params,req);
	/* Wait for next state before doing stuff */
	params->olympus_cmd = NULL;
	return PTP_RC_OK;
}

uint16_t
ums_wrap2_senddata (
	PTPParams* params, PTPContainer* ptp, unsigned long sendlen, PTPDataHandler*getter
) {
	unsigned char	*data;
	uint16_t	ret;
	unsigned long	gotlen;

	if (is_outer_operation (params, ptp->Code))
		return ums_wrap_senddata (params, ptp, sendlen, getter);
	data = malloc (sendlen);
	ret = getter->getfunc(params, getter->priv, sendlen, data, &gotlen);
	if (ret != PTP_RC_OK) {
		GP_DEBUG( "ums_wrap2_senddata *** data get from handler FAILED, ret %d", ret );
		return ret;
	}
	params->olympus_cmd = generate_xml (params, ptp, data, sendlen);
	free (data);
	/* Do not do stuff yet, do it in getresp */
	return PTP_RC_OK;
}

uint16_t
ums_wrap2_getdata (PTPParams* params, PTPContainer* ptp, PTPDataHandler *putter) {
	PTPContainer tmp;
	if (is_outer_operation (params, ptp->Code))
		return ums_wrap_getdata (params, ptp, putter);
	/* either send or get data, not both. So olympus_cmd is NULL now */
	params->olympus_cmd = generate_xml (params, ptp, NULL, 0);
	/* do the fun stuff */

	/* we must not yet overwrite the ptp content, it is used in getresp */
	memcpy (&tmp, ptp, sizeof(tmp));
	parse_xml (params, "txt", &tmp);
	return PTP_RC_OK;
}

uint16_t
ums_wrap2_getresp (PTPParams* params, PTPContainer* resp) {
	if (is_outer_operation(params, resp->Code))
		return ums_wrap_getresp (params, resp);

	if (!params->olympus_cmd) /* no data phase at all */
		params->olympus_cmd = generate_xml (params, resp, NULL, 0);
	if (!params->olympus_reply) {
		/* do the fun stuff */
	}
	parse_xml (params, "txt", resp);
	return PTP_RC_OK;
}

uint16_t
ums_wrap2_event_check (PTPParams* params, PTPContainer* event) {
	/* insert our code */
	return ptp_usb_event_check (params, event);
}

uint16_t
ums_wrap2_event_wait (PTPParams* params, PTPContainer* event) {
	/* insert our code */
	return ptp_usb_event_wait (params, event);
}

uint16_t
olympus_setup (PTPParams *params) {
	uint16_t	ret;

	memcpy (&params->deviceinfo,&params->deviceinfo,sizeof(params->deviceinfo));
	params->getresp_func	= ums_wrap2_getresp;
	params->senddata_func	= ums_wrap2_senddata;
	params->getdata_func	= ums_wrap2_getdata;
	params->sendreq_func	= ums_wrap2_sendreq;
	params->event_check	= ums_wrap2_event_check;
	params->event_wait	= ums_wrap2_event_wait;

	/* overwrite cmds, props and events with xml passed deviceinfo */
	ret = ptp_olympus_getdeviceinfo (params, &params->deviceinfo);
	if (ret != PTP_RC_OK)
		return ret;
	return PTP_RC_OK;
}


/* 9302:
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c9302/></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c9302>
<x3cVersion>0100</x3cVersion>
<productIDs>224F0045003000360034003000300030003000300030003000300030002D00300030003000300031003000300039002D004700370033003500310039003500360033000000 224F004C003000300032003300300031003000300030003000300030002D00300030003000300031003300300035002D003200310033003000340033003600300037000000</productIDs>
</c9302>
</output>
</x3c>
*/
/* 9301
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c9301/></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c9301>
<cmd><c1001/>
<c1002/>
<c1014/>
<c1015/>
<c1016/>
<c9101/>
<c9103/>
<c910C/>
<c9107/>
<c910A/>
<c910B/>
<c9301/>
<c9302/>
<c9501/>
<c9402/>
<c9581/>
<c9482/>
<c9108/>
</cmd>
<event><eC101/>
<eC102/>
<eC103/>
<eC104/>
</event>
<prop><p5001>
<type>0002</type>
<attribute>00</attribute>
<default>00</default>
<value>64</value>
<range>01 64 01</range>
</p5001>
<p5003>
<type>FFFF</type>
<attribute>01</attribute>
<default>0A3400300033003200780033003000320034000000</default>
<value>0A3400300033003200780033003000320034000000</value>
<enum>0A3400300033003200780033003000320034000000</enum>
</p5003>
<p5007>
<type>0004</type>
<attribute>01</attribute>
<default>017C</default>
<value>0230</value>
<enum>017C 0190 01C2 01F4 0230 0276 02C6 0320 0384 03E8 044C 0514 0578 0640 0708 07D0 0898</enum>
</p5007>
<p5008>
<type>0006</type>
<attribute>00</attribute>
<default>00002710</default>
<value>00000D48</value>
<range>00000AF0 000020D0 00000064</range>
</p5008>
<p500A>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>8001</value>
<enum>0001 0002 8001</enum>
</p500A>
<p500B>
<type>0004</type>
<attribute>01</attribute>
<default>8001</default>
<value>8001</value>
<enum>0002 0004 8001 8011 8012</enum>
</p500B>
<p500C>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0004 8003 8001 8002 0003 0002 9001 9004 9010 9040</enum>
</p500C>
<p500E>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0003</value>
<enum>FFFF 0001 0002 0003 0004 8001 8002 8003 8006 9006 9008 900A 900F 9011 9013 9014</enum>
</p500E>
<p500F>
<type>0004</type>
<attribute>01</attribute>
<default>FFFF</default>
<value>0190</value>
<enum>FFFF 0064 007D 00A0 00C8 00FA 0140 0190 01F4 0280 0320 03E8 04E2 0640 07D0 09C4 0C80</enum>
</p500F>
<p5010>
<type>0003</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>EC78 EDC6 EF13 F060 F1AE F2FB F448 F596 F6E3 F830 F97E FACB FC18 FD66 FEB3 0000 014D 029A 03E8 0535 0682 07D0 091D 0A6A 0BB8 0D05 0E52 0FA0 10ED 123A 1388</enum>
</p5010>
<p5013>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0022 0012</enum>
</p5013>
<p5014>
<type>0002</type>
<attribute>01</attribute>
<default>00</default>
<value>00</value>
<enum>FE FF 00 01 02</enum>
</p5014>
<p5015>
<type>0002</type>
<attribute>01</attribute>
<default>00</default>
<value>00</value>
<enum>FE FF 00 01 02</enum>
</p5015>
<p5018>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0002</value>
<range>0001 0001 0001</range>
</p5018>
<p501C>
<type>0004</type>
<attribute>01</attribute>
<default>8100</default>
<value>8100</value>
<enum>8100 8101</enum>
</p501C>
<pD102>
<type>0004</type>
<attribute>01</attribute>
<default>0102</default>
<value>0020</value>
<enum>0020 0101 0102 0103 0104 0121 0122 0123 0124</enum>
</pD102>
<pD103>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0001</value>
<enum>0001 0002</enum>
</pD103>
<pD104>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0002 0003</enum>
</pD104>
<pD105>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0003</value>
<enum>0001 0002 0003</enum>
</pD105>
<pD106>
<type>0004</type>
<attribute>01</attribute>
<default>014D</default>
<value>014D</value>
<enum>03E8 01F4 014D</enum>
</pD106>
<pD107>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0002 8000 0004</enum>
</pD107>
<pD108>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001</enum>
</pD108>
<pD109>
<type>0004</type>
<attribute>01</attribute>
<default>0BB8</default>
<value>0BB8</value>
<enum>0BB8 0FA0 1194 14B4 157C 1770 19C8 1D4C</enum>
</pD109>
<pD10A>
<type>0003</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>FFF9 FFFA FFFB FFFC FFFD FFFE FFFF 0000 0001 0002 0003 0004 0005 0006 0007</enum>
</pD10A>
<pD10B>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001</enum>
</pD10B>
<pD10C>
<type>0004</type>
<attribute>01</attribute>
<default>1518</default>
<value>1518</value>
<enum>07D0 0802 0834 0866 0898 08CA 08FC 092E 0960 0992 09C4 09F6 0A28 0A5A 0A8C 0ABE 0AF0 0B54 0BB8 0C1C 0C80 0CE4 0D48 0DAC 0E10 0E74 0ED8 0F3C 0FA0 1068 1130 11F8 12C0 1388 1450 1518 15E0 16A8 1770 1838 1900 19C8 1A90 1B58 1CE8 1E78 2008 2198 2328 24B8 2648 2710 2AF8 2EE0 32C8 36B0</enum>
</pD10C>
<pD10D>
<type>0006</type>
<attribute>01</attribute>
<default>000100FA</default>
<value>000A000D</value>
<enum>0258000A 01F4000A 0190000A 012C000A 00FA000A 00C8000A 0096000A 0082000A 0064000A 0050000A 003C000A 0032000A 0028000A 0020000A 0019000A 0014000A 0010000A 000D000A 000A000A 000A000D 000A0010 000A0014 000A0019 00010003 00010004 00010005 00010006 00010008 0001000A 0001000D 0001000F 00010014 00010019 0001001E 00010028 00010032 0001003C 00010050 00010064 0001007D 000100A0 000100C8 000100FA 00010140 00010190 000101F4 00010280 00010320 000103E8 000104E2 00010640 000107D0 000109C4 00010C80 00010FA0</enum>
</pD10D>
<pD10E>
<type>0006</type>
<attribute>01</attribute>
<default>00000000</default>
<value>00000000</value>
<enum>00000000 01E00001</enum>
</pD10E>
<pD10F>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0000</value>
<enum>0000 0001 0002 0003 0004 0005 0007 000A 000F 0014 0019 001E</enum>
</pD10F>
<pD11A>
<type>0003</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>F448 F574 F704 F830 F97E FACB FC18 FD66 FEB3 0000 014D 029A 03E8 0535 0682 07D0 08FC 0A8C 0BB8</enum>
</pD11A>
<pD11E>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0000 0001</enum>
</pD11E>
<pD11F>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0000 0001</enum>
</pD11F>
<pD120>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0000 0001</enum>
</pD120>
<pD122>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0002</enum>
</pD122>
<pD124>
<type>0003</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>FFFE FFFF 0000 0001 0002</enum>
</pD124>
<pD126>
<type>0004</type>
<attribute>01</attribute>
<default>FFFF</default>
<value>0000</value>
<enum>FFFF 0000 0001</enum>
</pD126>
<pD127>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0000</value>
<enum>0000 0010 0001 0100</enum>
</pD127>
<pD129>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0001</enum>
</pD129>
<pD12F>
<type>0006</type>
<attribute>01</attribute>
<default>454E5500</default>
<value>454E5500</value>
<enum>454E5500 46524100 44455500 45535000 49544100 4A504E00 4B4F5200 43485300 43485400 52555300 43535900 4E4C4400 44414E00 504C4B00 50544700 53564500 4E4F5200 46494E00 48525600 534C5600 48554E00 454C4C00 534B5900 54524B00 4C564900 45544900 4C544800 554B5200 53524200 42475200 524F4D00 494E4400 4D534C00 54484100</enum>
</pD12F>
<pD130>
<type>0004</type>
<attribute>01</attribute>
<default>0005</default>
<value>0005</value>
<enum>0000 0001 0002 0003 0004 0005 0006 0007 0008 0009 000A 000B 000C 000D 000E 000F 0010 0011 0012 0013 0014 FFFF</enum>
</pD130>
<pD131>
<type>0004</type>
<attribute>01</attribute>
<default>003C</default>
<value>003C</value>
<enum>0000 003C 00B4 012C 0258</enum>
</pD131>
<pD135>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0002</value>
<enum>0001 0002</enum>
</pD135>
<pD136>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0001</enum>
</pD136>
<pD13A>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0001</value>
<enum>FC18 0000 0001 03E8</enum>
</pD13A>
<pD12B>
<type>0004</type>
<attribute>01</attribute>
<default>014D</default>
<value>014D</value>
<enum>03E8 014D</enum>
</pD12B>
<pD132>
<type>0003</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>FFF9 FFFA FFFB FFFC FFFD FFFE FFFF 0000 0001 0002 0003 0004 0005 0006 0007</enum>
</pD132>
<pD12C>
<type>0004</type>
<default>0000</default>
<value>0000</value>
<enum>0000</enum>
</pD12C>
<pD12D>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000</enum>
</pD12D>
<pD13B>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0002</value>
<enum>0000 0001 0002 8001 8003 8101</enum>
</pD13B>
<pD13E>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0002</value>
<enum>0002</enum>
</pD13E>
<pD13D>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0001 0002</enum>
</pD13D>
<pD140>
<type>0004</type>
<attribute>01</attribute>
<default>0008</default>
<value>0008</value>
<enum>0008 001E 003C FFFF</enum>
</pD140>
<pD141>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0002</value>
<enum>0000 0001 0002 8001 8003</enum>
</pD141>
<pD143>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0001</enum>
</pD143>
<pD144>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0002 0003</enum>
</pD144>
<pD145>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0002 0003 0004</enum>
</pD145>
<pD146>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0002</value>
<enum>0002 0001</enum>
</pD146>
<pD147>
<type>0004</type>
<attribute>01</attribute>
<default>0002</default>
<value>0002</value>
<enum>0000 0001 0002 0003</enum>
</pD147>
<pD148>
<type>0003</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>EC78 EDC6 EF13 F060 F1AE F2FB F448 F596 F6E3 F830 F97E FACB FC18 FD66 FEB3 0000 014D 029A 03E8 0535 0682 07D0 091D 0A6A 0BB8 0D05 0E52 0FA0 10ED 123A 1388</enum>
</pD148>
<pD149>
<type>0006</type>
<attribute>01</attribute>
<default>00010001</default>
<value>00010001</value>
<enum>00010001 000A000D 000A0010 00010002 000A0019 00010003 00010004 00010005 00010006 00010008 0001000A 0001000D 00010010 00010014 00010019 00010020 00010028 00010032 00010040 00010050 00010064 00010080</enum>
</pD149>
<pD14A>
<type>0003</type>
<attribute>01</attribute>
<default>FFFF</default>
<value>FFFF</value>
<enum>FFFF 0000 0001</enum>
</pD14A>
<pD14B>
<type>0004</type>
<attribute>01</attribute>
<default>0006</default>
<value>0006</value>
<enum>0002 0004 0005 0006 0007 0008 000A</enum>
</pD14B>
<pD14E>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0000 0001</enum>
</pD14E>
<pD14F>
<type>0004</type>
<attribute>01</attribute>
<default>0003</default>
<value>0003</value>
<enum>0001 0002 0003</enum>
</pD14F>
<pD150>
<type>0004</type>
<attribute>01</attribute>
<default>00C8</default>
<value>00C8</value>
<enum>00C8 00FA 0140 0190 01F4 0280 0320 03E8 04E2 0640 07D0 09C4 0C80</enum>
</pD150>
<pD151>
<type>0004</type>
<attribute>01</attribute>
<default>0320</default>
<value>0320</value>
<enum>00C8 00FA 0140 0190 01F4 0280 0320 03E8 04E2 0640 07D0 09C4 0C80</enum>
</pD151>
<pD152>
<type>0004</type>
<attribute>01</attribute>
<default>0008</default>
<value>0008</value>
<enum>0001 0002 0004 0008 000F 0014 0019 001E</enum>
</pD152>
<pD153>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0010</enum>
</pD153>
<pD154>
<type>0004</type>
<attribute>01</attribute>
<default>015E</default>
<value>015E</value>
<range>0001 270F 0001</range>
</pD154>
<pD157>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0001</enum>
</pD157>
<pD158>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0000 0001</enum>
</pD158>
<pD155>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0011 0012 0013 0014 0021 0022 0023 0024 0031 0032 0033 0034 0000</enum>
</pD155>
<pD159>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0303 0307 030A</enum>
</pD159>
<pD15A>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0314 0328 033C</enum>
</pD15A>
<pD15B>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0314 0328 033C</enum>
</pD15B>
<pD15C>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0303 0307 030A</enum>
</pD15C>
<pD15D>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0303 0307 030A</enum>
</pD15D>
<pD15F>
<type>0004</type>
<attribute>01</attribute>
<default>0000</default>
<value>0000</value>
<enum>0000 0001</enum>
</pD15F>
<pD160>
<type>0004</type>
<attribute>01</attribute>
<default>0001</default>
<value>0001</value>
<enum>0001 0002</enum>
</pD160>
<pD161>
<type>0006</type>
<attribute>01</attribute>
<default>00040003</default>
<value>00040003</value>
<enum>00040003 00030002 00100009 00060006</enum>
</pD161>
</prop>
</c9301>
</output>
</x3c>
 */
/* 910a: GetCameraControlMode
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c910A/></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c910A>
<param>00000001</param>
</c910A>
</output>
</x3c>

 */
/* 1014:
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1014><p5007/></c1014></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1014>
<p5007>
<type>0004</type>
<attribute>01</attribute>
<default>017C</default>
<value>0230</value>
<enum>017C 0190 01C2 01F4 0230 0276 02C6 0320 0384 03E8 044C 0514 0578 0640 0708 07D0 0898</enum>
</p5007>
</c1014>
</output>
</x3c>

 */
/* 1016
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1016><p5007><value>0230</value></p5007></c1016></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1016>
<p5007/>
</c1016>
</output>
</x3c>


<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1016><pD10D><value>000A000D</value></pD10D></c1016></input></x3c>
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1016>
<pD10D/>
</c1016>
</output>
</x3c>
*/

/*
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1015><p5001/></c1015></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1015>
<p5001>
<value>64</value>
</p5001>
</c1015>
</output>
</x3c>
 */

/* event:
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<input>
<eC102>
<p5018/>
<pD126/>
<p5013/>
<pD104/>
<p500C/>
<pD106/>
<pD159/>
<pD103/>
<pD15C/>
<pD10E/>
</eC102>
</input>
</x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><output><result>2001</result><eC102/></output></x3c>

*/

/*
<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c"><input><c1015><pD10E/></c1015></input></x3c>

<?xml version="1.0"?>
<x3c xmlns="http://www1.olympus-imaging.com/ww/x3c">
<output>
<result>2001</result>
<c1015>
<pD10E>
<value>00000000</value>
</pD10E>
</c1015>
</output>
</x3c>
*/

