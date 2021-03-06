/* fdd.c: Routines for emulating floppy disk drives
   Copyright (c) 2007 Gergely Szasz

   $Id: fdd.c 3942 2009-01-10 14:18:46Z pak21 $

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

   Author contact information:

   Philip: philip-fuse@shadowmagic.org.uk

*/

#include <config.h>

#include <libspectrum.h>

#include "bitmap.h"
#include "compat.h"
#include "disk/upd_fdc.h"
#include "disk/wd_fdc.h"
#include "event.h"
#include "fdd.h"
#include "machine.h"
#include "spectrum.h"

#define FDD_LOAD_FACT 2
#define FDD_HEAD_FACT 16			/* load head */
#define FDD_STEP_FACT 34

static const char *fdd_error[] = {
  "OK",
  "invalid disk geometry",
  "read only disk",

  "unknown error code"			/* will be the last */
};

static void
fdd_event( libspectrum_dword last_tstates, int event, void *user_data );

static int motor_event;

int
fdd_init_events( void )
{
  int error;

  motor_event = event_register( fdd_event, "FDD motor on" );
  if( motor_event == -1 ) return 1;

  error = upd_fdc_init_events();
  if( error ) return error;

  error = wd_fdc_init_events();
  if( error ) return error;

  return 0;
}

const char *
fdd_strerror( int error )
{
  if( error >= FDD_LAST_ERROR )
    error = FDD_LAST_ERROR;
  return fdd_error[ error ];
}

/*
 * disk->sides  1  2  1  2  1  2  1  2
 * d->c_head    0  0  1  1  0  0  1  1
 * d->upside    0  0  0  0  1  1  1  1
 *
 * UNREADABLE   0  0  1  0  1  0  0  0
 */

static void
fdd_set_data( fdd_t *d, int fact )
{
  int head = d->upsidedown ? 1 - d->c_head : d->c_head;

  if( !d->loaded )
    return;

  if( d->unreadable || ( d->disk->sides == 1 && head == 1 ) ||
      d->c_cylinder >= d->disk->cylinders ) {
    d->disk->track = NULL;
    d->disk->clocks = NULL;
    return;
  }

  d->disk->track = d->disk->data + 
		   ( d->disk->sides * d->c_cylinder + head ) * d->disk->tlen;
  d->disk->clocks = d->disk->track + d->disk->bpt;
  if( fact > 0 ) {
    /* this generate a bpt/fact +-10% rectangular distribution skip in bytes 
       i know, we should use the higher bits of rand(), but we not
       keen on _real_ (pseudo)random numbers... ;)
    */
    d->disk->i += d->disk->bpt / fact + d->disk->bpt *
		  ( rand() % 10 + rand() % 10 - 9 ) / fact / 100;
    while( d->disk->i >= d->disk->bpt )
      d->disk->i -= d->disk->bpt;
  }
  d->index = d->disk->i ? 0 : 1;
}

/* initialise fdd */
int
fdd_init( fdd_t *d, fdd_type_t type, int heads, int cyls )
{
  d->fdd_heads = d->fdd_cylinders = d->c_head = d->c_cylinder = 0;
  d->upsidedown = d->unreadable = d->loaded = d->auto_geom = d->selected = 0;
  d->index = d->tr00 = d->wrprot = 1;
  d->disk = NULL;
  d->type = type;

  if( heads < 0 || heads > 2 || cyls < 0 || cyls > 83 )
    return d->status = FDD_GEOM;

  if( heads == 0 || cyls == 0 )
    d->auto_geom = 1;
  d->fdd_heads = heads;
  d->fdd_cylinders = cyls;

  return d->status = FDD_OK;
}

void
fdd_motoron( fdd_t *d, int on )
{
  if( !d->loaded )
    return;
  on = on > 0 ? 1 : 0;
  if( d->motoron == on )
    return;
  d->motoron = on;
  /*
  TEAC FD55 Spec:
  (13) READY output signal
    i)	The FDD is powered on.
    ii)	Disk is installed.
    iii)The disk rotates at more than 50% of the rated speed.
    iv) Two index pulses have been counted after item iii) is
	satisfied
    
    Note: Pre-ready is the state that at least one INDEX
	pulse has been detected after item iii) is satisfied
  */
  event_remove_type_user_data( motor_event, d );		/* remove pending motor-on event for *this* drive */
  if( on ) {
    event_add_with_data( tstates + 4 *			/* 2 revolution: 2 * 200 / 1000 */
			 machine_current->timings.processor_speed / 10,
			 motor_event, d );
  } else {
    event_add_with_data( tstates + 3 *			/* 1.5 revolution */
			 machine_current->timings.processor_speed / 10,
			 motor_event, d );
  }
}

