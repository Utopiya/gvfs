/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_VFS_JOB_DBUS_H__
#define __G_VFS_JOB_DBUS_H__

#include <dbus/dbus.h>
#include <gvfsjob.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_DBUS         (g_vfs_job_dbus_get_type ())
#define G_VFS_JOB_DBUS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_DBUS, GVfsJobDBus))
#define G_VFS_JOB_DBUS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_DBUS, GVfsJobDBusClass))
#define G_VFS_IS_JOB_DBUS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_DBUS))
#define G_VFS_IS_JOB_DBUS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_DBUS))
#define G_VFS_JOB_DBUS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_DBUS, GVfsJobDBusClass))

typedef struct _GVfsJobDBus        GVfsJobDBus;
typedef struct _GVfsJobDBusClass   GVfsJobDBusClass;

struct _GVfsJobDBus
{
  GVfsJob parent_instance;

  DBusConnection *connection;
  DBusMessage *message;
};

struct _GVfsJobDBusClass
{
  GVfsJobClass parent_class;

  /* Might be called on an i/o thread */
  DBusMessage * (*create_reply) (GVfsJob *job,
				 DBusConnection *connection,
				 DBusMessage *message);
};

GType g_vfs_job_dbus_get_type (void) G_GNUC_CONST;

gboolean        g_vfs_job_dbus_is_serial      (GVfsJobDBus    *job_dbus,
					       DBusConnection *connection,
					       dbus_uint32_t   serial);
DBusConnection *g_vfs_job_dbus_get_connection (GVfsJobDBus    *job_dbus);
DBusMessage    *g_vfs_job_dbus_get_message    (GVfsJobDBus    *job_dbus);

G_END_DECLS

#endif /* __G_VFS_JOB_DBUS_H__ */
