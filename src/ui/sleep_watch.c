/* sleep_watch: see sleep_watch.h. */
#include "sleep_watch.h"

#include <unistd.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define LOGIND_NAME "org.freedesktop.login1"
#define LOGIND_PATH "/org/freedesktop/login1"
#define LOGIND_IFACE "org.freedesktop.login1.Manager"

struct JrSleepWatch {
  GDBusConnection *bus;
  GCancellable    *cancel;
  guint            signal_id;
  int              inhibit_fd; /* -1 when not holding the inhibitor */
  gboolean         enabled;
  gboolean         inhibit_pending;
  JrSleepFunc      before_sleep;
  gpointer         user;
};

static void
release_inhibitor (JrSleepWatch *w)
{
  if (w->inhibit_fd >= 0)
    {
      close (w->inhibit_fd); /* closing the fd lets the system sleep */
      w->inhibit_fd = -1;
    }
}

static void
on_inhibit_reply (GObject *source, GAsyncResult *res, gpointer data)
{
  GUnixFDList *fds = NULL;
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source),
                                                                      &fds, res, &error);
  if (reply == NULL)
    {
      /* Cancelled means `data` is already freed: do not touch it. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          JrSleepWatch *w = data;
          w->inhibit_pending = FALSE;
          g_debug ("No sleep inhibitor: %s", error->message);
        }
      g_error_free (error);
      return;
    }

  JrSleepWatch *w = data;
  w->inhibit_pending = FALSE;
  gint32 index = -1;
  g_variant_get (reply, "(h)", &index);
  int fd = g_unix_fd_list_get (fds, index, NULL); /* a dup we own */
  g_variant_unref (reply);
  g_object_unref (fds);

  if (fd < 0)
    return;
  if (!w->enabled || w->inhibit_fd >= 0)
    close (fd); /* turned off meanwhile, or already held */
  else
    w->inhibit_fd = fd;
}

static void
take_inhibitor (JrSleepWatch *w)
{
  if (w->bus == NULL || w->inhibit_fd >= 0 || w->inhibit_pending)
    return;
  w->inhibit_pending = TRUE;
  g_dbus_connection_call_with_unix_fd_list (
    w->bus, LOGIND_NAME, LOGIND_PATH, LOGIND_IFACE, "Inhibit",
    g_variant_new ("(ssss)", "sleep", "Journal", "Lock the journal before sleep", "delay"),
    G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, w->cancel,
    on_inhibit_reply, w);
}

static void
on_prepare_for_sleep (GDBusConnection *bus, const char *sender, const char *path,
                      const char *iface, const char *signal, GVariant *params, gpointer data)
{
  (void) bus; (void) sender; (void) path; (void) iface; (void) signal;
  JrSleepWatch *w = data;
  gboolean going_to_sleep = FALSE;
  g_variant_get (params, "(b)", &going_to_sleep);

  if (!w->enabled)
    return;
  if (going_to_sleep)
    {
      w->before_sleep (w->user);
      release_inhibitor (w);
    }
  else
    take_inhibitor (w); /* resumed: be ready for the next sleep */
}

static void
on_bus_ready (GObject *source, GAsyncResult *res, gpointer data)
{
  (void) source;
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_finish (res, &error);
  if (bus == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("No system bus, cannot lock on sleep: %s", error->message);
      g_error_free (error);
      return;
    }

  JrSleepWatch *w = data;
  w->bus = bus;
  w->signal_id = g_dbus_connection_signal_subscribe (
    bus, LOGIND_NAME, LOGIND_IFACE, "PrepareForSleep", LOGIND_PATH, NULL,
    G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, w, NULL);
  if (w->enabled)
    take_inhibitor (w);
}

JrSleepWatch *
jr_sleep_watch_new (JrSleepFunc before_sleep, gpointer user)
{
  JrSleepWatch *w = g_new0 (JrSleepWatch, 1);
  w->inhibit_fd = -1;
  w->before_sleep = before_sleep;
  w->user = user;
  w->cancel = g_cancellable_new ();
  g_bus_get (G_BUS_TYPE_SYSTEM, w->cancel, on_bus_ready, w);
  return w;
}

void
jr_sleep_watch_set_enabled (JrSleepWatch *w, gboolean enabled)
{
  w->enabled = enabled;
  if (enabled)
    take_inhibitor (w);
  else
    release_inhibitor (w);
}

void
jr_sleep_watch_free (JrSleepWatch *w)
{
  if (w == NULL)
    return;
  g_cancellable_cancel (w->cancel);
  g_object_unref (w->cancel);
  if (w->bus != NULL)
    {
      g_dbus_connection_signal_unsubscribe (w->bus, w->signal_id);
      g_object_unref (w->bus);
    }
  release_inhibitor (w);
  g_free (w);
}
