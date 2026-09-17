// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/*
 * einkd - org.webosports.service.eink
 *
 * Refresh-mode control for an E Ink panel, on the luna-service2 bus for the
 * Settings app and the shell.
 *
 * The one panel this knows is the Minimal Phone MP01's: a 600x800 E Ink panel
 * behind a Pango CPLD that turns MIPI DSI into E Ink waveforms. Its kernel
 * driver (the stock panel-z10-eink-i2c.ko) exposes a single write-only command
 * register in sysfs,
 *
 *   /sys/bus/i2c/drivers/eink_cpld/<addr>/eink_cpld_registers
 *
 * and stock Android drives it from a system_server service, MinimalRefreshService
 * in services.jar, through DisplayManager.setRefreshMode(int) - which writes the
 * integer to that file, nothing more. That service is the reference for every
 * value used here:
 *
 *   1  the panel's slowest, cleanest waveform - Minimal calls it "Slow",
 *      "Best quality"; the base of both its Slow and Hybrid modes
 *   4  the fastest - "Ultra", "Fast refresh"; the base of its Ultra mode and
 *      what Hybrid jumps to while an app scrolls or animates
 *   5  clear: a full refresh that wipes ghosting. Stock always follows it with
 *      3 and puts the base mode back 100 ms later, and so does this.
 *
 * (2 and 3 are also modes as far as the driver is concerned - the sibling
 * refresh_mode attribute reads the last selection back as 0..3 - but stock
 * never rests on either, so neither is offered.)
 *
 * Hybrid is not offered yet. Stock implements it with a WindowMonitor inside
 * system_server that watches scroll, animation and video state; the LuneOS
 * equivalent would be the compositor telling this service when to go fast,
 * which is a later step. The API is shaped so that adding a mode is a table
 * entry.
 *
 * The chosen mode is persisted as the systemservice preference
 * "einkRefreshMode", the same store the Display panel already uses for its
 * other settings, so the shell can subscribe to it too.
 *
 * The refresh key. The MP01 has a button between volume up and down that the
 * kernel reports as key code 252 - Android's Generic.kl maps it to
 * KEYCODE_AREFRESH, and stock's PhoneWindowManager does a full refresh on a
 * short press and opens Minimal's quick settings after 400 ms. That is
 * handled here rather than in the compositor: the code is not in Qt's evdev
 * keymap so nothing else would see it, and reading it from evdev directly
 * means it works on the lock screen and before the shell is up. The long
 * press opens the Display settings, the LuneOS equivalent of the quick
 * settings.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glob.h>
#include <linux/input.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EINK_SERVICE "org.webosports.service.eink"
#define PREF_KEY "einkRefreshMode"
#define REGISTER_GLOB "/sys/bus/i2c/drivers/eink_cpld/*/eink_cpld_registers"

/* The clear command and what stock writes straight after it. */
#define CMD_CLEAR 5
#define CMD_AFTER_CLEAR 3
#define RESTORE_DELAY_MS 100

/*
 * The refresh key. Not in input-event-codes.h - 252 is vendor space - so it is
 * named here after Android's keylayout entry for it.
 */
#define KEY_AREFRESH 252
#define LONG_PRESS_MS 400
#define SETTINGS_APP "org.webosports.app.settings.display"

struct mode
{
	const char *id;
	int value;
	const char *label;
	const char *description;
};

static const struct mode modes[] =
{
	{ "slow",  1, "Slow",  "Best quality; for reading and text." },
	{ "ultra", 4, "Ultra", "Fast refresh; more ghosting, better for scrolling and video." },
};

#define N_MODES ((int)(sizeof(modes) / sizeof(modes[0])))
#define DEFAULT_MODE 0

static LSHandle *service_handle = NULL;
static GMainLoop *main_loop = NULL;
static int exit_status = 0;

static char *register_path = NULL;
static int current_mode = DEFAULT_MODE;
static bool prefs_loaded = false;
static guint restore_source = 0;
static guint prefs_retry_source = 0;
static int prefs_attempts = 0;

static int key_fd = -1;
static guint key_source = 0;
static guint key_scan_source = 0;
static int key_scan_attempts = 0;
static guint long_press_source = 0;
static bool long_press_fired = false;

/*
 * The hub went away - restarted, or crashed. The registration made by
 * LSRegister() died with it and nothing re-establishes one, so exit non-zero and
 * let systemd start a fresh process. (Same reasoning as torchd.)
 */
static void hub_disconnected(LSHandle *sh, void *ctx)
{
	g_warning("lost the luna-service2 hub; exiting so systemd restarts us with a "
	          "fresh registration");
	exit_status = 1;

	if (main_loop)
	{
		g_main_loop_quit(main_loop);
	}
}

