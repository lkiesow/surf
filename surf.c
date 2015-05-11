/* See LICENSE file for copyright and license details.
 *
 * To understand surf, start reading main().
 */

#include <signal.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <gtk/gtkx.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <webkit2/webkit2.h>
#include <glib/gstdio.h>
#include <JavaScriptCore/JavaScript.h>
#include <sys/file.h>
#include <libgen.h>
#include <stdarg.h>
#include <regex.h>

#include "arg.h"

char *argv0;

#define LENGTH(x)               (sizeof x / sizeof x[0])
#define CLEANMASK(mask)         (mask & (MODKEY|GDK_SHIFT_MASK))

enum { AtomFind, AtomGo, AtomUri, AtomLast };
enum {
	OnDoc   = WEBKIT_HIT_TEST_RESULT_CONTEXT_DOCUMENT,
	OnLink  = WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK,
	OnImg   = WEBKIT_HIT_TEST_RESULT_CONTEXT_IMAGE,
	OnMedia = WEBKIT_HIT_TEST_RESULT_CONTEXT_MEDIA,
	OnSel   = WEBKIT_HIT_TEST_RESULT_CONTEXT_SELECTION,
	OnEdit  = WEBKIT_HIT_TEST_RESULT_CONTEXT_EDITABLE,
	OnAny   = OnDoc | OnLink | OnImg | OnMedia | OnSel | OnEdit,
};

typedef union Arg Arg;
union Arg {
	gboolean b;
	gint i;
	const void *v;
};

typedef struct Client {
	GtkWidget *win, *scroll, *vbox, *pane;
	GdkWindow *cwin;
	WebKitWebView *view;
	WebKitFindController *finder;
	WebKitWebInspector *inspector;
	char *title, *linkhover;
	const char *needle;
	gint progress;
	struct Client *next;
	gboolean zoomed, fullscreen, inspecting, sslfailed;
} Client;

typedef struct {
	guint mod;
	guint keyval;
	void (*func)(Client *c, const Arg *arg);
	const Arg arg;
} Key;

typedef struct {
	unsigned int where;
	unsigned int mask;
	guint button;
	void (*func)(Client *c, const Arg *arg);
	const Arg arg;
} Button;

typedef struct {
	char *regex;
	char *style;
	regex_t re;
} SiteStyle;

static Display *dpy;
static Atom atoms[AtomLast];
static Client *clients = NULL;
static Window embed = 0;
static gboolean showxid = FALSE;
static char winid[64];
static gboolean usingproxy = 0;
static char togglestat[9];
static char pagestat[3];
static int policysel = 0;
static char *stylefile = NULL;
static SoupCache *diskcache = NULL;

static void addaccelgroup(Client *c);
static void beforerequest(WebKitWebView *w, WebKitWebResource *r,
		WebKitURIRequest *req, Client *c);
static char *buildpath(const char *path);
static void cleanup(void);
static void clipboard(Client *c, const Arg *arg);
static WebKitCookieAcceptPolicy cookiepolicy_get(void);
static char cookiepolicy_set(const WebKitCookieAcceptPolicy p);
static char *copystr(char **str, const char *src);
static WebKitWebView *createwindow(WebKitWebView *v, Client *c);
static gboolean decidedownload(WebKitPolicyDecision *d, Client *c);
static gboolean decidepolicy(WebKitWebView *v, WebKitPolicyDecision *d,
		WebKitPolicyDecisionType t, Client *c);
static gboolean decidewindow(gboolean newwin, WebKitPolicyDecision *d,
		Client *c);
static void destroyclient(Client *c);
static void destroywin(GtkWidget* w, Client *c);
static void die(const char *errstr, ...);
static void eval(Client *c, const Arg *arg);
static void find(Client *c, const Arg *arg);
static void fullscreen(Client *c, const Arg *arg);
static gboolean permissionrequest(WebKitWebView *v, WebKitPermissionRequest *r,
		Client *c);
static const char *getatom(Client *c, int a);
static void gettogglestat(Client *c);
static void getpagestat(Client *c);
static char *geturi(Client *c);
static gchar *getstyle(const char *uri);

static void handleplumb(Client *c, WebKitWebView *w, const gchar *uri);

static gboolean initdownload(Client *c, Arg *a);

static void inspector(Client *c, const Arg *arg);

static gboolean keypress(GtkAccelGroup *group,
		GObject *obj, guint key, GdkModifierType mods,
		Client *c);
static void loadchange(WebKitWebView *view, WebKitLoadEvent event,
		Client *c);
static void loaduri(Client *c, const Arg *arg);
static void navigate(Client *c, const Arg *arg);
static Client *newclient(void);
static void newwindow(Client *c, const Arg *arg, gboolean noembed);
static void pasteuri(GtkClipboard *clipboard, const char *text, gpointer d);
static void print(Client *c, const Arg *arg);
static GdkFilterReturn processx(GdkXEvent *xevent, GdkEvent *event,
		gpointer d);
