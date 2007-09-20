/* -*- Mode: C; tab-width: 5; indent-tabs-mode: t; c-basic-offset: 5 -*- */

/* NetworkManager -- Network link manager
 *
 * Dan Williams <dcbw@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2005 Red Hat, Inc.
 */

#include <glib.h>
#include <string.h>
#include <dbus/dbus.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include "nm-vpn-service.h"
#include "nm-dbus-manager.h"
#include "nm-utils.h"

G_DEFINE_TYPE (NMVPNService, nm_vpn_service, G_TYPE_OBJECT)

typedef struct {
	NMDBusManager *dbus_mgr;
	char *name;
	char *dbus_service;
	char *program;

	GPid pid;
	GSList *connections;
	guint service_start_timeout;
	gulong name_owner_id;
} NMVPNServicePrivate;

#define NM_VPN_SERVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_VPN_SERVICE, NMVPNServicePrivate))

#define VPN_CONNECTION_GROUP "VPN Connection"

static GKeyFile *
find_service_file (const char *name)
{
	GDir *dir;
	const char *fn;
	GKeyFile *key_file = NULL;

	dir = g_dir_open (VPN_NAME_FILES_DIR, 0, NULL);
	if (!dir)
		return NULL;

	while ((fn = g_dir_read_name (dir))) {
		char *path;
		gboolean found = FALSE;

		key_file = g_key_file_new ();
		path = g_build_filename (VPN_NAME_FILES_DIR, fn, NULL);

		if (g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL)) {
			gchar *val;

			val = g_key_file_get_string (key_file, VPN_CONNECTION_GROUP, "service", NULL);
			if (val) {
				if (!strcmp (val, name))
					found = TRUE;
				g_free (val);
			}
		}

		g_free (path);

		if (found)
			break;

		g_key_file_free (key_file);
		key_file = NULL;
	}

	g_dir_close (dir);

	return key_file;
}

NMVPNService *
nm_vpn_service_new (const char *name)
{
	GKeyFile *key_file;
	NMVPNService *service = NULL;
	NMVPNServicePrivate *priv;
	char *dbus_service = NULL;
	char *program = NULL;
	gboolean success = FALSE;

	g_return_val_if_fail (name != NULL, NULL);

	key_file = find_service_file (name);
	if (!key_file)
		return NULL;

	dbus_service = g_key_file_get_string (key_file, VPN_CONNECTION_GROUP, "service", NULL);
	if (!dbus_service)
		goto out;

	program = g_key_file_get_string (key_file, VPN_CONNECTION_GROUP, "program", NULL);
	if (!program)
		goto out;

	service = (NMVPNService *) g_object_new (NM_TYPE_VPN_SERVICE, NULL);
	if (!service)
		goto out;

	priv = NM_VPN_SERVICE_GET_PRIVATE (service);

	priv->name = g_strdup (name);
	priv->dbus_service = dbus_service;
	priv->program = program;

	success = TRUE;

 out:
	g_key_file_free (key_file);

	if (!success) {
		g_free (dbus_service);
		g_free (program);
	}

	return service;
}

const char *
nm_vpn_service_get_name (NMVPNService *service)
{
	g_return_val_if_fail (NM_IS_VPN_SERVICE (service), NULL);

	return NM_VPN_SERVICE_GET_PRIVATE (service)->name;
}

static void
nm_vpn_service_connections_stop (NMVPNService *service, gboolean fail)
{
	NMVPNServicePrivate *priv = NM_VPN_SERVICE_GET_PRIVATE (service);
	GSList *iter;

	for (iter = priv->connections; iter; iter = iter->next)
		fail ? nm_vpn_connection_fail (NM_VPN_CONNECTION (iter->data)) 
			: nm_vpn_connection_disconnect (NM_VPN_CONNECTION (iter->data));
}

/*
 * nm_vpn_service_child_setup
 *
 * Set the process group ID of the newly forked process
 *
 */
static void
nm_vpn_service_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point */
	pid_t pid = getpid ();
	setpgid (pid, pid);
}

