/* paths.c: Path-related compatibility routines
   Copyright (c) 1999-2007 Philip Kendall

   $Id: paths.c 3844 2008-11-19 21:31:07Z fredm $

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

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include <config.h>

#include "compat.h"

const char*
compat_get_temp_path( void )
{
  return "T:";
}

const char*
compat_get_home_path( void )
{
  return "PROGDIR:settings";
}

int
compat_is_absolute_path( const char *path )
{
  return strchr(path,':');
}
