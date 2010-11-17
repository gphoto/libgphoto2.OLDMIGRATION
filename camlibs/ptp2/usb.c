/* usb.c
 *
 * Copyright (C) 2001-2004 Mariusz Woloszyn <emsi@ipartners.pl>
 * Copyright (C) 2003-2007 Marcus Meissner <marcus@jet.franken.de>
 * Copyright (C) 2006-2007 Linus Walleij <triad@df.lth.se>
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

#define _BSD_SOURCE
#include <config.h>
#include "ptp.h"
#include "ptp-private.h"
#include "ptp-bugs.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gphoto2/gphoto2-library.h>
#include <gphoto2/gphoto2-port-log.h>
#include <gphoto2/gphoto2-setting.h>

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

#define CONTEXT_BLOCK_SIZE	100000

#define PTP_CNT_INIT(cnt) {memset(&cnt,0,sizeof(cnt));}

/* PTP2_FAST_TIMEOUT: how long (in milliseconds) we should wait for
 * an URB to come back on an interrupt endpoint */
#define PTP2_FAST_TIMEOUT       50

/* Pack / unpack functions */

#include "ptp-pack.c"

/* send / receive functions */

uint16_t
ptp_usb_sendreq (PTPParams* params, PTPContainer* req)
{
	int res;
	PTPUSBBulkContainer usbreq;
	unsigned long towrite;
	Camera *camera = ((PTPData *)params->data)->camera;

	/* build appropriate USB container */
	usbreq.length=htod32(PTP_USB_BULK_REQ_LEN-
		(sizeof(uint32_t)*(5-req->Nparam)));
	usbreq.type=htod16(PTP_USB_CONTAINER_COMMAND);
	usbreq.code=htod16(req->Code);
	usbreq.trans_id=htod32(req->Transaction_ID);
	usbreq.payload.params.param1=htod32(req->Param1);
	usbreq.payload.params.param2=htod32(req->Param2);
	usbreq.payload.params.param3=htod32(req->Param3);
	usbreq.payload.params.param4=htod32(req->Param4);
	usbreq.payload.params.param5=htod32(req->Param5);
	/* send it to responder */
	towrite = PTP_USB_BULK_REQ_LEN-(sizeof(uint32_t)*(5-req->Nparam));
	res = gp_port_write (camera->port, (char*)&usbreq, towrite);
	if (res != towrite) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_sendreq",
			"request code 0x%04x sending req result %d",
			req->Code,res);
		return PTP_ERROR_IO;
	}
	return PTP_RC_OK;
}

