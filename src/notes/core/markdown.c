/* markdown: see markdown.h. */
#include "markdown.h"

#include <string.h>

#define ARROW "\xe2\x86\x92"
#define MAX_DEPTH 4
/* Finding where a mark closes looks ahead, so a long run of unclosed
 * marks costs the square of its length: a paragraph longer than this is
 * shown as plain text (no real one is; a page could ask the AI for one). */
#define MAX_SPAN_TEXT (16 * 1024)

static void
block_free (gpointer p)
{
  JnMdBlock *b = p;
  g_free (b->lang);
  g_free (b->text);
  g_free (b);
}

static void
add (GPtrArray *out, JnMdKind kind, int level, const char *lang, const char *text)
{
  if (kind == JN_MD_BLANK && (out->len == 0 || ((JnMdBlock *) out->pdata[out->len - 1])->kind == JN_MD_BLANK))
    return; /* no leading or repeated blank lines */
  JnMdBlock *b = g_new0 (JnMdBlock, 1);
  b->kind = kind;
  b->level = level;
  b->lang = lang != NULL && *lang != '\0' ? g_strdup (lang) : NULL;
  b->text = text != NULL ? g_strdup (text) : NULL;
  g_ptr_array_add (out, b);
}

static int
indent_of (const char *line, const char **rest)
{
  int n = 0;
  const char *p = line;
  for (; *p == ' ' || *p == '\t'; p++)
    n += *p == '\t' ? 4 : 1;
  *rest = p;
  return n;
}

static char *
rstrip_dup (const char *s)
{
  char *d = g_strdup (s);
  return g_strchomp (d);
}

static gboolean
is_rule (const char *t)
{
  char c = *t;
  if (c != '-' && c != '*' && c != '_')
    return FALSE;
  int n = 0;
  for (const char *p = t; *p != '\0'; p++)
    if (*p == c)
      n++;
    else if (*p != ' ')
      return FALSE;
  return n >= 3;
}

