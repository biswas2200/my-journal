/* report: where a bug report, a "this looks wrong" or an idea goes.
 *
 * Reports go to the project's GitHub issues, as forms (the files in
 * .github/ISSUE_TEMPLATE): what happened, the steps, what you expected,
 * and screenshots or a recording dragged into the form. The address
 * fills in the version and the system (distribution, kernel, GTK), and
 * the part of the app in the title; nothing personal, and never a word
 * of your notes or journal. */
#pragma once

#include <glib.h>

#define JR_REPORT_REPO "https://github.com/biswas2200/my-journal"

typedef enum {
  JR_REPORT_BUG,  /* something is broken */
  JR_REPORT_UI,   /* something looks wrong */
  JR_REPORT_IDEA, /* an idea or improvement */
} JrReportKind;

/* The new-issue address for `kind`, its form pre-filled with `version`
 * and `system`, and `part` ("Notes", "Journal", ...; NULL: none) at the
 * start of the title. Everything is URL-escaped. */
char *jr_report_url    (JrReportKind kind, const char *part, const char *version, const char *system);
/* "Ubuntu 26.04 LTS · Linux 7.0.0-42-generic", from /etc/os-release and
 * uname; `extra` (e.g. "GTK 4.22.1") is added when given. */
char *jr_report_system (const char *extra);
