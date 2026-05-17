#include <dbus/dbus.h>

static void log(DBusError &error) {
  fprintf(stderr, "DBus error: %s\n", error.name);
  fprintf(stderr, "Message: %s\n", error.message);
}

// Query the org.freedesktop.appearance / color-scheme setting via xdg-desktop-portal.
// Returns: 0 = no preference or portal unavailable, 1 = prefer dark, 2 = prefer light.
static unsigned xdgPortalColorScheme() {
    DBusError error;
    dbus_error_init(&error);

    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (conn == nullptr) {
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        return 0;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings",
        "Read");
    if (msg == nullptr) return 0;

    const char *ns  = "org.freedesktop.appearance";
    const char *key = "color-scheme";
    if (!dbus_message_append_args(msg,
            DBUS_TYPE_STRING, &ns,
            DBUS_TYPE_STRING, &key,
            DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return 0;
    }

    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
    dbus_message_unref(msg);

    if (reply == nullptr) {
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        return 0;
    }

    // The Read method returns `v`. xdg-desktop-portal stores values as
    // Variant<T>, so in practice the body is Variant<Variant<UInt32>>.
    // Peel one or two variant layers until we hit a UInt32.
    unsigned result = 0;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)
        && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter v1;
        dbus_message_iter_recurse(&iter, &v1);

        DBusMessageIter *valueIter = &v1;
        DBusMessageIter v2;
        if (dbus_message_iter_get_arg_type(&v1) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&v1, &v2);
            valueIter = &v2;
        }

        if (dbus_message_iter_get_arg_type(valueIter) == DBUS_TYPE_UINT32) {
            dbus_uint32_t value;
            dbus_message_iter_get_basic(valueIter, &value);
            result = (unsigned)value;
        }
    }

    dbus_message_unref(reply);
    return result;
}

void Application::App::inhibitScreenSaver() {
  DBusError error;
  dbus_error_init(&error);

  DBusConnection *connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
  if (connection == nullptr) {
    log(error);
    return;
  }

  DBusMessage *message = dbus_message_new_method_call(
      "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
      "org.freedesktop.ScreenSaver", "Inhibit");

  const char *app = "org.bsnes.bsnes-plus";
  const char *reason = "Playing a game";
  if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &app,
                                DBUS_TYPE_STRING, &reason, DBUS_TYPE_INVALID)) {
    dbus_connection_unref(connection);
    dbus_message_unref(message);
    fputs("Failed to append arguments to DBus call\n", stderr);
    return;
  }

  DBusMessage *reply = dbus_connection_send_with_reply_and_block(
      connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
  dbus_connection_unref(connection);
  dbus_message_unref(message);
  if (reply == nullptr) {
    log(error);
    return;
  }
  dbus_message_unref(reply);
}
