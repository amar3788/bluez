/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Instituto Nokia de Tecnologia - INdT
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

#include <stdint.h>
#include <errno.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "adapter.h"
#include "device.h"
#include "lib/uuid.h"
#include "dbus-common.h"
#include "log.h"

#include "error.h"
#include "gatt.h"
#include "gatt-dbus.h"

#define GATT_MGR_IFACE			"org.bluez.GattManager1"
#define GATT_SERVICE_IFACE		"org.bluez.GattService1"
#define GATT_CHR_IFACE			"org.bluez.GattCharacteristic1"
#define GATT_DESCRIPTOR_IFACE		"org.bluez.GattDescriptor1"

struct external_app {
	char *owner;
	char *path;
	DBusMessage *reg;
	GDBusClient *client;
	GSList *proxies;
	unsigned int watch;
};

/*
 * Attribute to Proxy hash table. Used to map incoming
 * ATT operations to its external characteristic proxy.
 */
static GHashTable *proxy_hash;

static GSList *external_apps;

static int external_app_path_cmp(gconstpointer a, gconstpointer b)
{
	const struct external_app *eapp = a;
	const char *path = b;

	return g_strcmp0(eapp->path, path);
}

static void external_app_watch_destroy(gpointer user_data)
{
	struct external_app *eapp = user_data;

	/* TODO: Remove from the database */

	external_apps = g_slist_remove(external_apps, eapp);

	g_dbus_client_unref(eapp->client);
	if (eapp->reg)
		dbus_message_unref(eapp->reg);

	g_free(eapp->owner);
	g_free(eapp->path);
	g_free(eapp);
}

static int proxy_path_cmp(gconstpointer a, gconstpointer b)
{
	GDBusProxy *proxy1 = (GDBusProxy *) a;
	GDBusProxy *proxy2 = (GDBusProxy *) b;
	const char *path1 = g_dbus_proxy_get_path(proxy1);
	const char *path2 = g_dbus_proxy_get_path(proxy2);

	return g_strcmp0(path1, path2);
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	struct external_app *eapp = user_data;
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	if (!g_str_has_prefix(path, eapp->path))
		return;

	if (g_strcmp0(interface, GATT_CHR_IFACE) != 0 &&
			g_strcmp0(interface, GATT_SERVICE_IFACE) != 0 &&
			g_strcmp0(interface, GATT_DESCRIPTOR_IFACE) != 0)
		return;

	DBG("path %s iface %s", path, interface);

	/*
	 * Object path follows a hierarchical organization. Add the
	 * proxies sorted by path helps the logic to register the
	 * object path later.
	 */
	eapp->proxies = g_slist_insert_sorted(eapp->proxies, proxy,
							proxy_path_cmp);
}

static void proxy_removed(GDBusProxy *proxy, void *user_data)
{
	struct external_app *eapp = user_data;
	const char *interface, *path;

	interface = g_dbus_proxy_get_interface(proxy);
	path = g_dbus_proxy_get_path(proxy);

	DBG("path %s iface %s", path, interface);

	eapp->proxies = g_slist_remove(eapp->proxies, proxy);
}

static int register_external_service(const struct external_app *eapp,
							GDBusProxy *proxy)
{
	DBusMessageIter iter;
	const char *str, *path, *iface;
	bt_uuid_t uuid;

	path = g_dbus_proxy_get_path(proxy);
	iface = g_dbus_proxy_get_interface(proxy);
	if (g_strcmp0(eapp->path, path) != 0 ||
			g_strcmp0(iface, GATT_SERVICE_IFACE) != 0)
		return -EINVAL;

	if (!g_dbus_proxy_get_property(proxy, "UUID", &iter))
		return -EINVAL;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return -EINVAL;

	dbus_message_iter_get_basic(&iter, &str);

	if (bt_string_to_uuid(&uuid, str) < 0)
		return -EINVAL;

	if (btd_gatt_add_service(&uuid) == NULL)
		return -EINVAL;

	return 0;
}

static int register_external_characteristics(GSList *proxies)

