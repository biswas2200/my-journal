/* openuri: see openuri.h. */
#include "openuri.h"

#define PORTAL_NAME  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.OpenURI"
#define PORTAL_WAIT_MS 5000

/* The portal: TRUE once it has the address (it answers with a request
 * handle at once and starts the browser itself). */
static gboolean
by_portal (const char *uri, GError **error)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (bus == NULL)
    return FALSE;
  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE, "OpenURI",
                                                            g_variant_new ("(ssa{sv})", "", uri, &options),
                                                            G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                                            PORTAL_WAIT_MS, NULL, error);
  return reply != NULL;
}

static gboolean
directly (const char *uri, GError **error)
{
  return g_app_info_launch_default_for_uri (uri, NULL, error);
}

gboolean
jr_open_uri_with (const char *uri, JrLaunch launch, GError **error)
{
  g_autoptr (GError) portal_error = NULL;
  if (by_portal (uri, &portal_error))
    return TRUE;
  g_debug ("No portal to open the address (%s); starting the browser directly", portal_error->message);
  return launch (uri, error);
}

gboolean
jr_open_uri (const char *uri, GError **error)
{
  return jr_open_uri_with (uri, directly, error);
}