uint16_t
ptp_usb_senddata (PTPParams* params, PTPContainer* ptp,
		  unsigned long size, PTPDataHandler *handler
) {
	uint16_t ret = PTP_RC_OK;
	int res, wlen, datawlen;
	PTPUSBBulkContainer usbdata;
	unsigned long bytes_left_to_transfer, written;
	Camera *camera = ((PTPData *)params->data)->camera;
	unsigned char *bytes;
	int progressid = 0;
	int usecontext = (size > CONTEXT_BLOCK_SIZE);
	GPContext *context = ((PTPData *)params->data)->context;

	/* build appropriate USB container */
	usbdata.length	= htod32(PTP_USB_BULK_HDR_LEN+size);
	usbdata.type	= htod16(PTP_USB_CONTAINER_DATA);
	usbdata.code	= htod16(ptp->Code);
	usbdata.trans_id= htod32(ptp->Transaction_ID);

	if (params->split_header_data) {
		datawlen = 0;
		wlen = PTP_USB_BULK_HDR_LEN;
	} else {
		unsigned long gotlen;
		/* For all camera devices. */
		datawlen = (size<PTP_USB_BULK_PAYLOAD_LEN_WRITE)?size:PTP_USB_BULK_PAYLOAD_LEN_WRITE;
		wlen = PTP_USB_BULK_HDR_LEN + datawlen;
		ret = handler->getfunc(params, handler->priv, datawlen, usbdata.payload.data, &gotlen);
		if (ret != PTP_RC_OK)
			return ret;
		if (gotlen != datawlen)
			return PTP_RC_GeneralError;
	}
	res = gp_port_write (camera->port, (char*)&usbdata, wlen);
	if (res != wlen) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_senddata",
		"request code 0x%04x sending data error 0x%04x",
			ptp->Code,ret);
		return PTP_ERROR_IO;
	}
	if (size <= datawlen) { /* nothing more to do */
		written = wlen;
		goto finalize;
	}
	if (usecontext)
		progressid = gp_context_progress_start (context, (size/CONTEXT_BLOCK_SIZE), _("Uploading..."));
	bytes = malloc (4096);
	if (!bytes)
		return PTP_RC_GeneralError;
	/* if everything OK send the rest */
	bytes_left_to_transfer = size-datawlen;
	ret = PTP_RC_OK;
	written = 0;
	while(bytes_left_to_transfer > 0) {
		unsigned long readlen, toread, oldwritten = written;
		int res;

		toread = 4096;
		if (toread > bytes_left_to_transfer)
			toread = bytes_left_to_transfer;
		ret = handler->getfunc (params, handler->priv, toread, bytes, &readlen);
		if (ret != PTP_RC_OK)
			break;
		res = gp_port_write (camera->port, (char*)bytes, readlen);
		if (res < 0) {
			ret = PTP_ERROR_IO;
			break;
		}
		bytes_left_to_transfer -= res;
		written += res;
		if (usecontext && (oldwritten/CONTEXT_BLOCK_SIZE < written/CONTEXT_BLOCK_SIZE))
			gp_context_progress_update (context, progressid, written/CONTEXT_BLOCK_SIZE);
#if 0 /* Does not work this way... Hmm. */
		if (gp_context_cancel(context) == GP_CONTEXT_FEEDBACK_CANCEL) {
			ret = ptp_usb_control_cancel_request (params,ptp->Transaction_ID);
			if (ret == PTP_RC_OK)
				ret = PTP_ERROR_CANCEL;
			break;
		}
#endif
	}
	if (usecontext)
		gp_context_progress_stop (context, progressid);
	free (bytes);
finalize:
	if ((ret == PTP_RC_OK) && ((written % params->maxpacketsize) == 0))
		gp_port_write (camera->port, "x", 0);
	if ((ret!=PTP_RC_OK) && (ret!=PTP_ERROR_CANCEL))
		ret = PTP_ERROR_IO;
	return ret;
}

static uint16_t
ptp_usb_getpacket(PTPParams *params,
		PTPUSBBulkContainer *packet, unsigned long *rlen)
{
	int		tries = 0, result;
	Camera		*camera = ((PTPData *)params->data)->camera;

	gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getpacket", "getting next ptp packet");
	/* read the header and potentially the first data */
	if (params->response_packet_size > 0) {
		gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getpacket", "queuing buffered response packet");
		/* If there is a buffered packet, just use it. */
		memcpy(packet, params->response_packet, params->response_packet_size);
		*rlen = params->response_packet_size;
		free(params->response_packet);
		params->response_packet = NULL;
		params->response_packet_size = 0;
		/* Here this signifies a "virtual read" */
		return PTP_RC_OK;
	}
retry:
	/* A packet should come in a single read always. */
	result = gp_port_read (camera->port, (char*)packet, sizeof(*packet));
	/* This might be a left over zero-write of the device at the end of the previous transmission */
	if (result == 0)
		result = gp_port_read (camera->port, (char*)packet, sizeof(*packet));
	if (result > 0) {
		*rlen = result;
		return PTP_RC_OK;
	}
	if (result == GP_ERROR_IO_READ) {
		gp_log (GP_LOG_DEBUG, "ptp2/usbread", "Clearing halt on IN EP and retrying once.");
		gp_port_usb_clear_halt (camera->port, GP_PORT_USB_ENDPOINT_IN);
		/* retrying only makes sense if we did not read anything yet */
		if ((tries++ < 1) && (result == 0))
			goto retry;
	}
	return PTP_ERROR_IO;
}

