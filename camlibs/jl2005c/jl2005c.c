/* jl2005c.c
 *
 * Copyright (C) 2006 Theodore Kilgore <kilgota@auburn.edu>
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

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>

#include <gphoto2/gphoto2.h>
#include <gphoto2/gphoto2-port.h>
#include "gphoto2-endian.h"

#include "jl2005c.h"

#define GP_MODULE "jl2005c" 

int 
jl2005c_init (Camera *camera, GPPort *port, CameraPrivateLibrary *priv) 
{
	unsigned char command[2];
	char response;
	char model_string[4];
	unsigned char info[0xe000];
	int info_block_size = 0;
	memset(info,0, sizeof(info)); 
	memset(command,0,sizeof(command));
	GP_DEBUG("Running jl2005c_init\n");
	set_usb_in_endpoint	(camera, 0x84); 
	gp_port_write (port, "\x08\x00", 2); 
	usleep (10000);
	gp_port_write (port, "\x95\x60", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	model_string[0]=response;
	usleep (10000);
	gp_port_write (port, "\x95\x61", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	model_string[1]=response;
	usleep (10000);
	gp_port_write (port, "\x95\x62", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	model_string[2]=response;
	usleep (10000);
	gp_port_write (port,"\x95\x63" , 2); 
	usleep (10000);
gp_port_read (port, &response, 1);
	model_string[3]=response;
	GP_DEBUG("Model string is %02x%02x%02x%02x\n",model_string[0], model_string[1],
			    model_string[2], model_string[3]);
	usleep (10000);
	gp_port_write (port, "\x95\x64", 2); 
	usleep (10000);	
	gp_port_read (port, &response, 1);

	usleep (10000);	
	gp_port_write (port, "\x95\x65", 2);
	usleep (10000);
	gp_port_read (port, &response, 1); /* Number of pix returned here */
	priv->nb_entries = (unsigned)response;
	GP_DEBUG("%d entries in the camera\n", response);
	GP_DEBUG("%d entries in the camera\n", priv->nb_entries);
	info_block_size = ((unsigned)response * 0x10) + 2;
	if (info_block_size%0x200) 
		info_block_size += 0x200 - (info_block_size%0x200);
	usleep (10000);

	gp_port_write (port, "\x95\x66", 2);
	usleep (10000);
	gp_port_read (port, &response, 1); 
	usleep (10000);

	gp_port_write (port, "\x95\x67", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x68", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x69", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x6a", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x6b", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x6c", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);

	priv->data_to_read = (response &0xff)*0x100;

	gp_port_write (port, "\x95\x6d", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	priv->data_to_read += (response&0xff);
	priv->data_to_read *= 0x200;
	priv->total_data_in_camera = priv->data_to_read;
	GP_DEBUG ("data_to_read = 0x%lx = %lu\n", priv->data_to_read, 
							priv->data_to_read);
	GP_DEBUG ("total_data_in_camera = 0x%lx = %lu\n", priv->data_to_read, 
							priv->data_to_read);

	gp_port_write (port, "\x95\x6e", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);

	usleep (10000);
	gp_port_write (port, "\x95\x6f", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);

	usleep (10000);
	gp_port_write (port, "\x0a\x00", 2);

	usleep (10000);
	/* Switch the inep over to 0x82. It stays there ever after. */ 
	set_usb_in_endpoint	(camera, 0x82); 
	usleep (10000);
	gp_port_read(port, (char *)info, info_block_size);
	usleep (10000);
	gp_port_write (port, "\x0b\x00",2);
	usleep (10000);
	memmove(priv->info, info, info_block_size);
	priv->model=info[6];

	GP_DEBUG("Leaving jl2005c_init\n");
        return GP_OK;
}


int 
jl2005c_rewind (Camera *camera, GPPort *port) 
{
//	gp_port_write (port, "\x0b\x00",2);
	unsigned char command[2];
	char response;
	unsigned char info[0xe000];
	int info_block_size = 0;
	int junk_to_read = 0;
	memset(info,0, sizeof(info)); 
	memset(command,0,sizeof(command));
	GP_DEBUG("Running jl2005c_rewind\n");
	gp_port_close(port);
	usleep (100000);
	gp_port_open(port);

	set_usb_in_endpoint	(camera, 0x84); 
	gp_port_write (port, "\x08\x00", 2); 

	usleep (10000);
	gp_port_write (port, "\x95\x60", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);
	gp_port_write (port, "\x95\x61", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);
	gp_port_write (port, "\x95\x62", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);
	gp_port_write (port,"\x95\x63" , 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);
	gp_port_write (port, "\x95\x64", 2); 
	usleep (10000);	
	gp_port_read (port, &response, 1);

	usleep (10000);	
	gp_port_write (port, "\x95\x65", 2);
	usleep (10000);
	gp_port_read (port, &response, 1); 
	info_block_size = ((unsigned)response * 0x10) + 2;
	if (info_block_size%0x200) 
		info_block_size += 0x200 - (info_block_size%0x200);
	usleep (10000);

	gp_port_write (port, "\x95\x66", 2);
	usleep (10000);
	gp_port_read (port, &response, 1); 
	usleep (10000);

	gp_port_write (port, "\x95\x67", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x68", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x69", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x6a", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x6b", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	gp_port_write (port, "\x95\x6c", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);

	junk_to_read = (response &0xff)*0x100;

	gp_port_write (port, "\x95\x6d", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);

	junk_to_read += (response&0xff);
	junk_to_read *= 0x200;
	gp_port_write (port, "\x95\x6e", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);
	gp_port_write (port, "\x95\x6f", 2); 
	usleep (10000);
	gp_port_read (port, &response, 1);
	usleep (10000);
	gp_port_write (port, "\x0a\x00", 2);

	usleep (10000);

	/* Switch the inep over to 0x82. It stays there ever after. */ 
	set_usb_in_endpoint	(camera, 0x82); 
	usleep (10000);
	gp_port_read(port, (char *)info, info_block_size);

	usleep (10000);
	gp_port_write (port, "\x0b\x00",2);
	usleep (10000);

	GP_DEBUG("Completing jl2005c_rewind\n");

        return GP_OK;
}


int
jl2005c_get_pic_data_size (CameraPrivateLibrary *priv, Info *info, int n)
{
	int size;
	unsigned char model=priv->model;
	GP_DEBUG("info[48+16*n+7] = %02X\n", info[48+16*n+7]);
	size = info[0x30+0x10*n+6]*0x100+info[0x30+0x10*n+7];
	switch (model) {
	case 0x43:
	case 0x44:
		size *= 0x200;
		break;
	case 0x42:
		size *= 0x80;
		break;
	default:
		GP_DEBUG("Unknown model, unknown size\n");
		return GP_ERROR_NOT_SUPPORTED;
	}
	GP_DEBUG("size = 0x%x = %d\n", size, size);
	return (size);
}

unsigned long
jl2005c_get_start_of_photo(CameraPrivateLibrary *priv, Info *info, 
							unsigned int n)
{
	unsigned long start;
	unsigned char model = priv->model;
	start = info[0x30+0x10*n+0x0c]*0x100+
			info[0x30+0x10*n+0x0d];
	start -= info[0x30+0x0c]*0x100+
			info[0x30+0x0d];
	switch (model) {
	case 0x43:
	case 0x44:
		start *= 0x200;
		break;
	case 0x42:
		start *= 0x80;
		break;
	default:
		GP_DEBUG("Unknown model\n");
		return GP_ERROR_NOT_SUPPORTED;
	}
	return start;
}


int 
set_usb_in_endpoint	(Camera *camera, int inep) 
{
	GPPortSettings settings;
	gp_port_get_settings ( camera ->port, &settings);
	if(settings.usb.inep!=inep)
		settings.usb.inep = inep;
	GP_DEBUG("inep reset to %02X\n", inep);
	return gp_port_set_settings ( camera->port, settings);
}	


int 
jl2005c_get_picture_data (GPPort *port, 
					char *data, int size) 

{
	/* inep has been reset to 0x82 already and does not get set back  */
	/* We have to send 0b 00, presumably to access the data register, 
	 * when starting to download the first photo only 
	 */
	usleep (10000);
	/*Data transfer begins*/
	gp_port_read (port, data, size); 
	usleep (10000);
	return GP_OK;
} 

int
jl2005c_reset (Camera *camera, GPPort *port)
{
	int blocksize = 0xfa00;
	/* These cameras want all data to be dumped. If that is not yet done,
	 * then do it now, before exiting! */
	while (camera->pl->bytes_read_from_camera < 
				    camera->pl->total_data_in_camera ) {
		if (! camera->pl->data_cache )
			camera->pl->data_cache = malloc (0xfa00);
		blocksize=0xfa00;
		if (camera->pl->bytes_read_from_camera +0xfa00 >=
				    camera->pl->total_data_in_camera ) 
			blocksize = camera->pl->total_data_in_camera -
					camera->pl->bytes_read_from_camera;
		if(blocksize) 
			jl2005c_get_picture_data (
					    camera->port, 
					    (char *) camera->pl->data_cache, 
					    blocksize);
		camera->pl->bytes_read_from_camera 
						+= blocksize;
		
	}


	gp_port_write(port, "\x07\x00", 2);
	return GP_OK;
}