void
fdd_head_load( fdd_t *d, int load )
{
  if( !d->loaded )
    return;
  load = load > 0 ? 1 : 0;
  if( d->loadhead == load )
    return;
  d->loadhead = load;
  fdd_set_data( d, FDD_HEAD_FACT );
}

void
fdd_select( fdd_t *d, int select )
{
  d->selected = select > 0 ? 1 : 0;
  /*
      ... Drive Select when activated to a logical
      zero level, will load the R/W head against the
      diskette enabling contact of the R/W head against
      the media. ...
  */
  if( d->type == FDD_SHUGART )
    fdd_head_load( d, d->selected );
}


/* load a disk into fdd */
int
fdd_load( fdd_t *d, disk_t *disk, int upsidedown )
{
  if( disk->sides < 0 || disk->sides > 2 ||
      disk->cylinders < 0 || disk->cylinders > 83 )
    return d->status = FDD_GEOM;

  if( d->auto_geom )
    d->fdd_heads = disk->sides;		/* 1 or 2 */
  if( d->auto_geom )
    d->fdd_cylinders = disk->cylinders > 42 ? 83 : 42;

  if( disk->cylinders > d->fdd_cylinders )
    d->unreadable = 1;

  d->disk = disk;
  d->upsidedown = upsidedown > 0 ? 1 : 0;
  d->wrprot = d->disk->wrprot;		/* write protect */
  d->loaded = 1;
  if( d->type == FDD_SHUGART && d->selected )
    fdd_head_load( d, 1 );

  fdd_set_data( d, FDD_LOAD_FACT );
  return d->status = FDD_OK;
}

void
fdd_unload( fdd_t *d )
{
  d->ready = d->loaded = 0;
  d->index = d->wrprot = 1;
  d->disk = NULL;
  fdd_motoron( d, 0 );
  if( d->type == FDD_SHUGART && d->selected )
    fdd_head_load( d, 0 );
}

/* change current head */
void
fdd_set_head( fdd_t *d, int head )
{
  if( d->fdd_heads == 1 )
    return;

  head = head > 0 ? 1 : 0;
  if( d->c_head == head )
    return;

  d->c_head = head;
  fdd_set_data( d, 0 );
}

/* change current track dir = 1 / -1 */
void
fdd_step( fdd_t *d, fdd_dir_t direction )
{
  if( direction == FDD_STEP_OUT ) {
    if( d->c_cylinder > 0 )
      d->c_cylinder--;
  } else { /* direction == FDD_STEP_IN */
    if( d->c_cylinder < d->fdd_cylinders - 1 )
      d->c_cylinder++;
  }
  if( d->c_cylinder == 0 )
    d->tr00 = 1;
  else
    d->tr00 = 0;

  fdd_set_data( d, FDD_STEP_FACT );
}

/* read/write next byte from/to sector */
int
fdd_read_write_data( fdd_t *d, fdd_write_t write )
{
  if( !d->selected || !d->ready || !d->loadhead || d->disk->track == NULL ) {
    if( d->loaded && d->motoron ) {			/* spin the disk */
      if( d->disk->i >= d->disk->bpt ) {		/* next data byte */
        d->disk->i = 0;
      }
      if( !write )
        d->data = 0x100;				/* no data */
      d->disk->i++;
      d->index = d->disk->i >= d->disk->bpt ? 1 : 0;
    }
    return d->status = FDD_OK;
  }

  if( d->disk->i >= d->disk->bpt ) {		/* next data byte */
    d->disk->i = 0;
  }
  if( write ) {
    if( d->disk->wrprot ) {
      d->disk->i++;
      d->index = d->disk->i >= d->disk->bpt ? 1 : 0;
      return d->status = FDD_RDONLY;
    }
    d->disk->track[ d->disk->i ] = d->data & 0x00ff;
    if( d->data & 0xff00 )
      bitmap_set( d->disk->clocks, d->disk->i );
    else
      bitmap_reset( d->disk->clocks, d->disk->i );
    d->disk->dirty = 1;
  } else {	/* read */
    d->data = d->disk->track[ d->disk->i ];
    if( bitmap_test( d->disk->clocks, d->disk->i ) )
      d->data |= 0xff00;
  }
  d->disk->i++;
  d->index = d->disk->i >= d->disk->bpt ? 1 : 0;

  return d->status = FDD_OK;
}

void
fdd_wrprot( fdd_t *d, int wrprot )
{
  if( !d->loaded )
    return;

  d->wrprot = d->disk->wrprot = wrprot;
}

void
fdd_wait_index_hole( fdd_t *d )
{
  if( !d->selected || !d->ready )
    return;

  d->disk->i = 0;
  d->index = 1;
}

static void
fdd_event( libspectrum_dword last_tstates GCC_UNUSED, int event,
           void *user_data ) 
{
  fdd_t *d = user_data;
  d->ready = ( d->motoron & d->loaded );	/* 0x01 & 0x01 */
}