/*
 * Looked up on demand and re-tried until found: the panel modules are loaded by
 * the vendor's own loader, and there is no ordering between that and this
 * service worth relying on. A device with no such panel is a normal state - the
 * service answers available:false and the Settings panel hides its section.
 */
static bool ensure_register(void)
{
	glob_t g;

	if (register_path)
	{
		return true;
	}

	if (glob(REGISTER_GLOB, 0, NULL, &g) == 0 && g.gl_pathc > 0)
	{
		register_path = g_strdup(g.gl_pathv[0]);
		g_message("E Ink command register: %s", register_path);
	}

	globfree(&g);
	return register_path != NULL;
}

static bool write_register(int value)
{
	char buf[16];
	int fd, len;
	ssize_t n;

	if (!ensure_register())
	{
		return false;
	}

	len = snprintf(buf, sizeof(buf), "%d", value);
	fd = open(register_path, O_WRONLY);

	if (fd < 0)
	{
		g_warning("open %s: %s", register_path, g_strerror(errno));
		return false;
	}

	n = write(fd, buf, len);
	close(fd);

	if (n != len)
	{
		g_warning("write %d to %s: %s", value, register_path, g_strerror(errno));
		return false;
	}

	return true;
}

static int find_mode(const char *id, size_t len)
{
	for (int i = 0; i < N_MODES; i++)
	{
		if (strlen(modes[i].id) == len && strncmp(modes[i].id, id, len) == 0)
		{
			return i;
		}
	}

	return -1;
}

static bool apply_mode(void)
{
	return write_register(modes[current_mode].value);
}

static jvalue_ref build_status(void)
{
	bool available = ensure_register();
	jvalue_ref reply = jobject_create();
	jvalue_ref list = jarray_create(NULL);

	for (int i = 0; i < N_MODES; i++)
	{
		jvalue_ref m = jobject_create();
		jobject_put(m, J_CSTR_TO_JVAL("id"), jstring_create(modes[i].id));
		jobject_put(m, J_CSTR_TO_JVAL("label"), jstring_create(modes[i].label));
		jobject_put(m, J_CSTR_TO_JVAL("description"), jstring_create(modes[i].description));
		jarray_append(list, m);
	}

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("available"), jboolean_create(available));
	jobject_put(reply, J_CSTR_TO_JVAL("mode"), jstring_create(modes[current_mode].id));
	jobject_put(reply, J_CSTR_TO_JVAL("modes"), list);
	return reply;
}

