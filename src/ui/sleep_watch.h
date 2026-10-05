/* sleep_watch: lock before the laptop sleeps (lid close or suspend).
 *
 * Listens on the system bus for logind's PrepareForSleep signal. While
 * enabled it also holds a logind "delay" inhibitor, which makes the system
 * wait (briefly) until we have locked before it actually sleeps. On resume
 * the lock screen is already showing.
 */
#pragma once

#include <glib.h>

typedef struct JrSleepWatch JrSleepWatch;
typedef void (*JrSleepFunc) (gpointer user);

JrSleepWatch *jr_sleep_watch_new         (JrSleepFunc before_sleep, gpointer user);
void          jr_sleep_watch_set_enabled (JrSleepWatch *w, gboolean enabled);
void          jr_sleep_watch_free        (JrSleepWatch *w);