static void
vpn_service_watch_cb (GPid pid, gint status, gpointer user_data)
{
	NMVPNService *service = NM_VPN_SERVICE (user_data);
	NMVPNServicePrivate *priv = NM_VPN_SERVICE_GET_PRIVATE (service);

	if (WIFEXITED (status)) {
		guint err = WEXITSTATUS (status);

		if (err != 0)
			nm_warning ("VPN service '%s' exited with error: %d",
					  nm_vpn_service_get_name (service), WSTOPSIG (status));
	} else if (WIFSTOPPED (status))
		nm_warning ("VPN service '%s' stopped unexpectedly with signal %d",
				  nm_vpn_service_get_name (service), WSTOPSIG (status));
	else if (WIFSIGNALED (status))
		nm_warning ("VPN service '%s' died with signal %d",
				  nm_vpn_service_get_name (service), WTERMSIG (status));
	else
		nm_warning ("VPN service '%s' died from an unknown cause", 
				  nm_vpn_service_get_name (service));

	/* Reap child if needed. */
	waitpid (pid, NULL, WNOHANG);
	priv->pid = 0;

	nm_vpn_service_connections_stop (service, TRUE);
}

static gboolean
nm_vpn_service_timeout (gpointer data)
{
	NMVPNService *service = NM_VPN_SERVICE (data);

	nm_info ("VPN service '%s' did not start in time, cancelling connections",
		    nm_vpn_service_get_name (service));

	NM_VPN_SERVICE_GET_PRIVATE (service)->service_start_timeout = 0;
	nm_vpn_service_connections_stop (service, TRUE);

	return FALSE;
}

static gboolean
nm_vpn_service_daemon_exec (gpointer user_data)
{
	NMVPNService *service = NM_VPN_SERVICE (user_data);
	NMVPNServicePrivate *priv = NM_VPN_SERVICE_GET_PRIVATE (service);
	GPtrArray *vpn_argv;
	gboolean launched;
	GError *err = NULL;

	vpn_argv = g_ptr_array_new ();
	g_ptr_array_add (vpn_argv, priv->program);
	g_ptr_array_add (vpn_argv, NULL);

	launched = g_spawn_async (NULL,
	                          (char **) vpn_argv->pdata,
	                          NULL,
	                          0,
	                          nm_vpn_service_child_setup,
	                          NULL,
	                          &priv->pid,
	                          &err);

	g_ptr_array_free (vpn_argv, TRUE);

	if (launched) {
		GSource *vpn_watch;

		vpn_watch = g_child_watch_source_new (priv->pid);
		g_source_set_callback (vpn_watch, (GSourceFunc) vpn_service_watch_cb, service, NULL);
		g_source_attach (vpn_watch, NULL);
		g_source_unref (vpn_watch);

		nm_info ("VPN service '%s' executed (%s), PID %d", 
			    nm_vpn_service_get_name (service),
			    priv->dbus_service,
			    priv->pid);

		priv->service_start_timeout = g_timeout_add (2000, nm_vpn_service_timeout, service);
	} else {
		nm_warning ("VPN service '%s': could not launch the VPN service. error: '%s'.",
		            nm_vpn_service_get_name (service), err->message);
		g_error_free (err);

		nm_vpn_service_connections_stop (service, TRUE);
	}

	return FALSE;
}

static gboolean
destroy_service (gpointer data)
{
	g_object_unref (data);

	return FALSE;
}

static void
connection_state_changed (NMVPNConnection *connection, NMVPNConnectionState state, gpointer user_data)
{
	NMVPNServicePrivate *priv;

	switch (state) {
	case NM_VPN_CONNECTION_STATE_FAILED:
	case NM_VPN_CONNECTION_STATE_DISCONNECTED:
		/* Remove the connection from our list */
		priv = NM_VPN_SERVICE_GET_PRIVATE (user_data);
		priv->connections = g_slist_remove (priv->connections, connection);
		g_object_unref (connection);

		if (priv->connections == NULL) {
			/* schedule a timeout (10 seconds) to destroy the service */
			g_timeout_add (10000, destroy_service, user_data);
		}
		break;
	default:
		break;
	}
}