#define READLEN 64*1024 /* read blob size */

uint16_t
ptp_usb_getdata (PTPParams* params, PTPContainer* ptp, PTPDataHandler *handler)
{
	uint16_t ret;
	PTPUSBBulkContainer usbdata;
	unsigned char	*data;
	unsigned long	bytes_to_read, written, curread, oldsize;
	Camera		*camera = ((PTPData *)params->data)->camera;
	int usecontext, progressid = 0, tries = 0, res;
	GPContext *context = ((PTPData *)params->data)->context;

	gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getdata", "reading data");
	PTP_CNT_INIT(usbdata);
	do {
		unsigned long len, rlen;

		ret = ptp_usb_getpacket(params, &usbdata, &rlen);
		if (ret!=PTP_RC_OK) {
			ret = PTP_ERROR_IO;
			break;
		}
		if (dtoh16(usbdata.type)!=PTP_USB_CONTAINER_DATA) {
			/* We might have got a response instead. On error for instance. */
			if (dtoh16(usbdata.type) == PTP_USB_CONTAINER_RESPONSE) {
				params->response_packet = malloc(dtoh32(usbdata.length));
				if (!params->response_packet) return PTP_RC_GeneralError;
				memcpy(params->response_packet,
				       (uint8_t *) &usbdata, dtoh32(usbdata.length));
				params->response_packet_size = dtoh32(usbdata.length);
				ret = PTP_RC_OK;
			} else {
				ret = PTP_ERROR_DATA_EXPECTED;
			}
			break;
		}
		if (dtoh16(usbdata.code)!=ptp->Code) {
			/* A creative Zen device breaks down here, by leaving out
			 * Code and Transaction ID */
			if (MTP_ZEN_BROKEN_HEADER(params)) {
				gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getdata", "Read broken PTP header (Code is %04x vs %04x), compensating.",
					dtoh16(usbdata.code), ptp->Code
				);
				usbdata.code = dtoh16(ptp->Code);
				usbdata.trans_id = htod32(ptp->Transaction_ID);
			} else {
				gp_log (GP_LOG_ERROR, "ptp2/ptp_usb_getdata", "Read broken PTP header (Code is %04x vs %04x).",
					dtoh16(usbdata.code), ptp->Code
				);
				ret = PTP_ERROR_IO;
				break;
			}
		}
		if (usbdata.length == 0xffffffffU) {
			unsigned char	*data = malloc (PTP_USB_BULK_HS_MAX_PACKET_LEN_READ);
			if (!data) return PTP_RC_GeneralError;
			/* Copy first part of data to 'data' */
			handler->putfunc(
				params, handler->priv, rlen - PTP_USB_BULK_HDR_LEN, usbdata.payload.data,
				&written
			);
			/* stuff data directly to passed data handler */
			while (1) {
				unsigned long written;
				int result = gp_port_read (camera->port, (char*)data, PTP_USB_BULK_HS_MAX_PACKET_LEN_READ);
				if (result < 0) {
					free (data);
					return PTP_ERROR_IO;
				}
				handler->putfunc (params, handler->priv, result, data, &written);
				if (result < PTP_USB_BULK_HS_MAX_PACKET_LEN_READ) 
					break;
			}
			free (data);
			return PTP_RC_OK;
		}
		if (rlen > dtoh32(usbdata.length)) {
			/*
			 * Buffer the surplus response packet if it is >=
			 * PTP_USB_BULK_HDR_LEN
			 * (i.e. it is probably an entire package)
			 * else discard it as erroneous surplus data.
			 * This will even work if more than 2 packets appear
			 * in the same transaction, they will just be handled
			 * iteratively.
			 *
			 * Marcus observed stray bytes on iRiver devices;
			 * these are still discarded.
			 */
			unsigned int packlen = dtoh32(usbdata.length);
			unsigned int surplen = rlen - packlen;

			if (surplen >= PTP_USB_BULK_HDR_LEN) {
				params->response_packet = malloc(surplen);
				if (!params->response_packet) return PTP_RC_GeneralError;
				memcpy(params->response_packet,
				       (uint8_t *) &usbdata + packlen, surplen);
				params->response_packet_size = surplen;
			} else {
				gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getdata", "read %ld bytes too much, expect problems!", rlen - dtoh32(usbdata.length));
			}
			rlen = packlen;
		}

		/* For most PTP devices rlen is 512 == sizeof(usbdata)
		 * here. For MTP devices splitting header and data it might
		 * be 12.
		 */
		/* Evaluate full data length. */
		len=dtoh32(usbdata.length)-PTP_USB_BULK_HDR_LEN;

		/* autodetect split header/data MTP devices */
		if (dtoh32(usbdata.length) > 12 && (rlen==12))
			params->split_header_data = 1;

		/* Copy first part of data to 'data' */
		handler->putfunc(
			params, handler->priv, rlen - PTP_USB_BULK_HDR_LEN, usbdata.payload.data,
			&written
		);
		/* Is that all of data? */
		if (len+PTP_USB_BULK_HDR_LEN<=rlen) break;

		/* If not read the rest of it. */
retry:
		oldsize = 0;
		data = malloc(READLEN);
		if (!data) return PTP_RC_GeneralError;
		bytes_to_read = len - (rlen - PTP_USB_BULK_HDR_LEN);
		usecontext = (bytes_to_read > CONTEXT_BLOCK_SIZE);
		ret = PTP_RC_OK;
		if (usecontext)
			progressid = gp_context_progress_start (context, (bytes_to_read/CONTEXT_BLOCK_SIZE), _("Downloading..."));
		curread = 0; res = 0;
		while (bytes_to_read > 0) {
			unsigned long toread = bytes_to_read;
			int res;

			/* read in large blobs.
			 * if smaller than large blob, read all but the last short packet
			 * depending on EP packetsize.
			 */
			if (toread > READLEN)
				toread = READLEN;
			else if (toread > params->maxpacketsize)
				toread = toread - (toread % params->maxpacketsize);
			res = gp_port_read (camera->port, (char*)data, toread);
			if (res <= 0) {
				ret = PTP_ERROR_IO;
				break;
			}
			ret = handler->putfunc (params, handler->priv,
				res, data, &written
			);
			if (ret != PTP_RC_OK)
				break;
			if (written != res) {
				ret = PTP_ERROR_IO;
				break;
			}
			bytes_to_read -= res;
			curread += res;
			if (usecontext && (oldsize/CONTEXT_BLOCK_SIZE < curread/CONTEXT_BLOCK_SIZE))
				gp_context_progress_update (context, progressid, curread/CONTEXT_BLOCK_SIZE);
			if (gp_context_cancel(context) == GP_CONTEXT_FEEDBACK_CANCEL) {
				ret = PTP_ERROR_CANCEL;
				break;
			}
			oldsize = curread;
		}
		free (data);
		if (usecontext)
			gp_context_progress_stop (context, progressid);
		if (res == GP_ERROR_IO_READ) {
			gp_log (GP_LOG_DEBUG, "ptp2/usbread", "Clearing halt on IN EP and retrying once.");
			gp_port_usb_clear_halt (camera->port, GP_PORT_USB_ENDPOINT_IN);
			/* retrying only makes sense if we did not read anything yet */
			if ((tries++ < 1) && (curread == 0))
				goto retry;
		}

		if ((ret!=PTP_RC_OK) && (ret!=PTP_ERROR_CANCEL)) {
			ret = PTP_ERROR_IO;
			break;
		}
	} while (0);

	if ((ret!=PTP_RC_OK) && (ret!=PTP_ERROR_CANCEL)) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_getdata",
		"request code 0x%04x getting data error 0x%04x",
			ptp->Code, ret);
	}
	return ret;
}