static void post_status(void)
{
	LSError lserror;
	jvalue_ref status = build_status();

	LSErrorInit(&lserror);

	if (!LSSubscriptionReply(service_handle, "/getStatus",
	                         jvalue_tostring_simple(status), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&status);
}

static bool reply_error(LSHandle *sh, LSMessage *message, const char *text)
{
	LSError lserror;
	jvalue_ref reply = jobject_create();

	LSErrorInit(&lserror);
	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
	jobject_put(reply, J_CSTR_TO_JVAL("errorText"), jstring_create(text));

	if (!LSMessageReply(sh, message, jvalue_tostring_simple(reply), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&reply);
	return true;
}

static bool reply_status(LSHandle *sh, LSMessage *message)
{
	LSError lserror;
	jvalue_ref status = build_status();

	LSErrorInit(&lserror);

	if (!LSMessageReply(sh, message, jvalue_tostring_simple(status), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	j_release(&status);
	return true;
}

/* ------------------------------------------------------------ preferences -- */

static bool cb_set_preference(LSHandle *sh, LSMessage *reply, void *ctx)
{
	/* Nothing to do with the answer: the subscription below echoes the value. */
	return true;
}

static void save_preference(void)
{
	LSError lserror;
	char *payload = g_strdup_printf("{\"" PREF_KEY "\":\"%s\"}", modes[current_mode].id);

	LSErrorInit(&lserror);

	if (!LSCallOneReply(service_handle,
	                    "luna://com.webos.service.systemservice/setPreferences",
	                    payload, cb_set_preference, NULL, NULL, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	g_free(payload);
}

static gboolean subscribe_preference(gpointer data);

static bool cb_preference(LSHandle *sh, LSMessage *reply, void *ctx)
{
	JSchemaInfo schema;
	jvalue_ref parsed, value;
	bool ok = false;

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(reply)),
	                    DOMOPT_NOOPT, &schema);

	if (!jis_null(parsed) &&
	        jobject_get_exists(parsed, J_CSTR_TO_BUF("returnValue"), &value) &&
	        jis_boolean(value))
	{
		jboolean_get(value, &ok);
	}

	if (!ok)
	{
		/*
		 * systemservice is not up yet, or the hub bounced the call. Ask again
		 * shortly; the fallback in main() has meanwhile put the default mode on
		 * the panel so the screen is never left in an unknown state.
		 */
		j_release(&parsed);

		if (prefs_attempts++ < 30 && !prefs_retry_source)
		{
			prefs_retry_source = g_timeout_add_seconds(2, subscribe_preference, NULL);
		}

		return true;
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF(PREF_KEY), &value) && jis_string(value))
	{
		raw_buffer s = jstring_get_fast(value);
		int idx = find_mode(s.m_str, s.m_len);

		if (idx < 0)
		{
			g_warning("preference " PREF_KEY " holds unknown mode \"%.*s\"; keeping %s",
			          (int)s.m_len, s.m_str, modes[current_mode].id);
		}
		else if (idx != current_mode || !prefs_loaded)
		{
			current_mode = idx;
			apply_mode();
			post_status();
		}
	}
	else if (!prefs_loaded)
	{
		/* First boot: nothing stored yet. Make the default explicit. */
		apply_mode();
	}

	prefs_loaded = true;
	j_release(&parsed);
	return true;
}

static gboolean subscribe_preference(gpointer data)
{
	LSError lserror;

	prefs_retry_source = 0;
	LSErrorInit(&lserror);

	if (!LSCall(service_handle,
	            "luna://com.webos.service.systemservice/getPreferences",
	            "{\"keys\":[\"" PREF_KEY "\"],\"subscribe\":true}",
	            cb_preference, NULL, NULL, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	return G_SOURCE_REMOVE;
}

/*
 * Should systemservice never answer - it is not part of every image - the panel
 * still gets a known mode a few seconds in. The kernel default happens to be
 * the same waveform as "slow", so on a normal boot this changes nothing visible.
 */
static gboolean apply_default_if_unanswered(gpointer data)
{
	if (!prefs_loaded)
	{
		apply_mode();
	}

	return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------- methods -- */

static bool cb_get_status(LSHandle *sh, LSMessage *message, void *ctx)
{
	LSError lserror;
	LSErrorInit(&lserror);

	if (LSMessageIsSubscription(message) &&
	        !LSSubscriptionProcess(sh, message, &(bool){false}, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	return reply_status(sh, message);
}

static bool cb_set_mode(LSHandle *sh, LSMessage *message, void *ctx)
{
	JSchemaInfo schema;
	jvalue_ref parsed, value;
	int idx = -1;

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)),
	                    DOMOPT_NOOPT, &schema);

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, message, "malformed json");
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("mode"), &value) && jis_string(value))
	{
		raw_buffer s = jstring_get_fast(value);
		idx = find_mode(s.m_str, s.m_len);
	}

	j_release(&parsed);

	if (idx < 0)
	{
		return reply_error(sh, message, "need \"mode\": one of the ids listed by getStatus");
	}

	if (!ensure_register())
	{
		return reply_error(sh, message, "no E Ink panel on this device");
	}

	current_mode = idx;

	if (!apply_mode())
	{
		return reply_error(sh, message, "the panel rejected the mode");
	}

	save_preference();
	post_status();
	return reply_status(sh, message);
}

static gboolean restore_mode(gpointer data)
{
	restore_source = 0;
	apply_mode();
	return G_SOURCE_REMOVE;
}

/*
 * A full refresh, stock's forceFullRefresh: clear, the follow-up write, and the
 * base mode back after 100 ms. A second request inside that window just
 * restarts the timer; the panel is already clearing.
 */
static bool full_refresh(void)
{
	if (restore_source)
	{
		g_source_remove(restore_source);
		restore_source = 0;
	}

	if (!write_register(CMD_CLEAR) || !write_register(CMD_AFTER_CLEAR))
	{
		return false;
	}

	restore_source = g_timeout_add(RESTORE_DELAY_MS, restore_mode, NULL);
	return true;
}

static bool cb_refresh(LSHandle *sh, LSMessage *message, void *ctx)
{
	if (!ensure_register())
	{
		return reply_error(sh, message, "no E Ink panel on this device");
	}

	if (!full_refresh())
	{
		return reply_error(sh, message, "the panel rejected the refresh");
	}

	return reply_status(sh, message);
}

/* ------------------------------------------------------------ refresh key -- */

static bool cb_launched(LSHandle *sh, LSMessage *reply, void *ctx)
{
	return true;
}

static gboolean long_press(gpointer data)
{
	LSError lserror;

	long_press_source = 0;
	long_press_fired = true;
	LSErrorInit(&lserror);

	if (!LSCallOneReply(service_handle,
	                    "luna://com.webos.service.applicationManager/launch",
	                    "{\"id\":\"" SETTINGS_APP "\"}",
	                    cb_launched, NULL, NULL, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	return G_SOURCE_REMOVE;
}

static gboolean scan_for_refresh_key(gpointer data);

static void close_refresh_key(void)
{
	if (key_source)
	{
		g_source_remove(key_source);
		key_source = 0;
	}

	if (key_fd >= 0)
	{
		close(key_fd);
		key_fd = -1;
	}
}

static gboolean on_key_event(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	struct input_event ev;
	ssize_t n;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
	{
		g_warning("refresh key device went away; looking for it again");
		close_refresh_key();
		key_scan_attempts = 0;
		key_scan_source = g_timeout_add_seconds(2, scan_for_refresh_key, NULL);
		return G_SOURCE_REMOVE;
	}

	while ((n = read(key_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev))
	{
		if (ev.type != EV_KEY || ev.code != KEY_AREFRESH)
		{
			continue;
		}

		if (ev.value == 1)
		{
			long_press_fired = false;

			if (long_press_source)
			{
				g_source_remove(long_press_source);
			}

			long_press_source = g_timeout_add(LONG_PRESS_MS, long_press, NULL);
		}
		else if (ev.value == 0)
		{
			if (long_press_source)
			{
				g_source_remove(long_press_source);
				long_press_source = 0;
			}

			if (!long_press_fired && ensure_register())
			{
				full_refresh();
			}
		}
	}

	return G_SOURCE_CONTINUE;
}

/*
 * The keypad is built in, but its driver is one of the vendor modules and may
 * come up after this service, so the scan is retried for a while. Whichever
 * device advertises the key code is taken; the read is shared, not a grab, so
 * the compositor still sees the device (it ignores the code).
 */
static gboolean scan_for_refresh_key(gpointer data)
{
	unsigned long bits[(KEY_MAX + 1 + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))];
	char path[32], name[64];

	key_scan_source = 0;

	for (int i = 0; i < 32 && key_fd < 0; i++)
	{
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd < 0)
		{
			continue;
		}

		memset(bits, 0, sizeof(bits));

		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
		        (bits[KEY_AREFRESH / (8 * sizeof(unsigned long))] >>
		         (KEY_AREFRESH % (8 * sizeof(unsigned long)))) & 1UL)
		{
			GIOChannel *channel = g_io_channel_unix_new(fd);

			if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			{
				g_strlcpy(name, "?", sizeof(name));
			}

			g_message("refresh key (code %d) on %s (%s)", KEY_AREFRESH, path, name);
			key_fd = fd;
			key_source = g_io_add_watch(channel, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			                            on_key_event, NULL);
			g_io_channel_unref(channel);
			break;
		}

		close(fd);
	}

	if (key_fd < 0 && key_scan_attempts++ < 30)
	{
		key_scan_source = g_timeout_add_seconds(2, scan_for_refresh_key, NULL);
	}

	return G_SOURCE_REMOVE;
}

static LSMethod methods[] =
{
	{ "getStatus", cb_get_status, LUNA_METHOD_FLAGS_NONE },
	{ "setMode",   cb_set_mode,   LUNA_METHOD_FLAGS_NONE },
	{ "refresh",   cb_refresh,    LUNA_METHOD_FLAGS_NONE },
	{ NULL, NULL, LUNA_METHOD_FLAGS_NONE },
};

int main(int argc, char **argv)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	LSError lserror;

	main_loop = loop;

	LSErrorInit(&lserror);

	if (!LSRegister(EINK_SERVICE, &service_handle, &lserror))
	{
		goto fail;
	}

	if (!LSRegisterCategory(service_handle, "/", methods, NULL, NULL, &lserror))
	{
		goto fail;
	}

	if (!LSSetDisconnectHandler(service_handle, hub_disconnected, NULL, &lserror))
	{
		goto fail;
	}

	if (!LSGmainAttach(service_handle, loop, &lserror))
	{
		goto fail;
	}

	ensure_register();
	subscribe_preference(NULL);
	g_timeout_add_seconds(5, apply_default_if_unanswered, NULL);
	scan_for_refresh_key(NULL);

	g_main_loop_run(loop);

	close_refresh_key();
	LSUnregister(service_handle, &lserror);
	g_main_loop_unref(loop);
	g_free(register_path);
	return exit_status;

fail:
	LSErrorPrint(&lserror, stderr);
	LSErrorFree(&lserror);
	g_main_loop_unref(loop);
	return 1;
}