static void progresschange(WebKitWebView *view, GParamSpec *pspec, Client *c);
static void linkopen(Client *c, const Arg *arg);
static void linkopenembed(Client *c, const Arg *arg);
static void reload(Client *c, const Arg *arg);
static void runscript(WebKitWebView *view);
static void scroll_h(Client *c, const Arg *arg);
static void scroll_v(Client *c, const Arg *arg);
static void scroll(GtkAdjustment *a, const Arg *arg);
static void setatom(Client *c, int a, const char *v);
static void setup(int *argc, char **argv[]);
static void sigchld(int unused);
static void spawn(Client *c, const Arg *arg);
static void stop(Client *c, const Arg *arg);
static void titlechange(WebKitWebView *view, GParamSpec *pspec, Client *c);
static void titlechangeleave(void *a, void *b, Client *c);
static void toggle(Client *c, const Arg *arg);
static void togglecookiepolicy(Client *c, const Arg *arg);
static void togglegeolocation(Client *c, const Arg *arg);
static void togglescrollbars(Client *c, const Arg *arg);
static void togglestyle(Client *c, const Arg *arg);
static void updatetitle(Client *c);
static void updatewinid(Client *c);
static void usage(void);
static void zoom(Client *c, const Arg *arg);

/* configuration, allows nested code to access above variables */
#include "config.h"

static void
addaccelgroup(Client *c) {
	int i;
	GtkAccelGroup *group = gtk_accel_group_new();
	GClosure *closure;

	for(i = 0; i < LENGTH(keys); i++) {
		closure = g_cclosure_new(G_CALLBACK(keypress), c, NULL);
		gtk_accel_group_connect(group, keys[i].keyval, keys[i].mod,
				0, closure);
	}
	gtk_window_add_accel_group(GTK_WINDOW(c->win), group);
}

static void
beforerequest(WebKitWebView *w, WebKitWebResource *r, WebKitURIRequest *req,
    Client *c) {
	const gchar *uri = webkit_uri_request_get_uri(req);
	int i, isascii = 1;

	if(g_str_has_suffix(uri, "/favicon.ico"))
		webkit_uri_request_set_uri(req, "about:blank");

	if(!g_str_has_prefix(uri, "http://") \
			&& !g_str_has_prefix(uri, "https://") \
			&& !g_str_has_prefix(uri, "about:") \
			&& !g_str_has_prefix(uri, "file://") \
			&& !g_str_has_prefix(uri, "data:") \
			&& !g_str_has_prefix(uri, "blob:") \
			&& strlen(uri) > 0) {

		for(i = 0; i < strlen(uri); i++) {
			if(!g_ascii_isprint(uri[i])) {
				isascii = 0;
				break;
			}
		}
		if(isascii)
			handleplumb(c, w, uri);
	}
}

static char *
buildpath(const char *path) {
	char *apath, *p;
	FILE *f;

	/* creating directory */
	if(path[0] == '/') {
		apath = g_strdup(path);
	} else if(path[0] == '~') {
		if(path[1] == '/') {
			apath = g_strconcat(g_get_home_dir(), &path[1], NULL);
		} else {
			apath = g_strconcat(g_get_home_dir(), "/",
					&path[1], NULL);
		}
	} else {
		apath = g_strconcat(g_get_current_dir(), "/", path, NULL);
	}

	if((p = strrchr(apath, '/'))) {
		*p = '\0';
		g_mkdir_with_parents(apath, 0700);
		g_chmod(apath, 0700); /* in case it existed */
		*p = '/';
	}
	/* creating file (gives error when apath ends with "/") */
	if((f = fopen(apath, "a"))) {
		g_chmod(apath, 0600); /* always */
		fclose(f);
	}

	return apath;
}

static void
cleanup(void) {
	if(diskcache) {
		soup_cache_flush(diskcache);
		soup_cache_dump(diskcache);
	}
	while(clients)
		destroyclient(clients);
	g_free(cookiefile);
	g_free(scriptfile);
	g_free(stylefile);
}

static WebKitCookieAcceptPolicy
cookiepolicy_get(void) {
	switch(cookiepolicies[policysel]) {
	case 'a':
		return WEBKIT_COOKIE_POLICY_ACCEPT_NEVER;
	case '@':
		return WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
	case 'A':
	default:
		break;
	}

	return WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
}

static char
cookiepolicy_set(const WebKitCookieAcceptPolicy ap) {
	switch(ap) {
	case WEBKIT_COOKIE_POLICY_ACCEPT_NEVER:
		return 'a';
	case WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY:
		return '@';
	case WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS:
	default:
		break;
	}

	return 'A';
}

static void
evalscript(WebKitWebView *view, const char *jsstr, ...) {
	va_list ap;
	gchar *script;

	va_start(ap, jsstr);
	script = g_strdup_vprintf(jsstr, ap);
	va_end(ap);
	webkit_web_view_run_javascript(view, script, NULL, NULL, NULL);
	g_free(script);
}

static void
runscript(WebKitWebView *view) {
	char *script;
	GError *error;

	if(g_file_get_contents(scriptfile, &script, NULL, &error))
		evalscript(view, script);
}

static void
clipboard(Client *c, const Arg *arg) {
	gboolean paste = *(gboolean *)arg;

	if(paste) {
		gtk_clipboard_request_text(
				gtk_clipboard_get(GDK_SELECTION_PRIMARY),
				pasteuri, c);
	} else {
		gtk_clipboard_set_text(
				gtk_clipboard_get(GDK_SELECTION_PRIMARY),
				c->linkhover ? c->linkhover : geturi(c), -1);
	}
}

static char *
copystr(char **str, const char *src) {
	char *tmp;
	tmp = g_strdup(src);

	if(str && *str) {
		g_free(*str);
		*str = tmp;
	}
	return tmp;
}

