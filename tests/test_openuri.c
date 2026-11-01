/* Opening a web address (src/shared/openuri.c) on a private D-Bus with a
 * fake portal: no real portal or browser is ever involved.
 *
 * Checked: with no portal, the browser is started directly; with one,
 * the address reaches it exactly as given (spaces, &, #, unicode) and no
 * browser is started here; a portal that refuses falls back to starting
 * the browser; when that fails too, the error says why. Hidden: random
 * addresses reach the portal unchanged. */
#include <string.h>
#include "cases.h"
#include "openuri.h"

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"

static const char XML[] =
  "<node><interface name='org.freedesktop.portal.OpenURI'><method name='OpenURI'>"
  "<arg type='s' name='parent_window' direction='in'/><arg type='s' name='uri' direction='in'/>"
  "<arg type='a{sv}' name='options' direction='in'/><arg type='o' name='handle' direction='out'/>"
  "</method></interface></node>";

/* ---- the fake portal, on its own thread ------------------------------------------ */

static GMutex lock;
static GCond cond;
static gboolean portal_ready, portal_refuses;
static char *portal_got;
static int portal_calls;
static GMainLoop *portal_loop;

static void
on_call (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
         GVariant *params, GDBusMethodInvocation *inv, gpointer user)
{
  (void) c; (void) sender; (void) path; (void) iface; (void) method; (void) user;
  const char *parent, *uri;
  g_autoptr (GVariant) options = NULL;
  g_variant_get (params, "(&s&s@a{sv})", &parent, &uri, &options);
  g_mutex_lock (&lock);
  portal_calls++;
  g_free (portal_got);
  portal_got = g_strdup (uri);
  gboolean refuse = portal_refuses;
  g_mutex_unlock (&lock);
  if (refuse)
    g_dbus_method_invocation_return_dbus_error (inv, "org.freedesktop.portal.Error.NotAllowed", "Not allowed");
  else
    g_dbus_method_invocation_return_value (inv, g_variant_new ("(o)", PORTAL_PATH "/request/1_1/t1"));
}

static void
on_owned (GDBusConnection *c, const char *name, gpointer user)
{
  (void) c; (void) name; (void) user;
  g_mutex_lock (&lock);
  portal_ready = TRUE;
  g_cond_signal (&cond);
  g_mutex_unlock (&lock);
}

static gpointer
portal_thread (gpointer data)
{
  const char *address = data;
  GMainContext *ctx = g_main_context_new ();
  g_main_context_push_thread_default (ctx);
  GDBusConnection *bus = g_dbus_connection_new_for_address_sync (
    address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL,
    NULL, NULL);
  g_assert_nonnull (bus);
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml (XML, NULL);
  static const GDBusInterfaceVTable vtable = { on_call, NULL, NULL, { 0 } };
  guint obj = g_dbus_connection_register_object (bus, PORTAL_PATH, info->interfaces[0], &vtable, NULL, NULL, NULL);
  guint own = g_bus_own_name_on_connection (bus, PORTAL_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_owned, NULL, NULL, NULL);
  g_mutex_lock (&lock);
  portal_loop = g_main_loop_new (ctx, FALSE);
  g_mutex_unlock (&lock);
  g_main_loop_run (portal_loop);
  g_bus_unown_name (own);
  g_dbus_connection_unregister_object (bus, obj);
  g_dbus_node_info_unref (info);
  g_dbus_connection_close_sync (bus, NULL, NULL);
  g_object_unref (bus);
  while (g_main_context_iteration (ctx, FALSE))
    ;
  g_main_context_pop_thread_default (ctx);
  g_main_context_unref (ctx);
  return NULL;
}

/* ---- the browser, never a real one ------------------------------------------------- */

static int launches;
static char *launched;
static gboolean no_browser;

static gboolean
fake_launch (const char *uri, GError **error)
{
  launches++;
  g_free (launched);
  launched = g_strdup (uri);
  if (no_browser)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No application is registered as handling this file");
      return FALSE;
    }
  return TRUE;
}

static void
reset (void)
{
  g_mutex_lock (&lock);
  portal_calls = 0;
  g_clear_pointer (&portal_got, g_free);
  g_mutex_unlock (&lock);
  launches = 0;
  g_clear_pointer (&launched, g_free);
}

