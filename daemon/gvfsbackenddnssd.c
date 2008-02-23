/* GIO - GLib Input, Output and Streaming Library
 * Original work, Copyright (C) 2003 Red Hat, Inc
 * GVFS port, Copyright (c) 2008 Andrew Walton.
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
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gurifuncs.h>
#include <gio/gio.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <avahi-glib/glib-watch.h>
#include <avahi-glib/glib-malloc.h>

#include "gvfsbackenddnssd.h"

#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsmonitor.h"

static struct {
	char *type;
	char *method;
	char *icon;
	gpointer handle;
} dns_sd_types[] = {
	{"_ftp._tcp", "ftp", "gnome-fs-ftp"},
	{"_webdav._tcp", "dav", "gnome-fs-share"},
	{"_webdavs._tcp", "davs", "gnome-fs-share"},
	{"_sftp-ssh._tcp", "sftp", "gnome-fs-ssh"},
};

static AvahiClient *global_client = NULL;
static gboolean avahi_initialized = FALSE;

static GList *dnssd_backends = NULL;

typedef struct {
  char *file_name; 
  char *name;
  char *type;
  char *target_uri;
  GIcon *icon;
} LinkFile;

static LinkFile root = { "/" };

struct _GVfsBackendDnsSd
{
  GVfsBackend parent_instance;
  GVfsMonitor *root_monitor;
  char *domain;
  GMountSpec *mount_spec;
  GList *files; /* list of LinkFiles */

  GList *browsers;
};

typedef struct _GVfsBackendDnsSd GVfsBackendDnsSd;

G_DEFINE_TYPE (GVfsBackendDnsSd, g_vfs_backend_dns_sd, G_VFS_TYPE_BACKEND)

static void add_browsers (GVfsBackendDnsSd *backend);
static void remove_browsers (GVfsBackendDnsSd *backend);
static AvahiClient *get_global_avahi_client (void);

/* Callback for state changes on the Client */
static void
avahi_client_callback (AvahiClient *client, AvahiClientState state, void *userdata)
{
  /* We need to set this early, as the add_browsers call below may reenter
     when this is called from the client creation call */
  if (global_client == NULL)
    global_client = client;
  
  if (state == AVAHI_CLIENT_FAILURE)
    {
      if (avahi_client_errno (client) == AVAHI_ERR_DISCONNECTED)
	{
	  /* Remove the service browsers from the handles */
	  g_list_foreach (dnssd_backends, (GFunc)remove_browsers, NULL);
	  
	  /* Destroy old client */
	  avahi_client_free (client);
	  global_client = NULL;
	  avahi_initialized = FALSE;
	  
	  /* Reconnect */
	  get_global_avahi_client ();
	}
    }
  else if (state == AVAHI_CLIENT_S_RUNNING)
    {
      /* Start browsing again */
      g_list_foreach (dnssd_backends, (GFunc)add_browsers, NULL);
    }
}

static AvahiClient *
get_global_avahi_client (void)
{
  static AvahiGLibPoll *glib_poll = NULL;
  int error;
  
  if (!avahi_initialized)
    {
      avahi_initialized = TRUE;
      
      if (glib_poll == NULL)
	{
	  avahi_set_allocator (avahi_glib_allocator ());
	  glib_poll = avahi_glib_poll_new (NULL, G_PRIORITY_DEFAULT);
	}
      
      /* Create a new AvahiClient instance */
      global_client = avahi_client_new (avahi_glib_poll_get (glib_poll),
					AVAHI_CLIENT_NO_FAIL,
					avahi_client_callback,
					glib_poll,
					&error);
      
      if (global_client == NULL)
	{    
	  /* Print out the error string */
	  g_warning ("Error initializing Avahi: %s", avahi_strerror (error));
	  return NULL;
	}
    }
  
  return global_client;
}

static GIcon *
get_icon_for_type (const char *type)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (dns_sd_types); i++)
    {
      if (strcmp (type, dns_sd_types[i].type) == 0)
	return g_themed_icon_new (dns_sd_types[i].icon);
    }
  
  return g_themed_icon_new ("text-x-generic");
}

static const char *
get_method_for_type (const char *type)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (dns_sd_types); i++)
    {
      if (strcmp (type, dns_sd_types[i].type) == 0)
	return dns_sd_types[i].method;
    }
  
  return NULL;
}