{
	GSList *list;

	for (list = proxies; list; list = g_slist_next(proxies)) {
		DBusMessageIter iter;
		const char *str, *path;
		bt_uuid_t uuid;
		struct btd_attribute *attr;
		GDBusProxy *proxy = list->data;

		if (!g_dbus_proxy_get_property(proxy, "UUID", &iter))
			return -EINVAL;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
			return -EINVAL;

		dbus_message_iter_get_basic(&iter, &str);

		if (bt_string_to_uuid(&uuid, str) < 0)
			return -EINVAL;

		/*
		 * TODO: Missing Flags/property
		 * Add properties according to Core SPEC 4.1 page 2183.
		 * Reference table 3.5: Characteristic Properties bit field.
		 */

		attr = btd_gatt_add_char(&uuid, 0x00, NULL);
		if (attr == NULL)
			return -EINVAL;

		path = g_dbus_proxy_get_path(proxy);
		DBG("Added GATT CHR: %s (%s)", path, str);

		g_hash_table_insert(proxy_hash, attr, g_dbus_proxy_ref(proxy));
	}

	return 0;
}

static void client_ready(GDBusClient *client, void *user_data)
{
	struct external_app *eapp = user_data;
	GDBusProxy *proxy;
	DBusConnection *conn = btd_get_dbus_connection();
	DBusMessage *reply;

	if (eapp->proxies == NULL)
		goto fail;

	proxy = eapp->proxies->data;
	if (register_external_service(eapp, proxy) < 0)
		goto fail;

	if (register_external_characteristics(g_slist_next(eapp->proxies)) < 0)
		goto fail;

	DBG("Added GATT service %s", eapp->path);

	reply = dbus_message_new_method_return(eapp->reg);
	goto reply;

fail:
	error("Could not register external service: %s", eapp->path);

	reply = btd_error_invalid_args(eapp->reg);
	/* TODO: missing eapp/database cleanup */

reply:
	dbus_message_unref(eapp->reg);
	eapp->reg = NULL;

	g_dbus_send_message(conn, reply);
}

static struct external_app *new_external_app(DBusConnection *conn,
					DBusMessage *msg, const char *path)
{
	struct external_app *eapp;
	GDBusClient *client;
	const char *sender = dbus_message_get_sender(msg);

	client = g_dbus_client_new(conn, sender, "/");
	if (client == NULL)
		return NULL;

	eapp = g_new0(struct external_app, 1);

	eapp->watch = g_dbus_add_disconnect_watch(btd_get_dbus_connection(),
			sender, NULL, eapp, external_app_watch_destroy);
	if (eapp->watch == 0) {
		g_dbus_client_unref(client);
		g_free(eapp);
		return NULL;
	}

	eapp->owner = g_strdup(sender);
	eapp->reg = dbus_message_ref(msg);
	eapp->client = client;
	eapp->path = g_strdup(path);

	g_dbus_client_set_proxy_handlers(client, proxy_added, proxy_removed,
								NULL, eapp);

	g_dbus_client_set_ready_watch(client, client_ready, eapp);

	return eapp;
}

static DBusMessage *register_service(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct external_app *eapp;
	DBusMessageIter iter;
	const char *path;

	if (!dbus_message_iter_init(msg, &iter))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &path);

	if (g_slist_find_custom(external_apps, path, external_app_path_cmp))
		return btd_error_already_exists(msg);

	eapp = new_external_app(conn, msg, path);
	if (eapp == NULL)
		return btd_error_failed(msg, "Not enough resources");

	external_apps = g_slist_prepend(external_apps, eapp);

	DBG("New app %p: %s", eapp, path);

	return NULL;
}

static DBusMessage *unregister_service(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable methods[] = {
	{ GDBUS_EXPERIMENTAL_ASYNC_METHOD("RegisterService",
				GDBUS_ARGS({ "service", "o"},
						{ "options", "a{sv}"}),
				NULL, register_service) },
	{ GDBUS_EXPERIMENTAL_METHOD("UnregisterService",
				GDBUS_ARGS({"service", "o"}),
				NULL, unregister_service) },
	{ }
};

gboolean gatt_dbus_manager_register(void)
{
	if (g_dbus_register_interface(btd_get_dbus_connection(),
				"/org/bluez", GATT_MGR_IFACE,
				methods, NULL, NULL, NULL, NULL) == FALSE)
		return FALSE;

	proxy_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal,
				NULL, (GDestroyNotify) g_dbus_proxy_unref);

	return TRUE;
}

void gatt_dbus_manager_unregister(void)
{
	g_hash_table_destroy(proxy_hash);
	proxy_hash = NULL;

	g_dbus_unregister_interface(btd_get_dbus_connection(), "/org/bluez",
							GATT_MGR_IFACE);
}