GPtrArray *
jn_md_blocks (const char *markdown)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (block_free);
  /* Broken UTF-8 (a damaged file) shows as U+FFFD, never read past. */
  g_autofree char *valid = g_utf8_make_valid (markdown != NULL ? markdown : "", -1);
  g_auto (GStrv) lines = g_strsplit (valid, "\n", -1);
  gboolean fence = FALSE;
  char fence_char = 0;
  int fence_indent = 0, fence_lines = 0;
  char *lang = NULL;
  GString *code = NULL;
  for (char **lp = lines; *lp != NULL; lp++)
    {
      char *line = *lp;
      gsize len = strlen (line);
      if (len > 0 && line[len - 1] == '\r')
        line[len - 1] = '\0';
      const char *t;
      int indent = indent_of (line, &t);
      if (fence)
        {
          /* Closes only as a fence alone on its line (spaces after it at
           * most); a line with more after the fence is code. */
          const char *after = t;
          while (*after == fence_char)
            after++;
          while (*after == ' ' || *after == '\t')
            after++;
          if (t[0] == fence_char && t[1] == fence_char && t[2] == fence_char && *after == '\0')
            {
              gboolean svg = lang != NULL && g_ascii_strcasecmp (lang, "svg") == 0;
              add (out, svg ? JN_MD_SVG : JN_MD_CODE, 0, svg ? NULL : lang, code->str);
              g_string_free (code, TRUE);
              g_clear_pointer (&lang, g_free);
              fence = FALSE;
              continue;
            }
          /* Remove up to the fence's own indent. */
          const char *body = line;
          for (int i = 0; i < fence_indent && (*body == ' ' || *body == '\t'); i++)
            body++;
          if (fence_lines++ > 0)
            g_string_append_c (code, '\n');
          g_string_append (code, body);
          continue;
        }
      if ((t[0] == '`' || t[0] == '~') && t[1] == t[0] && t[2] == t[0])
        {
          fence = TRUE;
          fence_char = t[0];
          fence_indent = indent;
          const char *info = t + 3;
          while (*info == fence_char)
            info++;
          g_autofree char *word = g_strstrip (g_strdup (info));
          code = g_string_new (NULL);
          fence_lines = 0;
          /* A language is one plain word (python, c++, svg); anything else
           * after the fence is kept as the first line of code, not lost. */
          gboolean plain = *word != '\0' && strlen (word) <= 24;
          for (const char *q = word; plain && *q != '\0'; q++)
            plain = g_ascii_isalnum (*q) || strchr ("+-#._", *q) != NULL;
          lang = plain ? g_strdup (word) : NULL;
          if (!plain && *word != '\0')
            {
              g_string_append (code, word);
              fence_lines = 1;
            }
          continue;
        }
      if (*t == '\0')
        {
          add (out, JN_MD_BLANK, 0, NULL, NULL);
          continue;
        }
      if (is_rule (t))
        {
          add (out, JN_MD_RULE, 0, NULL, NULL);
          continue;
        }
      if (line[0] == '\\' && line[1] != '\0' && strchr ("#-*+>`~_\\0123456789", line[1]) != NULL)
        {
          g_autofree char *p = rstrip_dup (line + 1);
          add (out, JN_MD_PARAGRAPH, 0, NULL, p);
          continue;
        }
      gboolean escaped_arrow = line[0] == '\\' && strncmp (line + 1, ARROW, strlen (ARROW)) == 0;
      if (escaped_arrow)
        {
          g_autofree char *p = rstrip_dup (line + 1); /* an escaped arrow */
          add (out, JN_MD_PARAGRAPH, 0, NULL, p);
          continue;
        }
      int hashes = 0;
      while (line[hashes] == '#')
        hashes++;
      if (hashes >= 1 && hashes <= 6 && line[hashes] == ' ')
        {
          g_autofree char *h = g_strstrip (g_strdup (line + hashes + 1));
          add (out, JN_MD_HEADING, hashes, NULL, h);
          continue;
        }
      int depth = MIN (indent / 2, MAX_DEPTH);
      if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ')
        {
          g_autofree char *x = g_strstrip (g_strdup (t + 2));
          add (out, JN_MD_BULLET, depth, NULL, x);
          continue;
        }
      gboolean arrow = strncmp (t, ARROW " ", strlen (ARROW " ")) == 0;
      gboolean typed_arrow = strncmp (t, "-> ", 3) == 0;
      if (arrow || typed_arrow)
        {
          g_autofree char *x = g_strstrip (g_strdup (t + (t[0] == '-' ? 3 : strlen (ARROW " "))));
          add (out, JN_MD_ARROW, depth, NULL, x);
          continue;
        }
      int digits = 0;
      while (g_ascii_isdigit (t[digits]) && digits < 9)
        digits++;
      if (digits > 0 && (t[digits] == '.' || t[digits] == ')') && t[digits + 1] == ' ')
        {
          g_autofree char *x = g_strstrip (g_strdup (t + digits + 2));
          add (out, JN_MD_NUMBERED, (int) g_ascii_strtoll (t, NULL, 10), NULL, x);
          continue;
        }
      g_autofree char *x = g_strstrip (g_strdup (t));
      add (out, JN_MD_PARAGRAPH, 0, NULL, x);
    }
  if (fence)
    {
      add (out, JN_MD_CODE, 0, lang, code->str); /* unclosed: code to the end */
      g_string_free (code, TRUE);
      g_free (lang);
    }
  while (out->len > 0 && ((JnMdBlock *) out->pdata[out->len - 1])->kind == JN_MD_BLANK)
    g_ptr_array_remove_index (out, out->len - 1);
  return out;
}

/* ---- spans ------------------------------------------------------------ */

static void
span_free (gpointer p)
{
  JnMdSpan *s = p;
  g_free (s->text);
  g_free (s->url);
  g_free (s);
}

static void
push (GPtrArray *out, JnMdSpanKind kind, const char *text, gssize len, const char *url)
{
  if (len == 0 || (len < 0 && *text == '\0'))
    return; /* $}$ is math of nothing */
  JnMdSpan *last = out->len > 0 ? out->pdata[out->len - 1] : NULL;
  if (kind == JN_SPAN_TEXT && last != NULL && last->kind == JN_SPAN_TEXT)
    {
      GString *s = g_string_new (last->text); /* runs of text are one span */
      g_string_append_len (s, text, len);
      g_free (last->text);
      last->text = g_string_free (s, FALSE);
      return;
    }
  JnMdSpan *s = g_new0 (JnMdSpan, 1);
  s->kind = kind;
  s->text = len < 0 ? g_strdup (text) : g_strndup (text, (gsize) len);
  s->url = g_strdup (url);
  g_ptr_array_add (out, s);
}

