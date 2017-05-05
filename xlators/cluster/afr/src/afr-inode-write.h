/*
   Copyright (c) 2007-2011 Gluster, Inc. <http://www.gluster.com>
   This file is part of GlusterFS.

   GlusterFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   GlusterFS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#ifndef __INODE_WRITE_H__
#define __INODE_WRITE_H__

int32_t
afr_chmod (call_frame_t *frame, xlator_t *this,
	   loc_t *loc, mode_t mode, dict_t *xdata);

int32_t
afr_chown (call_frame_t *frame, xlator_t *this,
	   loc_t *loc, uid_t uid, gid_t gid, dict_t *xdata);

int
afr_fchown (call_frame_t *frame, xlator_t *this,
	    fd_t *fd, uid_t uid, gid_t gid, dict_t *xdata);

int32_t
afr_fchmod (call_frame_t *frame, xlator_t *this,
	    fd_t *fd, mode_t mode, dict_t *xdata);

int32_t
afr_writev (call_frame_t *frame, xlator_t *this, fd_t *fd,
	    struct iovec *vector, int32_t count, off_t offset,
            uint32_t flags, struct iobref *iobref, dict_t *xdata);

int32_t
afr_truncate (call_frame_t *frame, xlator_t *this,
	      loc_t *loc, off_t offset, dict_t *xdata);

int32_t
afr_ftruncate (call_frame_t *frame, xlator_t *this,
	       fd_t *fd, off_t offset, dict_t *xdata);

int32_t
afr_utimens (call_frame_t *frame, xlator_t *this,
	     loc_t *loc, struct timespec tv[2], dict_t *xdata);

int
afr_setattr (call_frame_t *frame, xlator_t *this,
             loc_t *loc, struct iatt *buf, int32_t valid, dict_t *xdata);

int
afr_fsetattr (call_frame_t *frame, xlator_t *this,
              fd_t *fd, struct iatt *buf, int32_t valid, dict_t *xdata);

int32_t
afr_setxattr (call_frame_t *frame, xlator_t *this,
              loc_t *loc, dict_t *dict, int32_t flags, dict_t *xdata);

int32_t
afr_fsetxattr (call_frame_t *frame, xlator_t *this,
               fd_t *fd, dict_t *dict, int32_t flags, dict_t *xdata);

int32_t
afr_removexattr (call_frame_t *frame, xlator_t *this,
		 loc_t *loc, const char *name, dict_t *xdata);

int32_t
afr_fremovexattr (call_frame_t *frame, xlator_t *this,
                  fd_t *fd, const char *name, dict_t *xdata);

#endif /* __INODE_WRITE_H__ */