static const char *const URIS[] = {
  "https://accounts.google.com/o/oauth2/v2/auth?client_id=1.apps&redirect_uri=http%3A%2F%2F127.0.0.1%3A41234%2F",
  "https://github.com/biswas2200/my-journal/issues/new?template=bug_report.yml&title=%5BNotes%5D%20",
  "https://example.org/a b?x=1&y=2#part",
  "https://example.org/caf\xc3\xa9/\xe0\xa4\xa8\xe0\xa5\x8b\xe0\xa4\x9f",
};

static void
test_open (gconstpointer data)
{
  const char *address = data;
  JrCases c = { "open uri", 0, 0 };

  /* No portal on this bus yet: the browser directly. */
  reset ();
  g_autoptr (GError) error = NULL;
  gboolean ok = jr_open_uri_with (URIS[0], fake_launch, &error);
  jr_case (&c, "no portal: the browser directly", ok && launches == 1 && g_strcmp0 (launched, URIS[0]) == 0,
           "ok=%d launches=%d", ok, launches);
  no_browser = TRUE;
  reset ();
  g_clear_error (&error);
  ok = jr_open_uri_with (URIS[0], fake_launch, &error);
  jr_case (&c, "no portal, no browser: says why", !ok && error != NULL && strstr (error->message, "No application") != NULL,
           "ok=%d %s", ok, error != NULL ? error->message : "(no error)");
  no_browser = FALSE;

  GThread *t = g_thread_new ("portal", portal_thread, (gpointer) address);
  g_mutex_lock (&lock);
  while (!portal_ready)
    g_cond_wait (&cond, &lock);
  g_mutex_unlock (&lock);

  for (gsize i = 0; i < G_N_ELEMENTS (URIS); i++)
    {
      reset ();
      g_clear_error (&error);
      ok = jr_open_uri_with (URIS[i], fake_launch, &error);
      g_mutex_lock (&lock);
      jr_case (&c, "the portal takes it, as given", ok && portal_calls == 1 && g_strcmp0 (portal_got, URIS[i]) == 0 &&
               launches == 0, "Input %s  Got %s (ok=%d launches=%d)", URIS[i], portal_got, ok, launches);
      g_mutex_unlock (&lock);
    }

  portal_refuses = TRUE;
  reset ();
  g_clear_error (&error);
  ok = jr_open_uri_with (URIS[1], fake_launch, &error);
  jr_case (&c, "the portal refuses: the browser directly", ok && portal_calls == 1 && launches == 1 &&
           g_strcmp0 (launched, URIS[1]) == 0, "ok=%d calls=%d launches=%d", ok, portal_calls, launches);
  no_browser = TRUE;
  reset ();
  g_clear_error (&error);
  ok = jr_open_uri_with (URIS[1], fake_launch, &error);
  jr_case (&c, "refused, no browser: says why", !ok && error != NULL, "ok=%d", ok);
  no_browser = FALSE;
  portal_refuses = FALSE;

  /* Hidden: random addresses arrive unchanged. */
  JrCases h = { "open uri (hidden)", 0, 0 };
  for (int i = 0; i < 200; i++)
    {
      GString *build = g_string_new ("https://example.org/");
      for (int n = g_test_rand_int_range (0, 80); n > 0; n--)
        {
          static const char *const bits[] = { "a", "Z", "9", "%20", "&", "=", "?", "#", "/", "-", "_", "~", " ",
                                              "\xc3\xa9", "\xe2\x98\x81", "\xf0\x9f\x93\x9d" };
          g_string_append (build, bits[g_test_rand_int_range (0, G_N_ELEMENTS (bits))]);
        }
      g_autofree char *uri = g_string_free (build, FALSE);
      reset ();
      ok = jr_open_uri_with (uri, fake_launch, NULL);
      g_mutex_lock (&lock);
      jr_case (&h, "random address", ok && g_strcmp0 (portal_got, uri) == 0 && launches == 0, "Input %s  Got %s", uri,
               portal_got);
      g_mutex_unlock (&lock);
    }
  jr_cases_done (&h);

  g_mutex_lock (&lock);
  g_main_loop_quit (portal_loop);
  g_mutex_unlock (&lock);
  g_thread_join (t);
  g_main_loop_unref (portal_loop);
  reset ();
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_dbus_unset ();
  GTestDBus *bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
  g_autofree char *address = g_strdup (g_test_dbus_get_bus_address (bus));
  g_test_add_data_func ("/openuri/portal-first", address, test_open);
  int rc = g_test_run ();
  g_test_dbus_down (bus);
  g_object_unref (bus);
  return rc;
}