static char *
encode_filename (const char *service,
		 const char *type)
{
  GString *string;
  const char *p;
  
  string = g_string_new (NULL);
  
  p = service;
  
  while (*p)
    {
      if (*p == '\\') 
	g_string_append (string, "\\\\");
      else if (*p == '.') 
	g_string_append (string, "\\.");
      else if (*p == '/') 
	g_string_append (string, "\\s");
      else
	g_string_append_c (string, *p);
      p++;
    }
  
  g_string_append_c (string, '.');
  g_string_append (string, type);
  
  return g_string_free (string, FALSE);
}

static LinkFile *
link_file_new (const char *name,
	       const char *type,
	       const char *domain,
	       const char *host_name,
	       AvahiProtocol protocol,
	       const AvahiAddress *address,
	       uint16_t port,
	       AvahiStringList *txt)
{
  LinkFile *file;
  char *path, *user, *user_str;
  AvahiStringList *path_l, *user_l;
  char a[128];
  const char *method;
  
  file = g_slice_new0 (LinkFile);

  file->name = g_strdup (name);
  file->type = g_strdup (type);
  file->file_name = encode_filename (name, type);
  file->icon = get_icon_for_type (type);
	

  path = NULL;
  user_str = NULL;
  if (txt != NULL)
    {
      path_l = avahi_string_list_find (txt, "path");
      if (path_l != NULL)
	avahi_string_list_get_pair (path_l, NULL, &path, NULL);
      
      user_l = avahi_string_list_find (txt, "u");
      if (user_l != NULL)
	{
	  avahi_string_list_get_pair (user_l, NULL, &user, NULL);
	  
	  user_str = g_strconcat (user, "@", NULL);
	}
    }
  
  if (path == NULL)
    path = g_strdup ("/");
      
  avahi_address_snprint (a, sizeof(a), address);

  method = get_method_for_type (type);

  if (protocol == AVAHI_PROTO_INET6)
	/* an ipv6 address, follow rfc2732 */
    file->target_uri = g_strdup_printf ("%s://%s[%s]:%d%s",
					method,
					user_str?user_str:"",
					a, port, path);
  else
    file->target_uri = g_strdup_printf ("%s://%s%s:%d%s",
					method,
					user_str?user_str:"",
					a, port, path);
  g_free (user_str);
  g_free (path);

  return file;
}

static void
link_file_free (LinkFile *file)
{
  g_free (file->file_name);
  g_free (file->name);
  g_free (file->type);
  g_free (file->target_uri);
 
  if (file->icon)
    g_object_unref (file->icon);
  
  g_slice_free (LinkFile, file);
}

static LinkFile *
lookup_link_file_by_name_and_type (GVfsBackendDnsSd *backend,
				   const char *name,
				   const char *type)
{
  GList *l;
  LinkFile *file;

  for (l = backend->files; l != NULL; l = l->next)
    {
      file = l->data;
      if (strcmp (file->name, name) == 0 &&
	  strcmp (file->type, type) == 0)
        return file;
    }
  
  return NULL;
}

static LinkFile *
lookup_link_file (GVfsBackendDnsSd *backend,
		  GVfsJob *job,
		  const char *file_name)
{
  GList *l;
  LinkFile *file;

  if (*file_name != '/')
    goto out;

  while (*file_name == '/')
    file_name++;

  if (*file_name == 0)
    return &root;
  
  if (strchr (file_name, '/') != NULL)
    goto out;
  
  for (l = backend->files; l != NULL; l = l->next)
    {
      file = l->data;
      if (strcmp (file->file_name, file_name) == 0)
        return file;
    }

  out:
  g_vfs_job_failed (job, G_IO_ERROR,
		    G_IO_ERROR_NOT_FOUND,
		    _("File doesn't exist"));

  return NULL;
}


static void
file_info_from_file (LinkFile *file,
                     GFileInfo *info)
{
  g_return_if_fail (file != NULL || info != NULL);

  g_file_info_set_name (info, file->file_name);
  g_file_info_set_display_name (info, file->name);

  if (file->icon) 
    g_file_info_set_icon (info, file->icon);

  g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
  g_file_info_set_size(info, 0);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, 
                                    file->target_uri);
}