NMVPNConnection *
nm_vpn_service_activate (NMVPNService *service,
					NMConnection *connection,
					NMDevice *device)
{
	NMVPNConnection *vpn_connection;
	NMVPNServicePrivate *priv;

	g_return_val_if_fail (NM_IS_VPN_SERVICE (service), NULL);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);
	g_return_val_if_fail (NM_IS_DEVICE (device), NULL);

	priv = NM_VPN_SERVICE_GET_PRIVATE (service);

	vpn_connection = nm_vpn_connection_new (connection, device);
	g_signal_connect (vpn_connection, "state-changed",
				   G_CALLBACK (connection_state_changed),
				   service);

	priv->connections = g_slist_prepend (priv->connections, vpn_connection);

	if (nm_dbus_manager_name_has_owner (priv->dbus_mgr, priv->dbus_service))
		nm_vpn_connection_activate (vpn_connection);
	else if (priv->service_start_timeout == 0) {
		nm_info ("VPN service '%s' exec scheduled...", nm_vpn_service_get_name (service));
		g_idle_add (nm_vpn_service_daemon_exec, service);
	}

	return vpn_connection;
}

GSList *
nm_vpn_service_get_connections (NMVPNService *service)
{
	g_return_val_if_fail (NM_IS_VPN_SERVICE (service), NULL);

	return g_slist_copy (NM_VPN_SERVICE_GET_PRIVATE (service)->connections);
}

static void
nm_vpn_service_name_owner_changed (NMDBusManager *mgr,
							const char *name,
							const char *old,
							const char *new,
							gpointer user_data)
{
	NMVPNService *service = NM_VPN_SERVICE (user_data);
	NMVPNServicePrivate *priv = NM_VPN_SERVICE_GET_PRIVATE (service);
	gboolean old_owner_good;
	gboolean new_owner_good;

	if (strcmp (name, priv->dbus_service))
		return;

	/* Service changed, no need to wait for the timeout any longer */
	if (priv->service_start_timeout) {
		g_source_remove (priv->service_start_timeout);
		priv->service_start_timeout = 0;
	}

	old_owner_good = (old && (strlen (old) > 0));
	new_owner_good = (new && (strlen (new) > 0));

	if (!old_owner_good && new_owner_good) {
		/* service just appeared */
		GSList *iter;

		nm_info ("VPN service '%s' just appeared, activating connections",
			    nm_vpn_service_get_name (service));

		for (iter = priv->connections; iter; iter = iter->next)
			nm_vpn_connection_activate (NM_VPN_CONNECTION (iter->data));

	} else if (old_owner_good && !new_owner_good) {
		/* service went away */
		nm_info ("VPN service '%s' disappeared, cancelling connections",
			    nm_vpn_service_get_name (service));
		nm_vpn_service_connections_stop (service, TRUE);
	}
}

/******************************************************************************/

static void
nm_vpn_service_init (NMVPNService *service)
{
	NMVPNServicePrivate *priv = NM_VPN_SERVICE_GET_PRIVATE (service);

	priv->dbus_mgr = nm_dbus_manager_get ();
	
	priv->name_owner_id = g_signal_connect (priv->dbus_mgr, "name-owner-changed",
									G_CALLBACK (nm_vpn_service_name_owner_changed),
									service);
}

static void
finalize (GObject *object)
{
	NMVPNServicePrivate *priv = NM_VPN_SERVICE_GET_PRIVATE (object);

	if (priv->service_start_timeout)
		g_source_remove (priv->service_start_timeout);

	nm_vpn_service_connections_stop (NM_VPN_SERVICE (object), FALSE);

	g_signal_handler_disconnect (priv->dbus_mgr, priv->name_owner_id);
	g_object_unref (priv->dbus_mgr);

	g_free (priv->name);
	g_free (priv->dbus_service);
	g_free (priv->program);

	G_OBJECT_CLASS (nm_vpn_service_parent_class)->finalize (object);
}

static void
nm_vpn_service_class_init (NMVPNServiceClass *service_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (service_class);

	g_type_class_add_private (service_class, sizeof (NMVPNServicePrivate));

	/* virtual methods */
	object_class->finalize = finalize;
}
