#include <stdio.h>
#include <stdlib.h>
#include <dbus/dbus-glib.h>

int main(int argc, char *argv[])
{
	DBusGConnection *connection;
	GError *error;
	DBusGProxy *proxy;
	char **name_list;
	char **name_list_ptr;

	g_type_init();

	error = NULL;
#if 0
	connection = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
#else
	connection = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
#endif

	if (!connection) {
		g_printerr("Failed to open connection to bus: %s\n", error->message);
		g_error_free(error);
		exit(1);
	}

	proxy = dbus_g_proxy_new_for_name(connection, 
					  DBUS_SERVICE_DBUS,
					  DBUS_PATH_DBUS,
					  DBUS_INTERFACE_DBUS);

	error = NULL;
	if (!dbus_g_proxy_call(proxy, "ListNames", &error, 
				G_TYPE_INVALID, G_TYPE_STRV, 
				&name_list, G_TYPE_INVALID)) {
		if (error->domain == DBUS_GERROR && 
		    error->code == DBUS_GERROR_REMOTE_EXCEPTION) {
			g_printerr("Caught remote method exception %s: %s\n", 
					dbus_g_error_get_name(error), error->message);
		} else {
			g_printerr("Error: %s\n", error->message);
		}
		g_error_free(error);
		exit(1);
	}

	g_print("Names on the message bus: \n");

	for (name_list_ptr = name_list; *name_list_ptr; name_list_ptr++) {
		g_print("	%s\n", *name_list_ptr);
	}

	g_strfreev(name_list);
	g_object_unref(proxy);

	exit(0);
}