static WebKitWebView *
createwindow(WebKitWebView  *v, Client *c) {
	Client *n = newclient();
	return n->view;
}

static gboolean
decidepolicy(WebKitWebView *v, WebKitPolicyDecision *d,
		WebKitPolicyDecisionType t, Client *c) {
	gboolean handled;

	switch (t) {
	case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
		if ((handled = decidewindow(FALSE, d, c)))
			webkit_policy_decision_ignore(d);
		break;
	case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
		handled = decidewindow(TRUE, d, c);
		webkit_policy_decision_ignore(d);
		break;
	case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
		if ((handled = decidedownload(d, c)))
			webkit_policy_decision_ignore(d);
		break;
	default:
		handled = FALSE;
	}
	return handled;
}

static gboolean
decidedownload(WebKitPolicyDecision *d, Client *c) {
	Arg arg;
	WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(d);

	if (!webkit_response_policy_decision_is_mime_type_supported(r)) {
		arg.v = webkit_uri_request_get_uri(
		    webkit_response_policy_decision_get_request(r));
		initdownload(c, &arg);
		return TRUE;
	}
	return FALSE;
}

static gboolean
decidewindow(gboolean newwin, WebKitPolicyDecision *d, Client *c) {
	Arg arg;
	WebKitNavigationAction *a;
	guint button, modifiers;
	gboolean handled = FALSE;
	int i;

	a = webkit_navigation_policy_decision_get_navigation_action(
		WEBKIT_NAVIGATION_POLICY_DECISION(d));

	if (webkit_navigation_action_is_user_gesture(a) &&
	    webkit_navigation_action_get_navigation_type(a) ==
	    WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
		arg.v = webkit_uri_request_get_uri(
		    webkit_navigation_action_get_request(a));

		if (newwin) {
			newwindow(NULL, &arg, 0);
			handled = TRUE;
		} else {
			button = webkit_navigation_action_get_mouse_button(a);
			modifiers = webkit_navigation_action_get_modifiers(a);
			for (i = 0; i < LENGTH(buttons); ++i) {
				if (buttons[i].where == OnLink &&
				    buttons[i].button == button &&
				    CLEANMASK(buttons[i].mask) ==
				    CLEANMASK(modifiers) &&
				    buttons[i].func) {
					buttons[i].func(c, buttons[i].arg.i ?
					    &buttons[i].arg : &arg);
					handled = TRUE;
				}
			}
		}
	}

	return handled;
}

static void
destroyclient(Client *c) {
	Client *p;

	webkit_web_view_stop_loading(c->view);
	gtk_widget_destroy(GTK_WIDGET(c->view));
	gtk_widget_destroy(c->scroll);
	gtk_widget_destroy(c->vbox);
	gtk_widget_destroy(c->win);

	for(p = clients; p && p->next != c; p = p->next);
	if(p) {
		p->next = c->next;
	} else {
		clients = c->next;
	}
	free(c);
	if(clients == NULL)
		gtk_main_quit();
}

static void
destroywin(GtkWidget* w, Client *c) {
	destroyclient(c);
}

static void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static void
find(Client *c, const Arg *arg) {
	const char *s;

	s = getatom(c, AtomFind);
	gboolean forward = *(gboolean *)arg;

	if (g_strcmp0(webkit_find_controller_get_search_text(c->finder), s))
		webkit_find_controller_search(c->finder, s, findopts, G_MAXUINT);
	else
		forward ? webkit_find_controller_search_next(c->finder) :
		    webkit_find_controller_search_previous(c->finder);
}

static void
fullscreen(Client *c, const Arg *arg) {
	if(c->fullscreen) {
		gtk_window_unfullscreen(GTK_WINDOW(c->win));
	} else {
		gtk_window_fullscreen(GTK_WINDOW(c->win));
	}
	c->fullscreen = !c->fullscreen;
}

static gboolean
permissionrequest(WebKitWebView *v, WebKitPermissionRequest *r, Client *c) {
	if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(r)) {
		if(allowgeolocation) {
			webkit_permission_request_allow(r);
		} else {
			webkit_permission_request_deny(r);
		}
		return TRUE;
	}
	return FALSE;
}

static const char *
getatom(Client *c, int a) {
	static char buf[BUFSIZ];
	Atom adummy;
	int idummy;
	unsigned long ldummy;
	unsigned char *p = NULL;

	XGetWindowProperty(dpy, GDK_WINDOW_XID(c->cwin),
			atoms[a], 0L, BUFSIZ, False, XA_STRING,
			&adummy, &idummy, &ldummy, &ldummy, &p);
	if(p) {
		strncpy(buf, (char *)p, LENGTH(buf)-1);
	} else {
		buf[0] = '\0';
	}
	XFree(p);

	return buf;
}

static char *
geturi(Client *c) {
	char *uri;

	if(!(uri = (char *)webkit_web_view_get_uri(c->view)))
		uri = "about:blank";
	return uri;
}

static gchar *
getstyle(const char *uri) {
	int i;

	if(stylefile != NULL)
		return g_strconcat("file://", stylefile, NULL);

	for(i = 0; i < LENGTH(styles); i++) {
		if(styles[i].regex && !regexec(&(styles[i].re), uri, 0,
					NULL, 0)) {
			return g_strconcat("file://", styles[i].style, NULL);
		}
	}
	return g_strdup("");
}

