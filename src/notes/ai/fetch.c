/* fetch: see fetch.h. */
#include "fetch.h"

#include <string.h>
#include "netguard.h"
#include "textify.h"

typedef struct {
  gboolean      allow_private; /* the connection check is off */
  gboolean      refused;
  GCancellable *cancel;
} Guard;

/* Every connection, also after a redirect: once connected and before any
 * byte is sent, a private address ends it. */
static void
on_network_event (SoupMessage *msg, GSocketClientEvent event, GIOStream *connection, gpointer data)
{
  (void) msg;
  Guard *g = data;
  if (event != G_SOCKET_CLIENT_CONNECTED || g->allow_private)
    return;
  gboolean socket = G_IS_SOCKET_CONNECTION (connection);
  if (!socket)
    return;
  g_autoptr (GSocketAddress) remote = g_socket_connection_get_remote_address (G_SOCKET_CONNECTION (connection), NULL);
  gboolean public = FALSE;
  if (remote != NULL)
    {
      gboolean inet = G_IS_INET_SOCKET_ADDRESS (remote);
      if (inet)
        public = jn_address_public (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (remote)));
    }
  if (!public)
    {
      g->refused = TRUE;
      g_cancellable_cancel (g->cancel);
    }
}

/* Before connecting: a host that resolves to any private address is
 * refused (a public name pointing inside the home network). */
static char *
host_problem (const char *url, gboolean allow_private)
{
  g_autoptr (GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  const char *host = uri != NULL ? g_uri_get_host (uri) : NULL;
  if (host == NULL)
    return g_strdup ("this is not a link");
  g_autoptr (GError) error = NULL;
  GList *addrs = g_resolver_lookup_by_name (g_resolver_get_default (), host, NULL, &error);
  if (addrs == NULL)
    return g_strdup_printf ("the website %s could not be found", host);
  gboolean private = FALSE;
  for (GList *a = addrs; a != NULL; a = a->next)
    private |= !jn_address_public (a->data);
  g_resolver_free_addresses (addrs);
  if (private && !allow_private)
    return g_strdup ("it points inside this computer or its network, which is never fetched");
  return NULL;
}

void
jn_fetch_url (SoupSession *session, const char *url, gboolean check_host, gboolean check_connection, JnAiSource *out)
{
  gboolean allow_private = !check_connection;
  char *problem = host_problem (url, !check_host);
  if (problem != NULL)
    {
      out->problem = problem;
      return;
    }
  SoupMessage *msg = soup_message_new ("GET", url);
  if (msg == NULL)
    {
      out->problem = g_strdup ("this is not a link");
      return;
    }
  soup_message_headers_replace (soup_message_get_request_headers (msg), "Accept", "text/html, text/plain;q=0.9, */*;q=0.1");
  Guard guard = { allow_private, FALSE, g_cancellable_new () };
  g_signal_connect (msg, "network-event", G_CALLBACK (on_network_event), &guard);
  g_autoptr (GError) error = NULL;
  GInputStream *in = soup_session_send (session, msg, guard.cancel, &error);
  guint status = soup_message_get_status (msg);
  if (guard.refused)
    out->problem = g_strdup ("it led inside this computer or its network, which is never fetched");
  else if (in == NULL)
    out->problem = g_strdup_printf ("the page could not be fetched (%s)", error != NULL ? error->message : "no answer");
  else if (status != SOUP_STATUS_OK)
    out->problem = g_strdup_printf ("the page could not be fetched (%u)", status);
  else
    {
      const char *type = soup_message_headers_get_content_type (soup_message_get_response_headers (msg), NULL);
      gboolean html = TRUE, plain = FALSE;
      if (type != NULL)
        {
          gboolean text_html = g_str_has_prefix (type, "text/html");
          gboolean xhtml = g_str_has_prefix (type, "application/xhtml");
          html = text_html || xhtml;
          plain = g_str_has_prefix (type, "text/plain");
        }
      if (!html && !plain)
        out->problem = g_strdup_printf ("this kind of file (%s) is not read yet", type);
      else
        {
          GByteArray *data = g_byte_array_new ();
          guint8 buf[16384];
          while (data->len < JN_FETCH_MAX_BYTES)
            {
              gssize n = g_input_stream_read (in, buf, MIN (sizeof buf, JN_FETCH_MAX_BYTES - data->len), guard.cancel,
                                              NULL);
              if (n <= 0)
                break;
              g_byte_array_append (data, buf, (guint) n);
            }
          if (guard.refused)
            out->problem = g_strdup ("it led inside this computer or its network, which is never fetched");
          else if (html)
            out->text = jn_textify ((const char *) data->data, data->len, JN_FETCH_MAX_TEXT);
          else
            {
              g_autofree char *valid = g_utf8_make_valid ((const char *) data->data, (gssize) data->len);
              out->text = jn_textify (valid, strlen (valid), JN_FETCH_MAX_TEXT); /* plain text has no tags */
            }
          g_byte_array_unref (data);
          if (out->text != NULL && *out->text == '\0')
            {
              g_clear_pointer (&out->text, g_free);
              out->problem = g_strdup ("the page has no text (it may need a browser to show it)");
            }
        }
    }
  g_clear_object (&in);
  g_object_unref (guard.cancel);
  g_object_unref (msg);
}

JnAiSource *
jn_fetch_sources (SoupSession *session, char **links, gboolean allow_private, gsize *n_sources)
{
  JnAiSource *out = g_new0 (JnAiSource, JN_FETCH_MAX_LINKS);
  gsize n = 0;
  for (char **l = links; l != NULL && *l != NULL && n < JN_FETCH_MAX_LINKS; l++)
    {
      g_autofree char *line = g_strstrip (g_strdup (*l));
      if (*line == '\0')
        continue;
      JnAiSource *s = &out[n++];
      g_autofree char *norm = NULL;
      JnUrlCheck check = jn_url_check (line, &norm);
      s->url = g_strdup (norm != NULL ? norm : line);
      if (check != JN_URL_OK)
        s->problem = g_strdup (jn_url_check_message (check));
      else
        jn_fetch_url (session, norm, !allow_private, !allow_private, s);
    }
  *n_sources = n;
  return out;
}

void
jn_fetch_free (JnAiSource *sources, gsize n_sources)
{
  for (gsize i = 0; i < n_sources; i++)
    {
      g_free (sources[i].url);
      g_free (sources[i].text);
      g_free (sources[i].problem);
    }
  g_free (sources);
}