/* Backend Functions */
static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *file_name,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  LinkFile *file;
  GList *l;
  GFileInfo *info;
  
  file = lookup_link_file (G_VFS_BACKEND_DNS_SD (backend),
			      G_VFS_JOB (job), file_name);
  
  if (file != &root)
    {
      if (file != NULL)
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_NOT_DIRECTORY,
                          _("The file is not a directory"));
      return TRUE;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
  
  /* Enumerate root */
  for (l = G_VFS_BACKEND_DNS_SD (backend)->files; l != NULL; l = l->next)
    {
      file = l->data;
      info = g_file_info_new ();
      file_info_from_file (file, info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);
    }

  g_vfs_job_enumerate_done (job);
  
  return TRUE;
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *file_name,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  LinkFile *file;

  file = lookup_link_file (G_VFS_BACKEND_DNS_SD (backend), 
                              G_VFS_JOB (job), file_name);

  if (file == &root)
    {
      GIcon *icon;
      g_file_info_set_name (info, "/");
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      /* TODO: Name */
      g_file_info_set_display_name (info, _("dns-sd"));
      icon = g_themed_icon_new ("network-workgroup");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_content_type (info, "inode/directory");
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else if (file != NULL)
    {
      file_info_from_file (file, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  return TRUE;
}


static void
resolve_callback (AvahiServiceResolver *r,
		  AvahiIfIndex interface,
		  AvahiProtocol protocol,
		  AvahiResolverEvent event,
		  const char *name,
		  const char *type,
		  const char *domain,
		  const char *host_name,
		  const AvahiAddress *address,
		  uint16_t port,
		  AvahiStringList *txt,
		  AvahiLookupResultFlags flags,
		  void *userdata)
{
  GVfsBackendDnsSd *backend = userdata;
  LinkFile *file;
  char *path;

  if (event == AVAHI_RESOLVER_FAILURE)
    return;
  
  /* Link-local ipv6 address, can't make a uri from this, ignore */
  if (address->proto == AVAHI_PROTO_INET6 &&
      address->data.ipv6.address[0] == 0xfe &&
      address->data.ipv6.address[1] == 0x80)
    return;
  
  file = lookup_link_file_by_name_and_type (backend,
					    name, type);

  if (file != NULL)
    return;

  file = link_file_new (name, type, domain, host_name, protocol,
			address, port, txt);

  backend->files = g_list_prepend (backend->files, file);

  path = g_strconcat ("/", file->file_name, NULL);
  g_vfs_monitor_emit_event (backend->root_monitor,
			    G_FILE_MONITOR_EVENT_CREATED,
			    path,
			    NULL);
  g_free (path);
}

static void
browse_callback (AvahiServiceBrowser *b,
		 AvahiIfIndex interface,
		 AvahiProtocol protocol,
		 AvahiBrowserEvent event,
		 const char *name,
		 const char *type,
		 const char *domain,
		 AvahiLookupResultFlags flags,
		 void *userdata)
{
  GVfsBackendDnsSd *backend = userdata;
  AvahiServiceResolver *sr;
  AvahiClient *client;
  LinkFile *file;
  char *path;
  
  switch (event)
    {
    case AVAHI_BROWSER_FAILURE:
      break;
      
    case AVAHI_BROWSER_NEW:
      client = get_global_avahi_client ();
      
      /* We ignore the returned resolver object. In the callback
	 function we free it. If the server is terminated before
	 the callback function is called the server will free
	 the resolver for us. */
      
      sr = avahi_service_resolver_new (client, interface, protocol,
				       name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, backend);

      if (sr == NULL) 
	g_warning ("Failed to resolve service name '%s': %s\n", name, avahi_strerror (avahi_client_errno (client)));
      
      break;
      
    case AVAHI_BROWSER_REMOVE:
      file = lookup_link_file_by_name_and_type (backend,
						name, type);

      if (file != NULL)
	{
	  backend->files = g_list_remove (backend->files, file);
	  
	  path = g_strconcat ("/", file->file_name, NULL);
	  g_vfs_monitor_emit_event (backend->root_monitor,
				    G_FILE_MONITOR_EVENT_DELETED,
				    path,
				    NULL);
	  g_free (path);

	  link_file_free (file);
	}
      
      break;
      
    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      break;
    }
}

static void
browse_type (GVfsBackendDnsSd *backend,
	     const char *type)
{
  AvahiClient *client;
  AvahiServiceBrowser *sb;
  const char *domain;

  client = get_global_avahi_client ();

  domain = NULL;
  if (strcmp (backend->domain, "local") != 0)
    domain = backend->domain;
      
  sb = avahi_service_browser_new (client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				  type, domain, 0, browse_callback, backend);
  
  if (sb == NULL)
    {
      g_warning ("Failed to create service browser: %s\n", avahi_strerror( avahi_client_errno (client)));
      return;
    }

  backend->browsers = g_list_prepend (backend->browsers, sb);

}

static void
add_browsers (GVfsBackendDnsSd *backend)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (dns_sd_types); i++) 
    browse_type (backend, dns_sd_types[i].type);
}