static void
handleplumb(Client *c, WebKitWebView *w, const gchar *uri) {
	Arg arg;

	webkit_web_view_stop_loading(w);
	arg = (Arg)PLUMB((char *)uri);
	spawn(c, &arg);
}

static gboolean
initdownload(Client *c, Arg *a) {
	Arg arg;

	updatewinid(c);
	arg = (Arg)DOWNLOAD((char *)a->v, geturi(c));
	spawn(c, &arg);
	return FALSE;
}

static void
inspector(Client *c, const Arg *arg) {
	if(c->inspecting) {
		c->inspecting = FALSE;
		webkit_web_inspector_close(c->inspector);
	} else {
		c->inspecting = TRUE;
		webkit_web_inspector_show(c->inspector);
	}
}

static gboolean
keypress(GtkAccelGroup *group, GObject *obj,
		guint key, GdkModifierType mods, Client *c) {
	guint i;
	gboolean processed = FALSE;

	mods = CLEANMASK(mods);
	key = gdk_keyval_to_lower(key);
	updatewinid(c);
	for(i = 0; i < LENGTH(keys); i++) {
		if(key == keys[i].keyval
				&& mods == keys[i].mod
				&& keys[i].func) {
			keys[i].func(c, &(keys[i].arg));
			processed = TRUE;
		}
	}

	return processed;
}

static void
mousetargetchanged(WebKitWebView *v, WebKitHitTestResult *h, guint mods,
    Client *c) {
	WebKitHitTestResultContext hc = webkit_hit_test_result_get_context(h);

	if (hc & OnLink) {
		c->linkhover = copystr(&c->linkhover,
		    webkit_hit_test_result_get_link_uri(h));
	} else if(c->linkhover) {
		free(c->linkhover);
		c->linkhover = NULL;
	}
	updatetitle(c);
}

static void
loadchange(WebKitWebView *view, WebKitLoadEvent event, Client *c) {
	GTlsCertificateFlags tlsflags;
	char *uri;

	switch(event) {
	case WEBKIT_LOAD_STARTED:
		c->sslfailed = FALSE;
		break;
	case WEBKIT_LOAD_COMMITTED:
		uri = geturi(c);
		if (webkit_web_view_get_tls_info(c->view, NULL, &tlsflags)) {
			c->sslfailed = TRUE;
		}
		setatom(c, AtomUri, uri);

/*
		if(enablestyles) {
			g_object_set(G_OBJECT(set), "user-stylesheet-uri",
					getstyle(uri), NULL);
		}
*/
		break;
	case WEBKIT_LOAD_FINISHED:
		c->progress = 100;
		updatetitle(c);
/*	CHECK */
		if(diskcache) {
			soup_cache_flush(diskcache);
			soup_cache_dump(diskcache);
		}
		break;
	default:
		break;
	}
}

static void
loaduri(Client *c, const Arg *arg) {
	char *u = NULL, *rp;
	const char *uri = (char *)arg->v;
	Arg a = { .b = FALSE };
	struct stat st;

	if(strcmp(uri, "") == 0)
		return;

	/* In case it's a file path. */
	if(stat(uri, &st) == 0) {
		rp = realpath(uri, NULL);
		u = g_strdup_printf("file://%s", rp);
		free(rp);
	} else {
		u = g_strrstr(uri, "://") ? g_strdup(uri)
			: g_strdup_printf("http://%s", uri);
	}

	setatom(c, AtomUri, uri);

	/* prevents endless loop */
	if(strcmp(u, geturi(c)) == 0) {
		reload(c, &a);
	} else {
		webkit_web_view_load_uri(c->view, u);
		c->progress = 0;
		c->title = copystr(&c->title, u);
		updatetitle(c);
	}
	g_free(u);
}

static void
navigate(Client *c, const Arg *arg) {
	int steps = *(int *)arg;

	if (steps < 0)
		webkit_web_view_go_back(c->view);
	else if (steps > 0)
		webkit_web_view_go_forward(c->view);
}

