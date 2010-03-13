/** \file
 *
 * \author Copyright 2001 Lutz M�ller <lutz@users.sf.net>
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
#include <gphoto2/gphoto2-port-log.h>

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include <gphoto2/gphoto2-port-result.h>

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
#endif

#ifndef DISABLE_DEBUGGING
/** 
 * \brief Internal logging function entry.
 * 
 * Internal structure to remember the logging functions.
 * Use gp_log_add_func() and gp_log_remove_func() to access it.
 */
typedef struct {
	unsigned int id;	/**< Internal id */
	GPLogLevel   level;	/**< Internal loglevel */
	GPLogFunc    func;	/**< Internal function pointer to call */
	void        *data;	/**< Private data supplied by caller */
} LogFunc;

static LogFunc *log_funcs = NULL;
static unsigned int log_funcs_count = 0;

/**
 * \brief Add a function to get logging information
 *
 * \param level the maximum level of logging it will get, up to and including the passed value
 * \param func a #GPLogFunc
 * \param data data
 *
 * Adds a log function that will be called for each log message that is flagged
 * with a log level that appears in given log level. This function returns
 * an id that you can use for removing the log function again (using
 * #gp_log_remove_func).
 *
 * \return an id or a gphoto2 error code
 **/
int
gp_log_add_func (GPLogLevel level, GPLogFunc func, void *data)
{
	LogFunc *new_log_funcs;

	if (!func)
		return (GP_ERROR_BAD_PARAMETERS);

	if (!log_funcs)
		new_log_funcs = malloc (sizeof (LogFunc));
	else
		new_log_funcs = realloc (log_funcs, sizeof (LogFunc) * 
					 (log_funcs_count + 1));
	if (!new_log_funcs)
		return (GP_ERROR_NO_MEMORY);

	log_funcs = new_log_funcs;
	log_funcs_count++;

	log_funcs[log_funcs_count - 1].id = log_funcs_count;
	log_funcs[log_funcs_count - 1].level = level;
	log_funcs[log_funcs_count - 1].func = func;
	log_funcs[log_funcs_count - 1].data = data;

	return (log_funcs_count);
}

/**
 * \brief Remove a logging receiving function
 * \param id an id (return value of #gp_log_add_func)
 *
 * Removes the log function with given id.
 *
 * \return a gphoto2 error code
 **/
int
gp_log_remove_func (int id)
{
	if (id < 1 || id > log_funcs_count)
		return (GP_ERROR_BAD_PARAMETERS);

	memmove (log_funcs + id - 1, log_funcs + id, log_funcs_count - id);
	log_funcs_count--;

	return (GP_OK);
}

/**
 * Width of offset field in characters. Note that HEXDUMP_COMPLETE_LINE 
 * needs to be changed when this value is changed.
 */
#define HEXDUMP_OFFSET_WIDTH 4

/**
 * Distance between offset, hexdump and ascii blocks. Note that
 * HEXDUMP_COMPLETE_LINE needs to be changed when this value is changed.
 */
#define HEXDUMP_BLOCK_DISTANCE 2

/** Initial value for x "pointer" (for hexdump) */
#define HEXDUMP_INIT_X (HEXDUMP_OFFSET_WIDTH + HEXDUMP_BLOCK_DISTANCE)

/** Initial value for y "pointer" (for ascii values) */
#define HEXDUMP_INIT_Y (HEXDUMP_INIT_X + 3 * 16 - 1 + HEXDUMP_BLOCK_DISTANCE)

/** Used to switch to next line */
#define HEXDUMP_LINE_WIDTH (HEXDUMP_INIT_Y + 16)

/** Used to put the '-' character in the middle of the hexdumps */
#define HEXDUMP_MIDDLE (HEXDUMP_INIT_X + 3 * 8 - 1)

/**
 * We are lazy and do our typing only once. Please note that you have
 * to add/remove some lines when increasing/decreasing the values of 
 * HEXDUMP_BLOCK_DISTANCE and/or HEXDUMP_OFFSET_WIDTH.
 */