uint16_t
ptp_usb_getresp (PTPParams* params, PTPContainer* resp)
{
	uint16_t 		ret;
	unsigned long		rlen;
	PTPUSBBulkContainer	usbresp;
	/*GPContext		*context = ((PTPData *)params->data)->context;*/

	gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getresp", "reading response");
	PTP_CNT_INIT(usbresp);
	/* read response, it should never be longer than sizeof(usbresp) */
	ret = ptp_usb_getpacket(params, &usbresp, &rlen);

	if (ret!=PTP_RC_OK) {
		ret = PTP_ERROR_IO;
	} else
	if (dtoh16(usbresp.type)!=PTP_USB_CONTAINER_RESPONSE) {
		ret = PTP_ERROR_RESP_EXPECTED;
	} else
	if (dtoh16(usbresp.code)!=resp->Code) {
		ret = dtoh16(usbresp.code);
	}
	if (ret!=PTP_RC_OK) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_getresp","request code 0x%04x getting resp error 0x%04x", resp->Code, ret);
		return ret;
	}
	/* build an appropriate PTPContainer */
	resp->Code=dtoh16(usbresp.code);
	resp->SessionID=params->session_id;
	resp->Transaction_ID=dtoh32(usbresp.trans_id);
	if (resp->Transaction_ID != params->transaction_id - 1) {
		if (MTP_ZEN_BROKEN_HEADER(params)) {
			gp_log (GP_LOG_DEBUG, "ptp2/ptp_usb_getresp", "Read broken PTP header (transid is %08x vs %08x), compensating.",
				resp->Transaction_ID, params->transaction_id - 1
			);
			resp->Transaction_ID=params->transaction_id-1;
		}
		/* else will be handled by ptp.c as error. */
	}
	resp->Nparam=(rlen-12)/4;
	resp->Param1=dtoh32(usbresp.payload.params.param1);
	resp->Param2=dtoh32(usbresp.payload.params.param2);
	resp->Param3=dtoh32(usbresp.payload.params.param3);
	resp->Param4=dtoh32(usbresp.payload.params.param4);
	resp->Param5=dtoh32(usbresp.payload.params.param5);
	return ret;
}