static Client *
newclient(void) {
	Client *c;
	WebKitSettings *settings;
	GdkRGBA bgcolor = { .0, .0, .0, .0 };
	GdkGeometry hints = { 1, 1 };
	GdkScreen *screen;
	gdouble dpi;
	char *ua;

	if(!(c = calloc(1, sizeof(Client))))
		die("Cannot malloc!\n");

	c->title = NULL;
	c->progress = 100;

	/* Window */
	if(embed) {
		c->win = gtk_plug_new(embed);
	} else {
		c->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

		/* TA:  20091214:  Despite what the GNOME docs say, the ICCCM
		 * is always correct, so we should still call this function.
		 * But when doing so, we *must* differentiate between a
		 * WM_CLASS and a resource on the window.  By convention, the
		 * window class (WM_CLASS) is capped, while the resource is in
		 * lowercase.   Both these values come as a pair.
		 */
		gtk_window_set_wmclass(GTK_WINDOW(c->win), "surf", "Surf");

		/* TA:  20091214:  And set the role here as well -- so that
		 * sessions can pick this up.
		 */
		gtk_window_set_role(GTK_WINDOW(c->win), "Surf");
	}
	gtk_window_set_default_size(GTK_WINDOW(c->win), 800, 600);
	g_signal_connect(G_OBJECT(c->win),
			"destroy",
			G_CALLBACK(destroywin), c);
	g_signal_connect(G_OBJECT(c->win),
			"leave_notify_event",
			G_CALLBACK(titlechangeleave), c);

	if(!kioskmode)
		addaccelgroup(c);

	/* Pane */
	c->pane = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

	/* VBox */
	c->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_pack1(GTK_PANED(c->pane), c->vbox, TRUE, TRUE);

	/* Webview */
	c->view = WEBKIT_WEB_VIEW(webkit_web_view_new());

	g_signal_connect(G_OBJECT(c->view),
			"notify::title",
			G_CALLBACK(titlechange), c);
	g_signal_connect(G_OBJECT(c->view),
			"create",
			G_CALLBACK(createwindow), c);
	g_signal_connect(G_OBJECT(c->view),
			"load-change",
			G_CALLBACK(loadchange), c);
	g_signal_connect(G_OBJECT(c->view),
			"notify::estimated-load-progress",
			G_CALLBACK(progresschange), c);
	g_signal_connect(G_OBJECT(c->view),
			"resource-load-starting",
			G_CALLBACK(beforerequest), c);
	g_signal_connect(G_OBJECT(c->view),
			"permission-request",
			G_CALLBACK(permissionrequest), c);
	g_signal_connect(G_OBJECT(c->view),
			"decide-policy",
			G_CALLBACK(decidepolicy), c);
	g_signal_connect(G_OBJECT(c->view),
			"mouse-target-changed",
			G_CALLBACK(mousetargetchanged), c);

	/* Scrolled Window */
	c->scroll = gtk_scrolled_window_new(NULL, NULL);

/*
	g_signal_connect(G_OBJECT(frame), "scrollbars-policy-changed",
			G_CALLBACK(gtk_true), NULL);
*/

	if(!enablescrollbars) {
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroll),
				GTK_POLICY_NEVER, GTK_POLICY_NEVER);
	} else {
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroll),
				GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	}

	/* Arranging */
	gtk_container_add(GTK_CONTAINER(c->scroll), GTK_WIDGET(c->view));
	gtk_container_add(GTK_CONTAINER(c->win), c->pane);
	gtk_container_add(GTK_CONTAINER(c->vbox), c->scroll);

	/* Setup */
	gtk_box_set_child_packing(GTK_BOX(c->vbox), c->scroll, TRUE,
			TRUE, 0, GTK_PACK_START);
	gtk_widget_grab_focus(GTK_WIDGET(c->view));
	gtk_widget_show(c->pane);
	gtk_widget_show(c->vbox);
	gtk_widget_show(c->scroll);
	gtk_widget_show(GTK_WIDGET(c->view));
	gtk_widget_show(c->win);
	c->cwin = gtk_widget_get_window(GTK_WIDGET(c->win));
	gtk_window_set_geometry_hints(GTK_WINDOW(c->win), NULL, &hints,
			GDK_HINT_MIN_SIZE);
	gdk_window_set_events(c->cwin, GDK_ALL_EVENTS_MASK);
	gdk_window_add_filter(c->cwin, processx, c);

	runscript(c->view);

	settings = webkit_web_view_get_settings(c->view);
	if(!(ua = getenv("SURF_USERAGENT")))
		ua = useragent;
	g_object_set(G_OBJECT(settings), "user-agent", ua, NULL);
	if (enablestyles) {
		g_object_set(G_OBJECT(settings), "user-stylesheet-uri",
					 getstyle("about:blank"), NULL);
	}
	g_object_set(G_OBJECT(settings), "auto-load-images", loadimages,
			NULL);
	g_object_set(G_OBJECT(settings), "enable-plugins", enableplugins,
			NULL);
	g_object_set(G_OBJECT(settings), "enable-scripts", enablescripts,
			NULL);
	g_object_set(G_OBJECT(settings), "enable-spatial-navigation",
			enablespatialbrowsing, NULL);
	g_object_set(G_OBJECT(settings), "enable-developer-extras",
			enableinspector, NULL);
	g_object_set(G_OBJECT(settings), "enable-default-context-menu",
			kioskmode ^ 1, NULL);
	g_object_set(G_OBJECT(settings), "default-font-size",
			defaultfontsize, NULL);
	g_object_set(G_OBJECT(settings), "resizable-text-areas",
			1, NULL);

	/*
	 * While stupid, CSS specifies that a pixel represents 1/96 of an inch.
	 * This ensures websites are not unusably small with a high DPI screen.
	 * It is equivalent to firefox's "layout.css.devPixelsPerPx" setting.
	 */
	if(zoomto96dpi) {
		screen = gdk_window_get_screen(c->cwin);
		dpi = gdk_screen_get_resolution(screen);
		if(dpi != -1) {
			g_object_set(G_OBJECT(settings), "enforce-96-dpi", true,
					NULL);
			webkit_web_view_set_zoom_level(c->view, dpi/96);
		}
	}
	/* This might conflict with _zoomto96dpi_. */
	if(zoomlevel != 1.0)
		webkit_web_view_set_zoom_level(c->view, zoomlevel);

	if(enableinspector) {
		c->inspector = webkit_web_view_get_inspector(c->view);
		c->inspecting = FALSE;
	}

	if(runinfullscreen) {
		c->fullscreen = 0;
		fullscreen(c, NULL);
	}

	setatom(c, AtomFind, "");
	setatom(c, AtomUri, "about:blank");
	if(hidebackground)
		webkit_web_view_set_background_color(c->view, &bgcolor);

	c->next = clients;
	clients = c;

	if(showxid) {
		gdk_display_sync(gtk_widget_get_display(c->win));
		printf("%u\n",
			(guint)GDK_WINDOW_XID(c->cwin));
		fflush(NULL);
                if (fclose(stdout) != 0) {
			die("Error closing stdout");
                }
	}

	return c;
}