#define HEXDUMP_COMPLETE_LINE {\
        curline[HEXDUMP_OFFSET_WIDTH - 4] = hexchars[(index >> 12) & 0xf]; \
        curline[HEXDUMP_OFFSET_WIDTH - 3] = hexchars[(index >>  8) & 0xf]; \
        curline[HEXDUMP_OFFSET_WIDTH - 2] = hexchars[(index >>  4) & 0xf]; \
        curline[HEXDUMP_OFFSET_WIDTH - 1] = '0'; \
        curline[HEXDUMP_OFFSET_WIDTH + 0] = ' '; \
        curline[HEXDUMP_OFFSET_WIDTH + 1] = ' '; \
        curline[HEXDUMP_MIDDLE] = '-'; \
        curline[HEXDUMP_INIT_Y-2] = ' '; \
        curline[HEXDUMP_INIT_Y-1] = ' '; \
        curline[HEXDUMP_LINE_WIDTH] = '\n'; \
        curline = curline + (HEXDUMP_LINE_WIDTH + 1);}

/**
 * \brief Log data
 * \brief domain the domain
 * \brief data the data to be logged
 * \brief size the size of the data
 *
 * Takes the data and creates a formatted hexdump string. If you would like
 * to log text messages, use #gp_log instead.
 **/
void
gp_log_data (const char *domain, const char *data, unsigned int size)
{
	static const char hexchars[16] = "0123456789abcdef";
	char *curline, *result;
	int x = HEXDUMP_INIT_X;
	int y = HEXDUMP_INIT_Y;
	int index;
	unsigned char value;

	if (!data) {
		gp_log (GP_LOG_DATA, domain, _("No hexdump (NULL buffer)"));
		return;
	}

	if (!size) {
		gp_log (GP_LOG_DATA, domain, _("Empty hexdump of empty buffer"));
		return;
	}

	if (size > 1024*1024) {
		/* Does not make sense for 200 MB movies */
		gp_log (GP_LOG_DATA, domain, _("Truncating dump from %d bytes to 1MB"), size);
		size = 1024*1024;
	}

	curline = result = malloc ((HEXDUMP_LINE_WIDTH+1)*(((size-1)/16)+1)+1);
	if (!result) {
		gp_log (GP_LOG_ERROR, "gphoto2-log", _("Malloc for %i bytes "
			"failed"), (HEXDUMP_LINE_WIDTH+1)*(((size-1)/16)+1)+1);
		return;
	}

	for (index = 0; index < size; ++index) {
                value = (unsigned char)data[index];
                curline[x] = hexchars[value >> 4];
                curline[x+1] = hexchars[value & 0xf];
                curline[x+2] = ' ';
                curline[y++] = ((value>=32)&&(value<127))?value:'.';
                x += 3;
                if ((index & 0xf) == 0xf) { /* end of line */
                        x = HEXDUMP_INIT_X;
                        y = HEXDUMP_INIT_Y;
                        HEXDUMP_COMPLETE_LINE;
                }
        }
        if ((index & 0xf) != 0) { /* not at end of line yet? */
                /* if so, complete this line */
                while (y < HEXDUMP_INIT_Y + 16) {
                        curline[x+0] = ' ';
                        curline[x+1] = ' ';
                        curline[x+2] = ' ';
                        curline[y++] = ' ';
                        x += 3;
                }
                HEXDUMP_COMPLETE_LINE;
        }
        curline[0] = '\0';

	gp_log (GP_LOG_DATA, domain, _("Hexdump of %i = 0x%x bytes follows:\n%s"),
		size, size, result);
	free (result);
}

#undef HEXDUMP_COMPLETE_LINE
#undef HEXDUMP_MIDDLE
#undef HEXDUMP_LINE_WIDTH
#undef HEXDUMP_INIT_Y
#undef HEXDUMP_INIT_X
#undef HEXDUMP_BLOCK_DISTANCE
#undef HEXDUMP_OFFSET_WIDTH

/**
 * \brief Log a debug or error message with va_list
 * \param level gphoto2 log level
 * \param domain the domain
 * \param format the format
 * \param args the va_list corresponding to format
 *
 * Logs a message at the given log level. You would normally use this
 * function to log as yet unformatted strings. 
 **/
void
gp_logv (GPLogLevel level, const char *domain, const char *format,
	 va_list args)
{
	int i;
#ifdef HAVE_VA_COPY
	va_list xargs;
#endif
	int strsize = 1000;
	char *str;
	int n;

	if (!log_funcs_count)
		return;

	str = malloc(strsize);
	if (!str) return;
#ifdef HAVE_VA_COPY
	va_copy (xargs, args);
#endif
	n = vsnprintf (str, strsize, format, xargs);
	if (n+1>strsize) {
		free (str);
		str = malloc(n+1);
		if (!str) return;
		strsize = n+1;
#ifdef HAVE_VA_COPY
		va_copy (xargs, args);
#endif
		n = vsnprintf (str, strsize, format, xargs);
	}
	for (i = 0; i < log_funcs_count; i++)
		if (log_funcs[i].level >= level)
			log_funcs[i].func (level, domain, str, log_funcs[i].data);
	free (str);
}

