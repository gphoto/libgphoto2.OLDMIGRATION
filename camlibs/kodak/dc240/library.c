/*
  Kodak DC 240/280/3400/5000 driver.
  Maintainer:
       Hubert Figuiere <hfiguiere@teaser.fr>

  $Id$
 */



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gphoto2.h>
#include <gphoto2-port.h>

#include "dc240.h"
#include "library.h"

#define GP_MODULE "dc240"

static char *dc240_packet_new   (int command_byte);
static int   dc240_packet_write (Camera *camera, char *packet, int size,
				 int read_response);
static int   dc240_packet_read  (Camera *camera, char *packet, int size);



static char *dc240_packet_new (int command_byte) {

    char *p = (char *)malloc(sizeof(char) * 8);

    memset (p, 0, 8);

    p[0] = command_byte;
    p[7] = 0x1a;

    return p;
}

static char *dc240_packet_new_path (const char *folder, const char *filename) {

    char *p;
    char buf[1024];
    int x;
    unsigned char cs = 0;

    p = (char *)malloc(sizeof(char)*60);
    if (!p)
        return NULL;

    strcpy(buf, folder);

    if (buf[strlen(buf)-1] != '/')
        strcat(buf, "/");

    if (filename)
        strcat(buf, filename);
    else
        strcat(buf, "*.*");

    for (x=0; x<strlen(buf); x++) {
        buf[x] = (buf[x] == '/'? '\\' : buf[x]);
        cs = cs ^ (unsigned char)buf[x];
    }

    memset(p, 0, sizeof(char)*60);
    p[0] = 0x80;
    memcpy(&p[1], buf, sizeof(char)*strlen(buf));
    p[59] = cs;

    return (p);
}


static int dc240_packet_write (Camera *camera, char *packet, int size, int read_response) {

    /* Writes the packet and returns the result */

    int x=0;
    char in[2];

write_again:
    /* If retry, give camera some recup time */
    if (x > 0)
        GP_SYSTEM_SLEEP(SLEEP_TIMEOUT);

    /* Return error if too many retries */
    if (x++ > RETRIES)
        return (GP_ERROR);

    if (gp_port_write(camera->port, packet, size) < 0)
        goto write_again;

    /* Read in the response from the camera if requested */
    if (read_response) {
        if (gp_port_read(camera->port, in, 1) < 0)
            /* On error, write again */
            goto write_again;
    }

    return (GP_OK);
}

static int dc240_packet_read (Camera *camera, char *packet, int size) {

    return (gp_port_read(camera->port, packet, size));
}

static int dc240_packet_write_ack (Camera *camera) {

    char p[2];

    p[0] = PACK1;
    if (gp_port_write(camera->port, p, 1) < 0)
        return GP_ERROR;
    return GP_OK;

}

static int dc240_packet_write_nak (Camera *camera) {

    char p[2];

    p[0] = PACK0;
    if (gp_port_write(camera->port, p, 1) < 0)
        return GP_ERROR;
    return GP_OK;
}

static int dc240_wait_for_completion (Camera *camera) {

    char p[8];
    int retval;
    int x=0, done=0;

    /* Wait for command completion */
    while ((x++ < 25)&&(!done)) {
            retval = dc240_packet_read(camera, p, 1);
            switch (retval) {
               case GP_ERROR: 
                    return (GP_ERROR); 
                    break;
               case GP_ERROR_IO_TIMEOUT:
               case GP_ERROR_TIMEOUT: 
                    break;
               default:
                    done = 1;
            }
            gp_camera_progress(camera, 0.0);
    }

    if (x==25)
            return (GP_ERROR);

//    GP_PORT_SLEEP(500);
    return (GP_OK);

}

/*
  Implement the wait state aka busy case
  See 2.7.2 in the DC240 Host interface Specification
 */
static int dc240_wait_for_busy_completion (Camera *camera) 
{
    enum {
	BUSY_RETRIES = 100
    };
    char p[8];
    int retval;
    int x=0, done=0;

    /* Wait for command completion */
    while ((x++ < BUSY_RETRIES)&&(!done)) {
	retval = dc240_packet_read(camera, p, 1);
	switch (retval) {
	case GP_ERROR:
	    return retval; 
	    break;
	case GP_ERROR_IO_TIMEOUT:
	case GP_ERROR_IO_READ:
	case GP_ERROR_TIMEOUT: 
	    /* in busy state, GP_ERROR_IO_READ can happend */
	    break;
	default:
	    if (*p != (char)0xf0) {
		done = 1;
	    }
	}
	gp_camera_progress(camera, 0.0);
    }
    
    if (x == BUSY_RETRIES)
	return (GP_ERROR);
    
    return GP_OK;
}