static void
newwindow(Client *c, const Arg *arg, gboolean noembed) {
	guint i = 0;
	const char *cmd[18], *uri;
	const Arg a = { .v = (void *)cmd };
	char tmp[64];

	cmd[i++] = argv0;
	cmd[i++] = "-a";
	cmd[i++] = cookiepolicies;
	if(!enablescrollbars)
		cmd[i++] = "-b";
	if(embed && !noembed) {
		cmd[i++] = "-e";
		snprintf(tmp, LENGTH(tmp), "%u", (int)embed);
		cmd[i++] = tmp;
	}
	if(!allowgeolocation)
		cmd[i++] = "-g";
	if(!loadimages)
		cmd[i++] = "-i";
	if(kioskmode)
		cmd[i++] = "-k";
	if(!enableplugins)
		cmd[i++] = "-p";
	if(!enablescripts)
		cmd[i++] = "-s";
	if(showxid)
		cmd[i++] = "-x";
	if(enablediskcache)
		cmd[i++] = "-D";
	cmd[i++] = "-c";
	cmd[i++] = cookiefile;
	cmd[i++] = "--";
	uri = arg->v ? (char *)arg->v : c->linkhover;
	if(uri)
		cmd[i++] = uri;
	cmd[i++] = NULL;
	spawn(NULL, &a);
}

static void
pasteuri(GtkClipboard *clipboard, const char *text, gpointer d) {
	Arg arg = {.v = text };
	if(text != NULL)
		loaduri((Client *) d, &arg);
}

static void
print(Client *c, const Arg *arg) {
	webkit_print_operation_run_dialog(webkit_print_operation_new(c->view),
	    GTK_WINDOW(c->win));
}

static GdkFilterReturn
processx(GdkXEvent *e, GdkEvent *event, gpointer d) {
	Client *c = (Client *)d;
	XPropertyEvent *ev;
	Arg arg;

	if(((XEvent *)e)->type == PropertyNotify) {
		ev = &((XEvent *)e)->xproperty;
		if(ev->state == PropertyNewValue) {
			if(ev->atom == atoms[AtomFind]) {
				arg.b = TRUE;
				find(c, &arg);

				return GDK_FILTER_REMOVE;
			} else if(ev->atom == atoms[AtomGo]) {
				arg.v = getatom(c, AtomGo);
				loaduri(c, &arg);

				return GDK_FILTER_REMOVE;
			}
		}
	}
	return GDK_FILTER_CONTINUE;
}

static void
progresschange(WebKitWebView *view, GParamSpec *pspec, Client *c) {
	c->progress = webkit_web_view_get_estimated_load_progress(c->view) *
	    100;
	updatetitle(c);
}

static void
linkopen(Client *c, const Arg *arg) {
	newwindow(NULL, arg, 1);
}

static void
linkopenembed(Client *c, const Arg *arg) {
	newwindow(NULL, arg, 0);
}

static void
reload(Client *c, const Arg *arg) {
	gboolean nocache = *(gboolean *)arg;
	if(nocache) {
		 webkit_web_view_reload_bypass_cache(c->view);
	} else {
		 webkit_web_view_reload(c->view);
	}
}

static void
scroll_h(Client *c, const Arg *arg) {
	scroll(gtk_scrolled_window_get_hadjustment(
				GTK_SCROLLED_WINDOW(c->scroll)), arg);
}

static void
scroll_v(Client *c, const Arg *arg) {
	scroll(gtk_scrolled_window_get_vadjustment(
				GTK_SCROLLED_WINDOW(c->scroll)), arg);
}

static void
scroll(GtkAdjustment *a, const Arg *arg) {
	gdouble v;

	v = gtk_adjustment_get_value(a);
	switch(arg->i) {
	case +10000:
	case -10000:
		v += gtk_adjustment_get_page_increment(a) *
			(arg->i / 10000);
		break;
	case +20000:
	case -20000:
	default:
		v += gtk_adjustment_get_step_increment(a) * arg->i;
	}

	v = MAX(v, 0.0);
	v = MIN(v, gtk_adjustment_get_upper(a) -
			gtk_adjustment_get_page_size(a));
	gtk_adjustment_set_value(a, v);
}

static void
setatom(Client *c, int a, const char *v) {
	XSync(dpy, False);
	XChangeProperty(dpy, GDK_WINDOW_XID(c->cwin),
			atoms[a], XA_STRING, 8, PropModeReplace,
			(unsigned char *)v, strlen(v) + 1);
}

