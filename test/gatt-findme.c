/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Instituto Nokia de Tecnologia - INdT
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "uuid.h"

#define SERVICE_INTERFACE "org.bluez.gatt.Service1"
#define CHARACTERISTIC_INTERFACE "org.bluez.gatt.Characteristic1"

static GMainLoop *main_loop;
static DBusConnection *dbus_conn;
static char *opt_src = NULL;
static char *opt_dst = NULL;

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	g_printerr("interface %s path %s\n", interface, path);

	if (g_str_equal(interface, "org.bluez.Adapter1")) {
		/* TODO: start discovery if "Discovering" property is false */
	} else if (g_str_equal(interface, "org.bluez.Device1")) {
		/* TODO: Check if:
		 *  - device address matches one given by -b
		 *  - device is connected (if not, call Device1.Connect().
		 */
		/* TODO: stop discovery when device is connected */
		/* TODO: create 1 second timer to check for:
		 * - Immediate Alert Service
		 * - Alert Level Characteristic of IAS
		 * - Write requested alert level to characteristic value
		 * - if not found, return error to user
		 */
	} else if (g_str_equal(interface, SERVICE_INTERFACE)) {
		/* TODO: collect list of services */
	} else if (g_str_equal(interface, CHARACTERISTIC_INTERFACE)) {
		/* TODO: collect list of characteristics */
	}
}

static GOptionEntry options[] = {
	{ "adapter", 'i', 0, G_OPTION_ARG_STRING, &opt_src,
				"Specify local adapter interface", "hciX" },
	{ "device", 'b', 0, G_OPTION_ARG_STRING, &opt_dst,
				"Specify remote Bluetooth address", "MAC" },
	/* TODO: add option for alert level, e.g. -l none|mild|high */
	{ NULL },
};

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GError *error = NULL;
	GDBusClient *client;
	int err = 0;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("%s\n", error->message);
		g_error_free(error);
		err = EXIT_FAILURE;
		goto done;
	}

	if (opt_dst == NULL) {
		g_printerr("Error: remote Bluetooth address not specified\n");
		err = EXIT_FAILURE;
		goto done;
	}

	main_loop = g_main_loop_new(NULL, FALSE);
	dbus_conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);

	client = g_dbus_client_new(dbus_conn, "org.bluez", "/org/bluez");
	if (client == NULL) {
		g_printerr("Could not create D-Bus client\n");
		err = EXIT_FAILURE;
		goto done;
	}

	g_dbus_client_set_proxy_handlers(client, proxy_added, NULL, NULL, NULL);

	g_main_loop_run(main_loop);

	g_dbus_client_unref(client);

	dbus_connection_unref(dbus_conn);
	g_main_loop_unref(main_loop);

done:
	g_option_context_free(context);
	g_free(opt_src);
	g_free(opt_dst);

	return err;
}
