/* ptp-bugs.h
 *
 * Copyright (C) 2005 Hubert Figuiere <hfiguiere@teaser.fr>
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


#ifndef __PTP_BUGS_H_
#define __PTP_BUGS_H_


#define PTPBUG_DCIM_WRONG_PARENT (1<<0)
#define PTPBUG_DELETE_SENDS_EVENT (1<<1)


#define DCIM_WRONG_PARENT_BUG(x) \
  ((x)->bugs & PTPBUG_DCIM_WRONG_PARENT)
#define DELETE_SENDS_EVENT(x) \
  ((x)->bugs & PTPBUG_DELETE_SENDS_EVENT)


#endif