/* Event handling functions */

/* PTP Events wait for or check mode */
#define PTP_EVENT_CHECK			0x0000	/* waits for */
#define PTP_EVENT_CHECK_FAST		0x0001	/* checks */

static inline uint16_t
ptp_usb_event (PTPParams* params, PTPContainer* event, int wait)
{
	int			result, timeout;
	unsigned long		rlen;
	PTPUSBEventContainer	usbevent;
	Camera			*camera = ((PTPData *)params->data)->camera;

	PTP_CNT_INIT(usbevent);

	if (event==NULL)
		return PTP_ERROR_BADPARAM;

	switch(wait) {
	case PTP_EVENT_CHECK:
		result = gp_port_check_int (camera->port, (char*)&usbevent, sizeof(usbevent));
		if (result <= 0) result = gp_port_check_int (camera->port, (char*)&usbevent, sizeof(usbevent));
		break;
	case PTP_EVENT_CHECK_FAST:
		gp_port_get_timeout (camera->port, &timeout);
		gp_port_set_timeout (camera->port, PTP2_FAST_TIMEOUT);
		result = gp_port_check_int (camera->port, (char*)&usbevent, sizeof(usbevent));
		if (result <= 0) result = gp_port_check_int (camera->port, (char*)&usbevent, sizeof(usbevent));
		gp_port_set_timeout (camera->port, timeout);
		break;
	default:
		return PTP_ERROR_BADPARAM;
	}
	if (result < 0) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_event", "reading event an error %d occurred", result);
		if (result == GP_ERROR_TIMEOUT)
			return PTP_ERROR_TIMEOUT;
		return PTP_ERROR_IO;
	}
	if (result == 0) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_event", "reading event a 0 read occurred, reporting timeout.");
		return PTP_ERROR_TIMEOUT;
	}
	rlen = result;
	if (rlen < 8) {
		gp_log (GP_LOG_ERROR, "ptp2/usb_event",
			"reading event an short read of %ld bytes occurred", rlen);
		return PTP_ERROR_IO;
	}

	/* Only do the additional reads for "events". Canon IXUS 2 likes to
	 * send unrelated data.
	 */
	if (	(dtoh16(usbevent.type) == PTP_USB_CONTAINER_EVENT) &&
		(dtoh32(usbevent.length) > rlen)
	) {
		gp_log (GP_LOG_DEBUG, "ptp2/usb_event","Canon incremental read (done: %ld, todo: %d)", rlen, dtoh32(usbevent.length));
		gp_port_get_timeout (camera->port, &timeout);
		gp_port_set_timeout (camera->port, PTP2_FAST_TIMEOUT);
		while (dtoh32(usbevent.length) > rlen) {
			result = gp_port_check_int (camera->port, ((char*)&usbevent)+rlen, sizeof(usbevent)-rlen);
			if (result <= 0)
				break;
			rlen += result;
		}
		gp_port_set_timeout (camera->port, timeout);
	}
	/* if we read anything over interrupt endpoint it must be an event */
	/* build an appropriate PTPContainer */
	event->Code=dtoh16(usbevent.code);
	event->SessionID=params->session_id;
	event->Transaction_ID=dtoh32(usbevent.trans_id);
	event->Param1=dtoh32(usbevent.param1);
	event->Param2=dtoh32(usbevent.param2);
	event->Param3=dtoh32(usbevent.param3);
	return PTP_RC_OK;
}