/**
 * \brief Log a debug or error message
 * \param level gphoto2 log level
 * \param domain the log domain
 * \param format a printf style format string
 * \param ... the variable argumentlist for above format string
 *
 * Logs a message at the given log level. You would normally use this
 * function to log general debug output in a printf way.
 **/
void
gp_log (GPLogLevel level, const char *domain, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	gp_logv (level, domain, format, args);
	va_end (args);
}

#else /* DISABLE_DEBUGGING */

/*
 * Even if debugging is disabled, we must keep stubs to these functions
 * around so that programs dynamically linked to this library will still run.
 */

/* Remove these macros so we can compile functions with these names */
#ifdef gp_log_add_func
#undef gp_log_add_func
#endif
#ifdef gp_log_remove_func
#undef gp_log_remove_func
#endif
#ifdef gp_log_data
#undef gp_log_data
#endif
#ifdef gp_logv
#undef gp_logv
#endif
#ifdef gp_log
#undef gp_log
#endif

int
gp_log_add_func (GPLogLevel level, GPLogFunc func, void *data)
{
	return 0;
}

int
gp_log_remove_func (int id)
{
	return 0;
}

void
gp_log_data (const char *domain, const char *data, unsigned int size)
{
}

void
gp_logv (GPLogLevel level, const char *domain, const char *format,
	 va_list args)
{
}

void
gp_log (GPLogLevel level, const char *domain, const char *format, ...)
{
}
#endif /* DISABLE_DEBUGGING */


#ifdef _GPHOTO2_INTERNAL_CODE

/** 
 * \brief (Internal) translate a enumeration code to a string
 */
const char *
gpi_enum_to_string(unsigned int _enum, 
		   const StringFlagItem *map)
{
	int i;
	for (i=0; map[i].str != NULL; i++) {
		if (_enum == map[i].flag) {
			return(map[i].str);
			break;
		}
	}
	return NULL;
}

/** 
 * \brief (Internal) translate a string to an enumeration code
 */
int
gpi_string_to_enum(const char *str,
		   unsigned int *result,
		   const StringFlagItem *map)
{
	int i;
	for (i=0; map[i].str != NULL; i++) {
		if (0==strcmp(map[i].str, str)) {
			(*result) = map[i].flag;
			return 0;
		}
	}
	return 1;
}

void
gpi_flags_to_string_list(unsigned int flags, 
			 const StringFlagItem *map,
			 string_item_func func, void *data)
{
	int i;
	for (i=0; map[i].str != NULL; i++) {
		if ((flags == 0) && (map[i].flag == 0)) {
			func(map[i].str, data);
			break;
		} else if (0 != (flags & map[i].flag)) {
			func(map[i].str, data);      
		}
	}
}

unsigned int 
gpi_string_to_flag(const char *str, 
	       const StringFlagItem *map)
{
	int i;
	for (i=0; map[i].str != NULL; i++) {
		if (0==strcmp(map[i].str, str)) {
			return map[i].flag;
		}
	}
  return 0;
}

int 
gpi_string_or_to_flags(const char *str, 
		       unsigned int *flags,
		       const StringFlagItem *map)
{
	int i;
	int found = 0;
	for (i=0; map[i].str != NULL; i++) {
		if (0==strcmp(map[i].str, str)) {
			(*flags) |= map[i].flag;
			found = 1;
		}
	}
	if (found) {
		return 0;
	} else {
		return 1;
	}
}

unsigned int
gpi_string_list_to_flags(const char *str[], 
		     const StringFlagItem *map)
{
	int i;
	unsigned int flags = 0;
	for (i=0; str[i] != NULL; i++) {
		flags |= gpi_string_to_flag(str[i], map);
	}
	return flags;
}

#endif /* _GPHOTO2_INTERNAL_CODE */


/*
 * Local Variables:
 * c-file-style:"linux"
 * indent-tabs-mode:t
 * End:
 */