static void
setup(int *argc, char **argv[]) {
	int i;
	WebKitWebContext *context;
	WebKitCookieManager *cm;

	/* clean up any zombies immediately */
	sigchld(0);
	gtk_init(argc, argv);

	dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());

	/* atoms */
	atoms[AtomFind] = XInternAtom(dpy, "_SURF_FIND", False);
	atoms[AtomGo] = XInternAtom(dpy, "_SURF_GO", False);
	atoms[AtomUri] = XInternAtom(dpy, "_SURF_URI", False);

	/* dirs and files */
	cookiefile = buildpath(cookiefile);
	scriptfile = buildpath(scriptfile);
	cachefolder = buildpath(cachefolder);
	styledir = buildpath(styledir);
	if(stylefile == NULL) {
		for(i = 0; i < LENGTH(styles); i++) {
			if(regcomp(&(styles[i].re), styles[i].regex,
						REG_EXTENDED)) {
				fprintf(stderr,
					"Could not compile regex: %s\n",
					styles[i].regex);
				styles[i].regex = NULL;
			}
			styles[i].style = buildpath(
					g_strconcat(styledir,
						styles[i].style, NULL));
		}
	} else {
		stylefile = buildpath(stylefile);
	}

	context = webkit_web_context_get_default();

	/* cookie jar */
	cm = webkit_web_context_get_cookie_manager(context);
	webkit_cookie_manager_set_persistent_storage(cm, cookiefile,
		WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
	webkit_cookie_manager_set_accept_policy(cm, cookiepolicy_get());

	/* disk cache */
	if(enablediskcache) {
		webkit_web_context_set_disk_cache_directory(context,
		    cachefolder);
	} else {
		webkit_web_context_set_cache_model(context,
		    WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
	}

	/* ssl */
	webkit_web_context_set_tls_errors_policy(context, strictssl ?
	    WEBKIT_TLS_ERRORS_POLICY_FAIL : WEBKIT_TLS_ERRORS_POLICY_IGNORE);
}

static void
sigchld(int unused) {
	if(signal(SIGCHLD, sigchld) == SIG_ERR)
		die("Can't install SIGCHLD handler");
	while(0 < waitpid(-1, NULL, WNOHANG));
}

static void
spawn(Client *c, const Arg *arg) {
	if(fork() == 0) {
		if(dpy)
			close(ConnectionNumber(dpy));
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "surf: execvp %s", ((char **)arg->v)[0]);
		perror(" failed");
		exit(0);
	}
}

static void
eval(Client *c, const Arg *arg) {
	evalscript(c->view, ((char **)arg->v)[0], "");
}

static void
stop(Client *c, const Arg *arg) {
	webkit_web_view_stop_loading(c->view);
}

static void
titlechange(WebKitWebView *view, GParamSpec *pspec, Client *c) {
	const gchar *t = webkit_web_view_get_title(view);
	if (t) {
		c->title = copystr(&c->title, t);
		updatetitle(c);
	}
}

static void
titlechangeleave(void *a, void *b, Client *c) {
	c->linkhover = NULL;
	updatetitle(c);
}

static void
toggle(Client *c, const Arg *arg) {
	WebKitSettings *settings;
	char *name = (char *)arg->v;
	gboolean value;
	Arg a = { .b = FALSE };

	settings = webkit_web_view_get_settings(c->view);
	g_object_get(G_OBJECT(settings), name, &value, NULL);
	g_object_set(G_OBJECT(settings), name, !value, NULL);

	reload(c, &a);
}

static void
togglecookiepolicy(Client *c, const Arg *arg) {
	WebKitCookieManager *cm;

	cm = webkit_web_context_get_cookie_manager(
	    webkit_web_context_get_default());

	policysel++;
	policysel %= strlen(cookiepolicies);
	webkit_cookie_manager_set_accept_policy(cm, cookiepolicy_get());

	updatetitle(c);
	/* Do not reload. */
}

static void
togglegeolocation(Client *c, const Arg *arg) {
	Arg a = { .b = FALSE };

	allowgeolocation ^= 1;

	reload(c, &a);
}

static void
twitch(Client *c, const Arg *arg) {
	GtkAdjustment *a;
	gdouble v;

	a = gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(c->scroll));

	v = gtk_adjustment_get_value(a);

	v += arg->i;

	v = MAX(v, 0.0);
	v = MIN(v, gtk_adjustment_get_upper(a) -
			gtk_adjustment_get_page_size(a));
	gtk_adjustment_set_value(a, v);
}

static void
togglescrollbars(Client *c, const Arg *arg) {
	GtkPolicyType vspolicy;
	Arg a;

	gtk_scrolled_window_get_policy(GTK_SCROLLED_WINDOW(c->scroll), NULL, &vspolicy);

	if(vspolicy == GTK_POLICY_AUTOMATIC) {
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroll),
				GTK_POLICY_NEVER, GTK_POLICY_NEVER);
	} else {
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroll),
				GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		a.i = +1;
		twitch(c, &a);
		a.i = -1;
		twitch(c, &a);
	}
}

static void
togglestyle(Client *c, const Arg *arg) {
	WebKitSettings *settings = webkit_web_view_get_settings(c->view);
	char *uri;

	enablestyles = !enablestyles;
	uri = enablestyles ? getstyle(geturi(c)) : g_strdup("");
	g_object_set(G_OBJECT(settings), "user-stylesheet-uri", uri, NULL);

	updatetitle(c);
}