/* Is the character just before p (in the text from start) a letter or
 * digit? p - 1 is never formed at the start: it would point outside. */
static gboolean
word_before (const char *start, const char *p)
{
  return p > start && g_ascii_isalnum (p[-1]);
}

/* A [1] or [1, 2] citation at p: its length, else 0. */
static gsize
citation (const char *p)
{
  const char *q = p + 1;
  gboolean any = FALSE;
  while (g_ascii_isdigit (*q) || *q == ',' || *q == ' ')
    {
      any |= g_ascii_isdigit (*q);
      q++;
    }
  return any && *q == ']' && g_ascii_isdigit (p[1]) ? (gsize) (q - p + 1) : 0;
}

/* "http://..." or "https://...": the only links that open. */
static gboolean
web_link (const char *s)
{
  const char *scheme = g_uri_peek_scheme (s);
  if (scheme == NULL)
    return FALSE;
  gboolean web = g_str_equal (scheme, "https") || g_str_equal (scheme, "http");
  return web && strncmp (s + strlen (scheme), "://", 3) == 0;
}

/* Simple LaTeX the AI may copy from a page ($1 \le k \le N$), as
 * readable text: common commands become symbols, \text{} and grouping
 * braces go, any other \command loses its backslash. */
static char *
plain_math (const char *m, gsize len)
{
  static const struct { const char *cmd, *sym; } SYM[] = {
    { "leq", "\xe2\x89\xa4" }, { "le", "\xe2\x89\xa4" }, { "geq", "\xe2\x89\xa5" }, { "ge", "\xe2\x89\xa5" },
    { "neq", "\xe2\x89\xa0" }, { "ne", "\xe2\x89\xa0" }, { "cdot", "\xc2\xb7" }, { "times", "\xc3\x97" },
    { "ldots", "\xe2\x80\xa6" }, { "dots", "\xe2\x80\xa6" }, { "infty", "\xe2\x88\x9e" },
    { "rightarrow", "\xe2\x86\x92" }, { "to", "\xe2\x86\x92" }, { "sum", "\xce\xa3" }, { "lfloor", "\xe2\x8c\x8a" },
    { "rfloor", "\xe2\x8c\x8b" }, { "lceil", "\xe2\x8c\x88" }, { "rceil", "\xe2\x8c\x89" }, { "pm", "\xc2\xb1" },
  };
  GString *out = g_string_new (NULL);
  const char *p = m, *end = m + len;
  while (p < end)
    {
      if (*p == '\\' && p + 1 < end && g_ascii_isalpha (p[1]))
        {
          const char *q = p + 1;
          while (q < end && g_ascii_isalpha (*q))
            q++;
          gsize n = (gsize) (q - p - 1);
          const char *sym = NULL;
          for (gsize i = 0; i < G_N_ELEMENTS (SYM) && sym == NULL; i++)
            if (strlen (SYM[i].cmd) == n && strncmp (p + 1, SYM[i].cmd, n) == 0)
              sym = SYM[i].sym;
          if (sym != NULL)
            g_string_append (out, sym);
          else if (!(n == 4 && strncmp (p + 1, "text", 4) == 0) && !(n == 6 && strncmp (p + 1, "mathrm", 6) == 0))
            g_string_append_len (out, p + 1, (gssize) n);
          p = q;
          continue;
        }
      if (*p == '\\' && p + 1 < end && (p[1] == ',' || p[1] == ';' || p[1] == ' '))
        {
          p += 2; /* LaTeX spacing */
          continue;
        }
      if (*p == '\\' && p + 1 < end)
        {
          g_string_append_c (out, p[1]); /* \{ \} \_ */
          p += 2;
          continue;
        }
      if (*p != '{' && *p != '}')
        g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

GPtrArray *
jn_md_spans (const char *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (span_free);
  g_autofree char *valid = g_utf8_make_valid (text != NULL ? text : "", -1);
  const char *s = valid;
  const char *p = s;
  if (strlen (s) > MAX_SPAN_TEXT)
    {
      push (out, JN_SPAN_TEXT, s, -1, NULL);
      return out;
    }
  while (*p != '\0')
    {
      if (*p == '\\' && p[1] != '\0' && g_ascii_ispunct (p[1]))
        {
          push (out, JN_SPAN_TEXT, p + 1, 1, NULL);
          p += 2;
          continue;
        }
      if (*p == '$' && p[1] != '$' && p[1] != '\0' && p[1] != ' ')
        {
          /* $math$: no space just inside the dollars, and not followed by
           * a digit ("costs $5 and $6" is money). */
          const char *end = strchr (p + 1, '$');
          if (end != NULL && end[-1] != ' ' && !g_ascii_isdigit (end[1]))
            {
              g_autofree char *m = plain_math (p + 1, (gsize) (end - p - 1));
              push (out, JN_SPAN_CODE, m, -1, NULL);
              p = end + 1;
              continue;
            }
        }
      if (*p == '`')
        {
          const char *end = strchr (p + 1, '`');
          if (end != NULL && end > p + 1)
            {
              push (out, JN_SPAN_CODE, p + 1, end - p - 1, NULL);
              p = end + 1;
              continue;
            }
        }
      if ((p[0] == '*' || p[0] == '_') && p[1] == p[0] && p[2] != '\0' && p[2] != ' ')
        {
          char mark[3] = { p[0], p[0], '\0' };
          const char *end = strstr (p + 2, mark);
          if (end != NULL && end > p + 2 && end[-1] != ' ')
            {
              push (out, JN_SPAN_BOLD, p + 2, end - p - 2, NULL);
              p = end + 2;
              continue;
            }
        }
      if ((p[0] == '*' || p[0] == '_') && p[1] != '\0' && p[1] != ' ' && p[1] != p[0] &&
          !(p[0] == '_' && word_before (s, p)))
        {
          const char *end = strchr (p + 1, p[0]);
          while (end != NULL && (end[-1] == ' ' || (p[0] == '_' && g_ascii_isalnum (end[1]))))
            end = strchr (end + 1, p[0]);
          if (end != NULL && end > p + 1)
            {
              push (out, JN_SPAN_ITALIC, p + 1, end - p - 1, NULL);
              p = end + 1;
              continue;
            }
        }
      if (*p == '[')
        {
          /* pre[0] or f(x)[1] is code, not a citation. */
          gboolean attached = word_before (s, p) || (p > s && (p[-1] == '_' || p[-1] == ')'));
          gsize n = attached ? 0 : citation (p);
          if (n > 0)
            {
              push (out, JN_SPAN_CITE, p + 1, (gssize) n - 2, NULL);
              p += n;
              continue;
            }
          const char *close = strchr (p + 1, ']');
          if (close != NULL && close[1] == '(')
            {
              int depth = 0;
              const char *q = close + 1;
              for (; *q != '\0'; q++)
                {
                  if (*q == '(')
                    depth++;
                  else if (*q == ')')
                    {
                      depth--;
                      if (depth == 0)
                        break;
                    }
                }
              if (*q == ')')
                {
                  g_autofree char *url = g_strndup (close + 2, (gsize) (q - close - 2));
                  g_strstrip (url);
                  if (web_link (url))
                    {
                      /* [](url) has no text: the address is shown, not lost. */
                      if (close == p + 1)
                        push (out, JN_SPAN_LINK, url, -1, url);
                      else
                        push (out, JN_SPAN_LINK, p + 1, close - p - 1, url);
                      p = q + 1;
                      continue;
                    }
                  /* Any other link (javascript:, file:) is not a link: it
                   * shows as written, plain text. */
                }
            }
        }
      gboolean autolink = *p == '<' && web_link (p + 1);
      if (autolink)
        {
          const char *end = strchr (p, '>');
          if (end != NULL && memchr (p, ' ', (gsize) (end - p)) == NULL)
            {
              g_autofree char *url = g_strndup (p + 1, (gsize) (end - p - 1));
              push (out, JN_SPAN_LINK, url, -1, url);
              p = end + 1;
              continue;
            }
        }
      const char *next = g_utf8_next_char (p);
      push (out, JN_SPAN_TEXT, p, next - p, NULL);
      p = next;
    }
  return out;
}