static void
remove_browsers (GVfsBackendDnsSd *backend)
{
  g_list_free (backend->browsers);
  backend->browsers = NULL;
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendDnsSd *op_backend = G_VFS_BACKEND_DNS_SD (backend);
  GMountSpec *real_mount_spec;
  const char *domain;

  domain = g_mount_spec_get (mount_spec, "host");

  if (domain == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"No domain specified for dns-sd share");
      return TRUE;
    }

  op_backend->domain = g_strdup (domain);

  if (get_global_avahi_client() == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			"Unable to initialize avahi");
      return TRUE;
    }
  
  real_mount_spec = g_mount_spec_new ("dns-sd");
  g_mount_spec_set (real_mount_spec, "host", op_backend->domain);
  g_vfs_backend_set_mount_spec (backend, real_mount_spec);
  op_backend->mount_spec = real_mount_spec;

  op_backend->root_monitor = g_vfs_monitor_new (backend);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

/* handles both file and dir monitors, 
 * as we really don't "support" (e.g. fire events for) either, yet. */
static gboolean
try_create_monitor (GVfsBackend *backend,
                    GVfsJobCreateMonitor *job,
                    const char *file_name,
                    GFileMonitorFlags flags)
{
  LinkFile *file;
  GVfsBackendDnsSd *network_backend;

  network_backend = G_VFS_BACKEND_DNS_SD (backend);

  file = lookup_link_file (network_backend, G_VFS_JOB (job), file_name);

  if (file != &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_NOT_SUPPORTED,
			_("Can't monitor file or directory."));
      return TRUE;
    }
  
  g_vfs_job_create_monitor_set_monitor (job, network_backend->root_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
g_vfs_backend_dns_sd_init (GVfsBackendDnsSd *network_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (network_backend);

  dnssd_backends = g_list_prepend (dnssd_backends, backend);
  
  /* TODO: Names, etc */
  g_vfs_backend_set_display_name (backend, _("Dns-SD"));
  g_vfs_backend_set_stable_name (backend, _("Network"));
  g_vfs_backend_set_icon_name (backend, "network-workgroup");
  g_vfs_backend_set_user_visible (backend, FALSE);

}

static void
g_vfs_backend_dns_sd_finalize (GObject *object)
{
  GVfsBackendDnsSd *backend;
  
  backend = G_VFS_BACKEND_DNS_SD (object);

  dnssd_backends = g_list_remove (dnssd_backends, backend);

  if (backend->mount_spec)
    g_mount_spec_unref (backend->mount_spec);
  
  if (backend->root_monitor)
    g_object_unref (backend->root_monitor);
  
  g_free (backend->domain);

  g_list_foreach (backend->files, (GFunc)link_file_free, NULL);
  
  if (G_OBJECT_CLASS (g_vfs_backend_dns_sd_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_dns_sd_parent_class)->finalize) (object);
}

static void
g_vfs_backend_dns_sd_class_init (GVfsBackendDnsSdClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_dns_sd_finalize;

  backend_class->try_mount        = try_mount;
  backend_class->try_query_info   = try_query_info;
  backend_class->try_enumerate    = try_enumerate;
  backend_class->try_create_dir_monitor = try_create_monitor;
  backend_class->try_create_file_monitor = try_create_monitor;
}