static int dc240_packet_exchange (Camera *camera, CameraFile *file,
                           char *cmd_packet, char *path_packet,
                           int *size, int block_size) {

    /* Reads in multi-packet data, appending it to the "file". */
    char check_sum;
    int i;
    int num_packets=1, num_bytes, retval;
    int x=0, retries=0;
    float t;
    char packet[HPBS+2];

    if (*size > 0) {
        /* Known size to begin with */
        t = (float)*size / (float)(block_size);
        num_packets = (int)t;
        if (t - (float)num_packets > 0)
            num_packets++;
    } else {
        /* Calculate size in 1st packet */
        num_packets = 2;
    }

read_data_write_again:
    /* Write command/path packets */
    if (cmd_packet) 
        if (dc240_packet_write(camera, cmd_packet, 8, 1) < 0)
            return (GP_ERROR);

    if (path_packet)
        if (dc240_packet_write(camera, path_packet, 60, 1) < 0)
            return (GP_ERROR);

    while (x < num_packets) {
read_data_read_again:
        gp_camera_progress(camera, (float)x/(float)num_packets);

        /* Read the response/data */
        retval = dc240_packet_read(camera, packet, block_size+2);
        if ((retval ==  GP_ERROR) || (retval == GP_ERROR_TIMEOUT) ||
          (retval == GP_ERROR_IO_TIMEOUT) ) {
            /* ERROR reading response/data */
            if (retries++ > RETRIES)
                return (GP_ERROR);

            if (x==0) {
                /* If first packet didn't come, write command again */
                goto read_data_write_again;
            } else {
                /* Didn't get data packet. Send NAK for retry */
                dc240_packet_write_nak(camera);
                goto read_data_read_again;
            }
        }

	/* Validate checksum */
	check_sum = 0;
	for (i = 1; i < block_size + 1; i++) {
	    check_sum ^= packet [i];
	}
	if ( block_size > 1 && check_sum != packet [i]) {
	    dc240_packet_write_nak (camera);
	    goto read_data_read_again;
	}
	
        /* Check for error in command/path */
        if ((unsigned char)packet[0] > 0xe0)
            return GP_ERROR;

        /* Check for end of data */
        if ((unsigned char)packet[0] == 0x00)
            return GP_OK;

        /* Write packet was OK */
        if (dc240_packet_write_ack(camera)== GP_ERROR)
            goto read_data_read_again;

        /* Set size for folder/file list command from 1st packet */
        if (((unsigned char)cmd_packet[0]==0x99)&&(x==0)) {
            *size = ((unsigned char)packet[1] * 256 +
                     (unsigned char)packet[2])* 20 + 2;
            t = (float)*size / (float)(block_size);
            num_packets = (int)t;
            if (t - (float)num_packets > 0)
                num_packets++;
        }

        /* Copy in data */
        if (x == num_packets)
            num_bytes = *size - ((num_packets-1) * block_size);
        else
            num_bytes = block_size;
        gp_file_append(file, &packet[1], num_bytes);

        /* Increment packet count, reset retries */
        x++;
        retries = 0;
    }

    /* Read in command completed */
    dc240_wait_for_completion(camera);

    return (GP_OK);
}


int dc240_open (Camera *camera) {

    char *p = dc240_packet_new(0x96);

    if (dc240_packet_write(camera, p, 8, 1) == GP_ERROR) {
        free(p);
        return (GP_ERROR);
    }

    if (dc240_wait_for_completion(camera) == GP_ERROR) {
        free(p);
        return (GP_ERROR);
    }


    free(p);
    return (GP_OK);
}

int dc240_close (Camera *camera) {

    char *p = dc240_packet_new(0x97);
    int retval, size = -1;

    retval = dc240_packet_exchange(camera, NULL, p, NULL, &size, -1);

    free(p);
    return (retval);
}