uint16_t
ptp_usb_event_check (PTPParams* params, PTPContainer* event) {

	return ptp_usb_event (params, event, PTP_EVENT_CHECK_FAST);
}

uint16_t
ptp_usb_event_wait (PTPParams* params, PTPContainer* event) {

	return ptp_usb_event (params, event, PTP_EVENT_CHECK);
}

uint16_t
ptp_usb_control_get_extended_event_data (PTPParams *params, char *buffer, int *size) {
	Camera	*camera = ((PTPData *)params->data)->camera;
	int	ret;

	gp_log (GP_LOG_DEBUG, "ptp2/get_extended_event_data", "get event data");
	ret = gp_port_usb_msg_class_read (camera->port, 0x65, 0x0000, 0x0000, buffer, *size);
	if (ret < GP_OK)
		return PTP_ERROR_IO;
	*size = ret;
	gp_log_data ("ptp2/get_extended_event_data", buffer, ret);
	return PTP_RC_OK;
}

uint16_t
ptp_usb_control_device_reset_request (PTPParams *params) {
	Camera	*camera = ((PTPData *)params->data)->camera;
	int	ret;

	gp_log (GP_LOG_DEBUG, "ptp2/device_reset_request", "sending reset");
	ret = gp_port_usb_msg_class_write (camera->port, 0x66, 0x0000, 0x0000, NULL, 0);
	if (ret < GP_OK)
		return PTP_ERROR_IO;
	return PTP_RC_OK;
}

uint16_t
ptp_usb_control_get_device_status (PTPParams *params, char *buffer, int *size) {
	Camera	*camera = ((PTPData *)params->data)->camera;
	int	ret;

	ret = gp_port_usb_msg_class_read (camera->port, 0x67, 0x0000, 0x0000, buffer, *size);
	if (ret < GP_OK)
		return PTP_ERROR_IO;
	gp_log_data ("ptp2/get_device_status", buffer, ret);
	*size = ret;
	return PTP_RC_OK;
}

uint16_t
ptp_usb_control_cancel_request (PTPParams *params, uint32_t transactionid) {
	Camera	*camera = ((PTPData *)params->data)->camera;
	int	ret;
	unsigned char	buffer[6];

	htod16a(&buffer[0],PTP_EC_CancelTransaction);
	htod32a(&buffer[2],transactionid);
	ret = gp_port_usb_msg_class_write (camera->port, 0x64, 0x0000, 0x0000, (char*)buffer, sizeof (buffer));
	if (ret < GP_OK)
		return PTP_ERROR_IO;
	return PTP_RC_OK;
}
