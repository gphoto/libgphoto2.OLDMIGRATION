/* check-mtp-device.c udev hookup
 *
 * Copyright � 2006  Marcus Meissner  <marcus@jet.franken.de>
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

/*
 * UDEV snippet:
 * ACTION=="add", SUBSYSTEM=="usb", PROGRAM="/usr/sbin/check_ptp_camera", ... stuff ...
 * you can also move it to /lib/udev/ and call without path.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/param.h>
#include <string.h>

#include <usb.h>

/*
 * the USB event we trigger on:
UDEV  [1166213739.401000] add@/class/usb_device/usbdev1.6
UDEV_LOG=3
ACTION=add
DEVPATH=/class/usb_device/usbdev1.6
SUBSYSTEM=usb_device
SEQNUM=1634
PHYSDEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-3
PHYSDEVBUS=usb
PHYSDEVDRIVER=usb
MAJOR=189
MINOR=5
UDEVD_EVENT=1
DEVNAME=/dev/bus/usb/001/006
*/

/* This function reads the Microsoft OS Descriptor and looks inside to
 * find if it is a MTP device. This is the similar to the way that
 * Windows Media Player 10 uses.
 * It is documented to some degree on various internet pages.
 */
int
main(int argc, char **arvg)
{
	char buf[1000], cmd, *s;
	int ret;
	usb_dev_handle *devh;
	struct usb_device *dev;
	struct usb_bus *bus;
	char	*devpath;
	unsigned int xbus, xdev;
	int i;

	devpath = getenv("DEVPATH");
	if (!devpath) {
		fprintf(stderr,"DEVPATH not set\n");
		goto errout2;
	}

	s = strstr(devpath,"usbdev");
	if (!s) goto errout2;
	s+=strlen("usbdev");

	if (2!=sscanf(s,"%u.%u",&xbus,&xdev)) {
		fprintf (stderr,"Failed to parse %s, xbus %d, xdev %d\n", devpath, xbus, xdev);
		goto errout2;
	}
	usb_init();
	usb_find_busses ();
	usb_find_devices ();

	for (bus = usb_busses; bus; bus = bus->next) {
		int busno = strtol(bus->dirname,NULL,10);
		if (busno != xbus) continue;
		for (dev = bus->devices; dev; dev = dev->next) {
			char *s = strchr(dev->filename,'/');
			int devno;

			if (!s) s = dev->filename;
			devno = strtol(s,NULL,10);
			if (devno != xdev) continue;
			goto found;
		}
	}
	fprintf(stderr,"Device not found.\n");
	goto errout2;

found:
	fprintf (stderr,"dev is %s\n", dev->filename);
	/* All of them are "vendor specific" device class, or interface defined. */
	if ((dev->descriptor.bDeviceClass!=0xff) && (dev->descriptor.bDeviceClass!=0)) {
		fprintf(stderr,"Non MTP deviceclass?\n");
		goto errout2;
	}

	devh = usb_open (dev);

	/*
	 * Loop over the device configurations and interfaces. Nokia MTP-capable 
	 * handsets (possibly others) typically have the string "MTP" in their 
	 * MTP interface descriptions, that's how they can be detected, before
	 * we try the more esoteric "OS descriptors" (below).
	 */
	if (dev->config) {
		for (i = 0; i < dev->descriptor.bNumConfigurations; i++) {
			unsigned int j;
			for (j = 0; j < dev->config[i].bNumInterfaces; j++) {
				int k;
				for (k = 0; k < dev->config[i].interface[j].num_altsetting; k++) {
					buf[0] = '\0';
					ret = usb_get_string_simple(devh, 
						dev->config[i].interface[j].altsetting[k].iInterface, 
						(char *) buf, 
						1024);
					if (ret < 3)
						continue;
					if (strcmp((char *) buf, "MTP") == 0) {
						fprintf(stderr, "Configuration %d, interface %d, altsetting %d:\n", i, j, k);
						fprintf(stderr, "   Interface description contains the string \"MTP\"\n");
						fprintf(stderr, "   Device recognized as MTP, no further probing.\n");
						usb_close(devh);
						return 0;
					}
				}
			}
		}
	}
	/* get string descriptor at 0xEE */
	ret = usb_get_descriptor (devh, 0x03, 0xee, buf, sizeof(buf));
	/*if (ret > 0) gp_log_data("get_MS_OSD",buf, ret);*/
	if (ret < 10) goto errout;
	if (!((buf[2] == 'M') && (buf[4]=='S') && (buf[6]=='F') && (buf[8]=='T'))) {
		fprintf(stderr,"MS OS Descriptor request failed.\n");
		goto errout;
	}
	cmd = buf[16];
	ret = usb_control_msg (devh, USB_ENDPOINT_IN|USB_RECIP_DEVICE|USB_TYPE_VENDOR, cmd, 0, 4, buf, sizeof(buf), 1000);
	if (ret == -1) {
		fprintf (stderr, "mtp matcher: control message says %d\n", ret);
		goto errout;
	}
	if (buf[0] != 0x28) {
		fprintf (stderr, "mtp matcher: ret is %d, buf[0] is %x\n", ret, buf[0]);
		goto errout;
	}
	/*if (ret > 0) gp_log_data("get_MS_ExtDesc",buf, ret);*/
	if ((buf[0x12] != 'M') || (buf[0x13] != 'T') || (buf[0x14] != 'P')) {
		fprintf (stderr, "mtp matcher: buf at 0x12 is %02x%02x%02x\n", buf[0x12], buf[0x13], buf[0x14]);
		goto errout;
	}
	ret = usb_control_msg (devh, USB_ENDPOINT_IN|USB_RECIP_DEVICE|USB_TYPE_VENDOR, cmd, 0, 5, buf, sizeof(buf), 1000);
	if (ret == -1) goto errout;
	if (buf[0] != 0x28) {
		fprintf (stderr, "mtp matcher: ret is %d, buf[0] is %x\n", ret, buf[0]);
		goto errout;
	}
	/*if (ret > 0) gp_log_data("get_MS_ExtProp",buf, ret);*/
	if ((buf[0x12] != 'M') || (buf[0x13] != 'T') || (buf[0x14] != 'P')) {
		fprintf (stderr, "mtp matcher: buf at 0x12 is %02x%02x%02x\n", buf[0x12], buf[0x13], buf[0x14]);
		goto errout;
	}
	usb_close (devh);
	fprintf(stderr,"MTP Device found!\n");
	return 0;
errout:
	usb_close (devh);
errout2:
	fprintf(stderr,"MTP Device not found!\n");
	return 1;
}