static int dc240_get_file_size (Camera *camera, const char *folder, const char *filename, int thumb) {

    CameraFile *f;
    char *p1, *p2;
    int size = 256, offset;
    long int fsize;
    const char *fdata;

    offset = (thumb? 92:104);

    gp_file_new(&f);
    p1 = dc240_packet_new(0x91);
    p2 = dc240_packet_new_path(folder, filename);
    if (dc240_packet_exchange(camera, f, p1, p2, &size, 256) < 0)
        size = 0;
    else {
	gp_file_get_data_and_size (f, &fdata, &fsize);
        size = ((unsigned char)fdata[offset]   << 24) |
               ((unsigned char)fdata[offset+1] << 16) |
               ((unsigned char)fdata[offset+2] << 8 ) |
               ((unsigned char)fdata[offset+3]);
    }

    gp_file_free(f);
    free(p1);
    free(p2);

    return (size);
}

int dc240_set_speed (Camera *camera, int speed) {

    char *p = dc240_packet_new(0x41);
    gp_port_settings settings;

    gp_port_settings_get(camera->port, &settings);

    switch (speed) {
    case 9600:
        p[2] = (unsigned char)0x96;
        p[3] = (unsigned char)0x00;
        settings.serial.speed = 9600;
        break;
    case 19200:
        p[2] = (unsigned char)0x19;
        p[3] = (unsigned char)0x20;
        settings.serial.speed = 19200;
        break;
    case 38400:
        p[2] = (unsigned char)0x38;
        p[3] = (unsigned char)0x40;
        settings.serial.speed = 38400;
        break;
    case 57600:
        p[2] = (unsigned char)0x57;
        p[3] = (unsigned char)0x60;
        settings.serial.speed = 57600;
        break;
    case 0: /* Default */
    case 115200:
        p[2] = (unsigned char)0x11;
        p[3] = (unsigned char)0x52;
        settings.serial.speed = 115200;
        break;
    /* how well supported is 230.4?
    case 230400:
         p[2] = (unsigned char)0x23;
         p[3] = (unsigned char)0x04;
         settings.serial.speed = 230400;
         break;
    */
    default:
        return (GP_ERROR);
    }

    if (dc240_packet_write(camera, p, 8, 1) == GP_ERROR)
        return (GP_ERROR);

    if (gp_port_settings_set (camera->port, settings) == GP_ERROR)
        return (GP_ERROR);

    free (p);

    GP_SYSTEM_SLEEP(300);
    if (dc240_wait_for_completion(camera) == GP_ERROR)
        return (GP_ERROR);

    /* Speed change went OK. */
    return (GP_OK);
}

#if 0
int dc240_get_status (Camera *camera) {

    CameraFile *file;
    char *p = dc240_packet_new(0x7F);
    int retval;
    int size = 256;

    gp_file_new (&file);

    retval = dc240_packet_exchange(camera, file, p, NULL, &size, 256);

    gp_file_free(file);
    free (p);

    return (retval);
}
#endif

static int dc240_get_directory_list (Camera *camera, CameraList *list, const char *folder,
                             unsigned char attrib) {

    CameraFile *file;
    int x, y=0, z, size=256;
    char buf[64];
    char *p1 = dc240_packet_new(0x99);
    char *p2 = dc240_packet_new_path(folder, NULL);
    const char *fdata;
    long int fsize;
    int ret;
    int num_of_entries = 0; /* number of entries in the listing */
    int total_size = 0; /* total useful size of the listing */

    gp_file_new(&file);
    ret = dc240_packet_exchange(camera, file, p1, p2, &size, 256);
    if (ret < 0) {
        return ret;
    }
    free(p1);
    free(p2);

    /* Don't expect to have a fully useful buffer. */
    gp_file_get_data_and_size (file, &fdata, &fsize);

    /* numbers in DC 240 are Big-Endian. */
    /* Conversion below should be endian neutral. */
    num_of_entries = (fdata [0] << 8) + fdata [1] + 1;
    total_size = 2 + (num_of_entries * 20);
    GP_DEBUG ("number of file entries : %d, size = %d", num_of_entries, fsize);
    for (x = 2; x < total_size; x += 20) {
        if ((fdata[x] != '.') && (attrib == (unsigned char)fdata[x+11]))  {
            /* Files have attrib 0x00, Folders have attrib 0x10 */
            if (attrib == 0x00) {
                strncpy(buf, &fdata[x], 8);    /* Copy over filename */
                buf[8] = 0;                         /* NULL terminate */
                strcat(buf, ".");                   /* Append dot */
                strcat(buf, &fdata[x+8]);      /* Append extension */
		GP_DEBUG ("found file: %s", buf);
            } else {
                strncpy(buf, &fdata[x], 8);    /* Copy over folder name */
                z=0;
                while ((buf[z] != 0x20)&&(z<8))     /* Chop trailing spaces */
                    z++;
                buf[z] = 0;                         /* NULL terminate */
		GP_DEBUG ("found folder: %s", buf);
            }
            gp_list_append(list, buf, NULL);
            y++;
        }
    }

    gp_file_free(file);

    return (GP_OK);
}

