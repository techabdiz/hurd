/* Private declarations for fileserver library
   Copyright (C) 1994 Free Software Foundation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#ifndef DISKFS_PRIV_H
#define DISKFS_PRIV_H

#include <mach.h>
#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/fshelp.h>
#include <hurd/ioserver.h>
#include <assert.h>

#include "diskfs.h"

extern mach_port_t fs_control_port;	/* receive right */

enum porttype
{
  PT_NONE,
  PT_PROTID,
  PT_PAGER,
  PT_CTL,
  PT_EXECBOOT,
  PT_INITBOOT,
  PT_TRANSBOOT,
};

volatile struct mapped_time_value *_diskfs_mtime;

/* Needed for MiG. */
typedef struct protid *protid_t;

/* Called by MiG to translate ports into struct protid *.  
   fsmutations.h arranges for this to happen for the io and
   fs interfaces. */
extern inline struct protid *
begin_using_protid_port (file_t port)
{
  return ports_check_port_type (port, PT_PROTID);
}

/* Called by MiG after server routines have been run; this
   balances begin_using_protid_port, and is arranged for the io
   and fs interfaces by fsmutations.h. */
extern inline void
end_using_protid_port (struct protid *cred)
{
  ports_done_with_port (cred);
}

/* Actually read or write a file.  The file size must already permit
   the requested access.  NP is the file to read/write.  DATA is a buffer
   to write from or fill on read.  OFFSET is the absolute address (-1
   not permitted here); AMT is the size of the read/write to perform;
   DIR is set for writing and clear for reading.  The inode must
   be locked.   If NOTIME is set, then don't update the access or
   modify times on the file.  */
error_t _diskfs_rdwr_internal (struct node *np, char *data, int offset, 
			       int amt, int dir, int notime);

/* Clean routine for control port. */
void _diskfs_control_clean (void *);

/* Number of outstanding PT_CTL ports. */
extern int _diskfs_ncontrol_ports;

/* Lock for _diskfs_ncontrol_ports. */
extern spin_lock_t _diskfs_control_lock;

/* This macro locks the node associated with PROTID, and then
   evaluates the expression OPERATION; then it syncs the inode
   (without waiting) and unlocks everything, and then returns
   the value `err' (which can be set by OPERATION if desired). */
#define CHANGE_NODE_FIELD(PROTID, OPERATION)				    \
({									    \
  error_t err = 0;							    \
  struct node *np;							    \
  									    \
  if (!(PROTID))							    \
    return EOPNOTSUPP;							    \
  									    \
  if (diskfs_readonly)							    \
    return EROFS;							    \
  									    \
  np = (PROTID)->po->np;						    \
  									    \
  mutex_lock (&np->lock);						    \
  (OPERATION);								    \
  diskfs_node_update (np, 0);						    \
  mutex_unlock (&np->lock);						    \
  return err;								    \
})

/* Bits the user is permitted to set with io_*_openmodes */
#define HONORED_STATE_MODES (O_APPEND|O_ASYNC|O_FSYNC|O_NONBLOCK|O_NOATIME)

/* Bits that are turned off after open */
#define OPENONLY_STATE_MODES (O_CREAT|O_EXCL|O_NOLINK|O_NOTRANS|O_NONBLOCK)

#endif