static void
gettogglestat(Client *c){
	gboolean value;
	int p = 0;
	WebKitSettings *settings = webkit_web_view_get_settings(c->view);

	togglestat[p++] = cookiepolicy_set(cookiepolicy_get());

	g_object_get(G_OBJECT(settings), "enable-caret-browsing",
			&value, NULL);
	togglestat[p++] = value? 'C': 'c';

	togglestat[p++] = allowgeolocation? 'G': 'g';

	togglestat[p++] = enablediskcache? 'D': 'd';

	g_object_get(G_OBJECT(settings), "auto-load-images", &value, NULL);
	togglestat[p++] = value? 'I': 'i';

	g_object_get(G_OBJECT(settings), "enable-scripts", &value, NULL);
	togglestat[p++] = value? 'S': 's';

	g_object_get(G_OBJECT(settings), "enable-plugins", &value, NULL);
	togglestat[p++] = value? 'V': 'v';

	togglestat[p++] = enablestyles ? 'M': 'm';

	togglestat[p] = '\0';
}

static void
getpagestat(Client *c) {
	const char *uri = geturi(c);

	if(strstr(uri, "https://") == uri) {
		pagestat[0] = c->sslfailed ? 'U' : 'T';
	} else {
		pagestat[0] = '-';
	}

	pagestat[1] = usingproxy ? 'P' : '-';
	pagestat[2] = '\0';
}

static void
updatetitle(Client *c) {
	char *t;

	if(showindicators) {
		gettogglestat(c);
		getpagestat(c);

		if(c->linkhover) {
			t = g_strdup_printf("%s:%s | %s", togglestat,
					pagestat, c->linkhover);
		} else if(c->progress != 100) {
			t = g_strdup_printf("[%i%%] %s:%s | %s", c->progress,
					togglestat, pagestat,
					(c->title == NULL)? "" : c->title);
		} else {
			t = g_strdup_printf("%s:%s | %s", togglestat, pagestat,
					(c->title == NULL)? "" : c->title);
		}

		gtk_window_set_title(GTK_WINDOW(c->win), t);
		g_free(t);
	} else {
		gtk_window_set_title(GTK_WINDOW(c->win),
				(c->title == NULL)? "" : c->title);
	}
}

static void
updatewinid(Client *c) {
	snprintf(winid, LENGTH(winid), "%u",
			(int)GDK_WINDOW_XID(c->cwin));
}

static void
usage(void) {
	die("usage: %s [-bBfFgGiIkKnNpPsSvx]"
		" [-a cookiepolicies ] "
		" [-c cookiefile] [-e xid] [-r scriptfile]"
		" [-t stylefile] [-u useragent] [-z zoomlevel]"
		" [uri]\n", basename(argv0));
}

static void
zoom(Client *c, const Arg *arg) {
	gdouble zoom;

	zoom = webkit_web_view_get_zoom_level(c->view);
	c->zoomed = TRUE;
	if(arg->i < 0) {
		/* zoom out */
		webkit_web_view_set_zoom_level(c->view, zoom - 0.1);
	} else if(arg->i > 0) {
		/* zoom in */
		webkit_web_view_set_zoom_level(c->view, zoom + 0.1);
	} else {
		/* reset */
		c->zoomed = FALSE;
		webkit_web_view_set_zoom_level(c->view, 1.0);
	}
}

int
main(int argc, char *argv[]) {
	Arg arg;
	Client *c;

	memset(&arg, 0, sizeof(arg));

	/* command line args */
	ARGBEGIN {
	case 'a':
		cookiepolicies = EARGF(usage());
		break;
	case 'b':
		enablescrollbars = 0;
		break;
	case 'B':
		enablescrollbars = 1;
		break;
	case 'c':
		cookiefile = EARGF(usage());
		break;
	case 'd':
		enablediskcache = 0;
		break;
	case 'D':
		enablediskcache = 1;
		break;
	case 'e':
		embed = strtol(EARGF(usage()), NULL, 0);
		break;
	case 'f':
		runinfullscreen = 1;
		break;
	case 'F':
		runinfullscreen = 0;
		break;
	case 'g':
		allowgeolocation = 0;
		break;
	case 'G':
		allowgeolocation = 1;
		break;
	case 'i':
		loadimages = 0;
		break;
	case 'I':
		loadimages = 1;
		break;
	case 'k':
		kioskmode = 0;
		break;
	case 'K':
		kioskmode = 1;
		break;
	case 'm':
		enablestyles = 0;
		break;
	case 'M':
		enablestyles = 1;
		break;
	case 'n':
		enableinspector = 0;
		break;
	case 'N':
		enableinspector = 1;
		break;
	case 'p':
		enableplugins = 0;
		break;
	case 'P':
		enableplugins = 1;
		break;
	case 'r':
		scriptfile = EARGF(usage());
		break;
	case 's':
		enablescripts = 0;
		break;
	case 'S':
		enablescripts = 1;
		break;
	case 't':
		stylefile = EARGF(usage());
		break;
	case 'u':
		useragent = EARGF(usage());
		break;
	case 'v':
		die("surf-"VERSION", ©2009-2014 surf engineers, "
				"see LICENSE for details\n");
	case 'x':
		showxid = TRUE;
		break;
	case 'z':
		zoomlevel = strtof(EARGF(usage()), NULL);
		break;
	default:
		usage();
	} ARGEND;
	if(argc > 0)
		arg.v = argv[0];

	setup(&argc, &argv);
	c = newclient();
	if(arg.v) {
		loaduri(clients, &arg);
	} else {
		updatetitle(c);
	}

	gtk_main();
	cleanup();

	return EXIT_SUCCESS;
}