int dc240_get_folders (Camera *camera, CameraList *list, const char *folder) {

    return (dc240_get_directory_list(camera, list, folder, 0x10));
}

int dc240_get_filenames (Camera *camera, CameraList *list, const char *folder) {

    return (dc240_get_directory_list(camera, list, folder, 0x00));
}

int dc240_file_action (Camera *camera, int action, CameraFile *file,
                       const char *folder, const char *filename) {

    int size=0, thumb=0, retval=GP_OK;
    char *cmd_packet, *path_packet;

    cmd_packet  = dc240_packet_new(action);
    path_packet = dc240_packet_new_path(folder, filename);

    switch (action) {
    case DC240_ACTION_PREVIEW:
        cmd_packet[4] = 0x02;
        thumb = 1;
        /* no break on purpose */
    case DC240_ACTION_IMAGE:
        if ((size = dc240_get_file_size(camera, folder, filename, thumb)) < 0) {
            retval = GP_ERROR;
            break;
        }
        retval = dc240_packet_exchange(camera, file, cmd_packet, path_packet, &size, HPBS);
        break;
    case DC240_ACTION_DELETE:
        size = -1;
        retval = dc240_packet_exchange(camera, file, cmd_packet, path_packet, &size, -1);
        break;
    default:
        return (GP_ERROR);
    }

    free(cmd_packet);
    free(path_packet);

    if ((filename) && (file)) {
	gp_file_set_name (file, filename);
	gp_file_set_mime_type (file, "image/jpeg");
    }

    return (retval);
}

/*
  See 5.1.27 in the DC240 Host Spec.
  Capture a picture on the flash card and retrieve its full
  name.
  See also 5.1.19
 */
int dc240_capture (Camera *camera, CameraFilePath *path) 
{
    CameraFile *file;
    int size = 256;
    int ret = GP_OK;
    char *p = dc240_packet_new(0x7C);
    const char *fdata;
    long int fsize;

    /* Take the picture to Flash memory */
    ret = dc240_packet_write(camera, p, 8, 1);
    free (p); 
    if (ret != GP_OK) {
	return ret;
    }

    gp_camera_status (camera, "Waiting for completion...");
    ret = dc240_wait_for_completion(camera);
    if (ret != GP_OK) {
	return ret;
    }
    
    ret = dc240_wait_for_busy_completion(camera);
    if (ret != GP_OK) {
	return ret;
    }
 
    fprintf (stderr, " dc240_wait_() end \n");
    /*
     get the last picture taken location (good firmware developers)
     */
    gp_file_new(&file);
    p = dc240_packet_new(0x4C); /* 5.1.19 Send last taken image name */
    ret = dc240_packet_exchange(camera, file, p, NULL, &size, 256);
    free(p);
    if (ret != GP_OK) {
	path->name[0] = 0;
	path->folder[0] = 0;
	free (file);
	return ret;
    }
    else {
	/* this part assumes that the response is of fixed size */
	/* according to the specs, this is the case since only the */
	/* numbering changes */
	gp_file_get_data_and_size (file, &fdata, &fsize);
	strncpy (path->folder, fdata, 14);
	path->folder [14] = 0;
	path->folder [0] = '/';
	path->folder [5] = '/';
	strncpy (path->name, &(fdata[15]), 13);
	path->name[13] = 0;
    }
    free (file);

    return GP_OK;
}

int dc240_packet_set_size (Camera *camera, short int size) {

    char *p = dc240_packet_new(0x2A);

    p[2] = (size >> 8) & 0xFF;
    p[3] = (size     ) & 0xFF;

    if (dc240_packet_write(camera, p, 8, 1) == GP_ERROR)
            return (GP_ERROR);

    if (dc240_wait_for_completion(camera)==GP_ERROR)
            return (GP_ERROR);

    free(p);

    return (GP_OK);
}
