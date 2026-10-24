/* MPRIS (org.mpris.MediaPlayer2.levari) over libdbus: lets desktop shells
   (GNOME/KDE, playerctl, ...) see the track and drive playback. Linux only;
   built when dbus-devel is present (CMake HAVE_MPRIS). The thread mirrors
   the daemon server: commands go through pl_post/pl_post_seek, playlist
   reads take p->mtx. */
#include "mpris.h"
#ifdef HAVE_MPRIS

#include "../playlist/playlist.h"
#include "../library/library.h"
#include "../common.h"
#include <dbus/dbus.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <SDL2/SDL.h>

#define MP_PATH   "/org/mpris/MediaPlayer2"
#define IF_ROOT   "org.mpris.MediaPlayer2"
#define IF_PLAYER "org.mpris.MediaPlayer2.Player"
#define NO_TRACK  MP_PATH "/TrackList/NoTrack"
#define BUS_NAME  "org.mpris.MediaPlayer2.levari"

static DBusConnection *bus;
static Player *mp;
static pthread_t mp_th;
static volatile int mp_quit;
static volatile int raise_req;

static const char *ROOT_PROPS[] = {"CanQuit", "CanRaise", "HasTrackList",
                                   "Identity", "DesktopEntry",
                                   "SupportedUriSchemes", "SupportedMimeTypes"};
static const char *PLAYER_PROPS[] = {"PlaybackStatus", "LoopStatus", "Rate",
                                     "MinimumRate", "MaximumRate", "Metadata",
                                     "Position", "CanGoNext", "CanGoPrevious",
                                     "CanPlay", "CanPause", "CanSeek", "CanControl"};

static const char MP_XML[] =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
"<node name=\"" MP_PATH "\">"
"<interface name=\"org.freedesktop.DBus.Introspectable\">"
"<method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>"
"</interface>"
"<interface name=\"org.freedesktop.DBus.Properties\">"
"<method name=\"Get\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
"<arg name=\"property_name\" type=\"s\" direction=\"in\"/>"
"<arg name=\"value\" type=\"v\" direction=\"out\"/></method>"
"<method name=\"GetAll\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
"<arg name=\"props\" type=\"a{sv}\" direction=\"out\"/></method>"
"<method name=\"Set\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
"<arg name=\"property_name\" type=\"s\" direction=\"in\"/>"
"<arg name=\"value\" type=\"v\" direction=\"in\"/></method>"
"<signal name=\"PropertiesChanged\"><arg type=\"s\"/>"
"<arg type=\"a{sv}\"/><arg type=\"as\"/></signal>"
"</interface>"
"<interface name=\"" IF_ROOT "\">"
"<method name=\"Raise\"/><method name=\"Quit\"/>"
"<property name=\"CanQuit\" type=\"b\" access=\"read\"/>"
"<property name=\"CanRaise\" type=\"b\" access=\"read\"/>"
"<property name=\"HasTrackList\" type=\"b\" access=\"read\"/>"
"<property name=\"Identity\" type=\"s\" access=\"read\"/>"
"<property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>"
"<property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>"
"<property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>"
"</interface>"
"<interface name=\"" IF_PLAYER "\">"
"<method name=\"Next\"/><method name=\"Previous\"/><method name=\"Pause\"/>"
"<method name=\"Play\"/><method name=\"Stop\"/><method name=\"PlayPause\"/>"
"<method name=\"Seek\"><arg name=\"Offset\" type=\"x\" direction=\"in\"/></method>"
"<method name=\"SetPosition\"><arg name=\"TrackId\" type=\"o\" direction=\"in\"/>"
"<arg name=\"Position\" type=\"x\" direction=\"in\"/></method>"
"<method name=\"GetPosition\"><arg name=\"Position\" type=\"x\" direction=\"out\"/></method>"
"<method name=\"SeekForward\"><arg name=\"Value\" type=\"x\" direction=\"in\"/></method>"
"<method name=\"SeekBackward\"><arg name=\"Value\" type=\"x\" direction=\"in\"/></method>"
"<method name=\"OpenUri\"><arg name=\"URI\" type=\"s\" direction=\"in\"/></method>"
"<property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>"
"<property name=\"LoopStatus\" type=\"s\" access=\"read\"/>"
"<property name=\"Rate\" type=\"d\" access=\"read\"/>"
"<property name=\"MinimumRate\" type=\"d\" access=\"read\"/>"
"<property name=\"MaximumRate\" type=\"d\" access=\"read\"/>"
"<property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>"
"<property name=\"Position\" type=\"x\" access=\"read\"/>"
"<property name=\"CanGoNext\" type=\"b\" access=\"read\"/>"
"<property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>"
"<property name=\"CanPlay\" type=\"b\" access=\"read\"/>"
"<property name=\"CanPause\" type=\"b\" access=\"read\"/>"
"<property name=\"CanSeek\" type=\"b\" access=\"read\"/>"
"<property name=\"CanControl\" type=\"b\" access=\"read\"/>"
"</interface>"
"</node>";

/* position within the virtual (CUE) track in microseconds: same basis as
   the daemon status line */
static long long pos_us(Player *p) {
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    if (!p->rate || p->cur < 0) return 0;
    long long pos = p->pos_frames - base;
    if (pos < 0) pos = 0;
    if (pos > endf - base) pos = endf - base;
    return pos * 1000000 / p->rate;
}

static void track_id(Player *p, char *out, size_t n) {
    if (p->cur < 0 || p->cur >= p->pl->count) lv_strlcpy(out, NO_TRACK, n);
    else snprintf(out, n, MP_PATH "/levari/track%d", p->cur + 1);
}

/* file:// URI with percent-escaping (spaces and non-ASCII in paths) */
static void file_uri(const char *path, char *out, size_t n) {
    size_t o = 0;
    const char *pre = "file://";
    while (*pre && o + 1 < n) out[o++] = *pre++;
    for (const unsigned char *s = (const unsigned char *)path; *s && o + 4 < n; s++) {
        int c = *s;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                   c == '_' || c == '~' || c == '-' || c == '+' || c == '@';
        if (keep) out[o++] = (char)c;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* inverse of file_uri for OpenUri arguments */
static void uri_unescape(const char *in, char *out, size_t n) {
    size_t o = 0;
    while (*in && o + 1 < n) {
        if (*in == '%' && in[1] && in[2]) {
            char d[3] = {in[1], in[2], 0};
            out[o++] = (char)strtol(d, NULL, 16);
            in += 3;
        } else out[o++] = *in++;
    }
    out[o] = 0;
}

/* variant appenders */
static void v_str(DBusMessageIter *it, const char *s) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
    dbus_message_iter_close_container(it, &v);
}
static void v_bool(DBusMessageIter *it, int b) {
    DBusMessageIter v;
    dbus_bool_t bb = b;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &bb);
    dbus_message_iter_close_container(it, &v);
}
static void v_i64(DBusMessageIter *it, long long x) {
    DBusMessageIter v;
    dbus_int64_t xx = (dbus_int64_t)x;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "x", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT64, &xx);
    dbus_message_iter_close_container(it, &v);
}
static void v_int(DBusMessageIter *it, int i) {
    DBusMessageIter v;
    dbus_int32_t ii = i;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "i", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &ii);
    dbus_message_iter_close_container(it, &v);
}
static void v_double(DBusMessageIter *it, double d) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "d", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &d);
    dbus_message_iter_close_container(it, &v);
}
static void v_objpath(DBusMessageIter *it, const char *o) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "o", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &o);
    dbus_message_iter_close_container(it, &v);
}
static void v_strarr(DBusMessageIter *it, const char **arr, int n) {
    DBusMessageIter v, a;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &a);
    for (int i = 0; i < n; i++) dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &arr[i]);
    dbus_message_iter_close_container(&v, &a);
    dbus_message_iter_close_container(it, &v);
}

/* dict-entry appenders for a{sv} */
static void d_str(DBusMessageIter *d, const char *k, const char *s) {
    DBusMessageIter e;
    dbus_message_iter_open_container(d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    v_str(&e, s);
    dbus_message_iter_close_container(d, &e);
}
static void d_i64(DBusMessageIter *d, const char *k, long long x) {
    DBusMessageIter e;
    dbus_message_iter_open_container(d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    v_i64(&e, x);
    dbus_message_iter_close_container(d, &e);
}

static void append_metadata(DBusMessageIter *v) {
    Player *p = mp;
    char id[128], path[LV_MAX_PATH] = "", url[LV_MAX_PATH * 3] = "";
    char title[256] = "", artist[256] = "", album[256] = "";
    int tnum = 0;
    long long len_us = 0;
    pthread_mutex_lock(&p->mtx);
    if (p->cur >= 0 && p->cur < p->pl->count) {
        Track *t = &p->pl->items[p->cur];
        lv_strlcpy(path, t->path, sizeof path);
        lv_strlcpy(title, t->tags.title, sizeof title);
        lv_strlcpy(artist, t->tags.artist, sizeof artist);
        lv_strlcpy(album, t->tags.album, sizeof album);
        tnum = t->tags.track;
        file_uri(t->path, url, sizeof url);
        if (t->duration > 0) len_us = (long long)t->duration * 1000000;
    }
    pthread_mutex_unlock(&p->mtx);
    if (!len_us) {
        long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
        long long len = endf - p->track_start;
        if (len > 0 && p->rate > 0) len_us = len * 1000000 / p->rate;
    }
    if (!title[0] && path[0]) lv_pretty_name(path, title, sizeof title);

    DBusMessageIter d;
    dbus_message_iter_open_container(v, DBUS_TYPE_ARRAY, "{sv}", &d);
    track_id(p, id, sizeof id);
    {
        DBusMessageIter e;
        const char *k = "mpris:trackid";
        dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
        v_objpath(&e, id);
        dbus_message_iter_close_container(&d, &e);
    }
    if (title[0]) d_str(&d, "xesam:title", title);
    if (artist[0]) {
        const char *one = artist;
        DBusMessageIter e, va, a;
        const char *k = "xesam:artist";
        dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "as", &va);
        dbus_message_iter_open_container(&va, DBUS_TYPE_ARRAY, "s", &a);
        dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &one);
        dbus_message_iter_close_container(&va, &a);
        dbus_message_iter_close_container(&e, &va);
        dbus_message_iter_close_container(&d, &e);
    }
    if (album[0]) d_str(&d, "xesam:album", album);
    if (tnum > 0) {
        DBusMessageIter e;
        const char *k = "xesam:tracknumber";
        dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
        v_int(&e, tnum);
        dbus_message_iter_close_container(&d, &e);
    }
    if (url[0]) d_str(&d, "xesam:url", url);
    if (len_us) d_i64(&d, "mpris:length", len_us);
    dbus_message_iter_close_container(v, &d);
}

static int prop_known(const char *iface, const char *name) {
    if (!strcmp(iface, IF_ROOT)) {
        for (size_t i = 0; i < sizeof ROOT_PROPS / sizeof ROOT_PROPS[0]; i++)
            if (!strcmp(name, ROOT_PROPS[i])) return 1;
    } else if (!strcmp(iface, IF_PLAYER)) {
        for (size_t i = 0; i < sizeof PLAYER_PROPS / sizeof PLAYER_PROPS[0]; i++)
            if (!strcmp(name, PLAYER_PROPS[i])) return 1;
    }
    return 0;
}

static void append_prop(DBusMessageIter *it, const char *iface, const char *name) {
    Player *p = mp;
    if (!strcmp(iface, IF_ROOT)) {
        if (!strcmp(name, "CanQuit")) v_bool(it, 1);
        else if (!strcmp(name, "CanRaise")) v_bool(it, 1);
        else if (!strcmp(name, "HasTrackList")) v_bool(it, 0);
        else if (!strcmp(name, "Identity")) v_str(it, "levari");
        else if (!strcmp(name, "DesktopEntry")) v_str(it, "levari");
        else if (!strcmp(name, "SupportedUriSchemes")) {
            const char *u[] = {"file"};
            v_strarr(it, u, 1);
        } else {
            const char *m[] = {"audio/mpeg", "audio/ogg", "audio/opus", "audio/flac",
                               "audio/x-wav", "audio/x-mod", "audio/aac", "audio/mp4",
                               "audio/x-ms-wma"};
            v_strarr(it, m, 9);
        }
        return;
    }
    if (!strcmp(name, "PlaybackStatus"))
        v_str(it, p->state == ST_PLAY ? "Playing" : p->state == ST_PAUSE ? "Paused" : "Stopped");
    else if (!strcmp(name, "LoopStatus"))
        v_str(it, p->repeat == 2 ? "Track" : p->repeat == 1 ? "Playlist" : "None");
    else if (!strcmp(name, "Rate") || !strcmp(name, "MinimumRate") || !strcmp(name, "MaximumRate"))
        v_double(it, 1.0);
    else if (!strcmp(name, "Metadata")) {
        DBusMessageIter v;
        dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a{sv}", &v);
        append_metadata(&v);
        dbus_message_iter_close_container(it, &v);
    } else if (!strcmp(name, "Position"))
        v_i64(it, pos_us(p));
    else if (!strcmp(name, "CanGoNext") || !strcmp(name, "CanGoPrevious"))
        v_bool(it, p->pl->count > 0);
    else
        v_bool(it, 1); /* CanPlay/CanPause/CanSeek/CanControl */
}

/* append one {name, variant-of-value} entry to an a{sv} container */
static void append_prop_dict(DBusMessageIter *d, const char *iface, const char *name) {
    DBusMessageIter e;
    dbus_message_iter_open_container(d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name);
    append_prop(&e, iface, name);
    dbus_message_iter_close_container(d, &e);
}

static void reply_ok(DBusConnection *c, DBusMessage *m) {
    DBusMessage *r = dbus_message_new_method_return(m);
    dbus_connection_send(c, r, NULL);
    dbus_connection_flush(c);
    dbus_message_unref(r);
}

static void reply_err(DBusConnection *c, DBusMessage *m, const char *name, const char *msg) {
    DBusMessage *r = dbus_message_new_error(m, name, msg);
    dbus_connection_send(c, r, NULL);
    dbus_connection_flush(c);
    dbus_message_unref(r);
}

static void seek_rel(double sec) {
    Player *p = mp;
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    if (endf <= base || !p->rate) return;
    double cur = (double)(p->pos_frames - base) / p->rate;
    pl_post_seek(p, ((cur + sec) * p->rate) / (double)(endf - base));
}

static void seek_abs(double sec) {
    Player *p = mp;
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    if (endf <= base || !p->rate) return;
    pl_post_seek(p, (sec * p->rate) / (double)(endf - base));
}

static DBusHandlerResult handle_props(DBusConnection *c, DBusMessage *m) {
    const char *member = dbus_message_get_member(m);
    DBusMessageIter it;
    if (!dbus_message_iter_init(m, &it)) {
        reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "no args");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
        reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad interface arg");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    const char *iface;
    dbus_message_iter_get_basic(&it, &iface);

    if (!strcmp(member, "GetAll")) {
        DBusMessage *r = dbus_message_new_method_return(m);
        DBusMessageIter ri, d;
        dbus_message_iter_init_append(r, &ri);
        dbus_message_iter_open_container(&ri, DBUS_TYPE_ARRAY, "{sv}", &d);
        if (!strcmp(iface, IF_ROOT))
            for (size_t i = 0; i < sizeof ROOT_PROPS / sizeof ROOT_PROPS[0]; i++)
                append_prop_dict(&d, IF_ROOT, ROOT_PROPS[i]);
        else if (!strcmp(iface, IF_PLAYER))
            for (size_t i = 0; i < sizeof PLAYER_PROPS / sizeof PLAYER_PROPS[0]; i++)
                append_prop_dict(&d, IF_PLAYER, PLAYER_PROPS[i]);
        dbus_message_iter_close_container(&ri, &d);
        dbus_connection_send(c, r, NULL);
        dbus_connection_flush(c);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "Get")) {
        reply_err(c, m, "org.freedesktop.DBus.Error.UnknownMethod", member);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
        reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad property arg");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    const char *name;
    dbus_message_iter_get_basic(&it, &name);
    if (!prop_known(iface, name)) {
        reply_err(c, m, "org.freedesktop.DBus.Error.UnknownProperty", name);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    DBusMessage *r = dbus_message_new_method_return(m);
    DBusMessageIter ri;
    dbus_message_iter_init_append(r, &ri);
    append_prop(&ri, iface, name);
    dbus_connection_send(c, r, NULL);
    dbus_connection_flush(c);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static int read_i64(DBusMessageIter *it, long long *out) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_INT64) return 0;
    dbus_int64_t v;
    dbus_message_iter_get_basic(it, &v);
    *out = (long long)v;
    return 1;
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *u) {
    (void)u;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *path = dbus_message_get_path(m);
    if (!path || strcmp(path, MP_PATH)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (!member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
        if (strcmp(member, "Introspect")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        DBusMessage *r = dbus_message_new_method_return(m);
        DBusMessageIter it;
        const char *xml = MP_XML;
        dbus_message_iter_init_append(r, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &xml);
        dbus_connection_send(c, r, NULL);
        dbus_connection_flush(c);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (iface && !strcmp(iface, "org.freedesktop.DBus.Properties"))
        return handle_props(c, m);

    /* unqualified members count as Player methods per the MPRIS spec */
    int is_player = !iface || !strcmp(iface, IF_PLAYER);
    int is_root = iface && !strcmp(iface, IF_ROOT);
    if (!is_player && !is_root) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (is_root) {
        if (!strcmp(member, "Raise")) {
            raise_req = 1;
            reply_ok(c, m);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!strcmp(member, "Quit")) {
            SDL_Event q;
            q.type = SDL_QUIT;
            SDL_PushEvent(&q); /* thread-safe; quits every window */
            reply_ok(c, m);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    if (!is_player) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!strcmp(member, "Next")) { pl_post(mp, CMD_NEXT, 0); reply_ok(c, m); return DBUS_HANDLER_RESULT_HANDLED; }
    if (!strcmp(member, "Previous")) { pl_post(mp, CMD_PREV, 0); reply_ok(c, m); return DBUS_HANDLER_RESULT_HANDLED; }
    if (!strcmp(member, "PlayPause")) { pl_post(mp, CMD_TOGGLE_PAUSE, 0); reply_ok(c, m); return DBUS_HANDLER_RESULT_HANDLED; }
    if (!strcmp(member, "Play")) {
        if (mp->state == ST_PAUSE) pl_post(mp, CMD_TOGGLE_PAUSE, 0);
        else pl_post(mp, CMD_PLAY, mp->cur >= 0 ? mp->cur : 0);
        reply_ok(c, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "Pause")) {
        if (mp->state == ST_PLAY) pl_post(mp, CMD_TOGGLE_PAUSE, 0);
        reply_ok(c, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "Stop")) { pl_post(mp, CMD_STOP, 0); reply_ok(c, m); return DBUS_HANDLER_RESULT_HANDLED; }

    DBusMessageIter it;
    if (!strcmp(member, "Seek") || !strcmp(member, "SeekForward") || !strcmp(member, "SeekBackward")) {
        long long off = 0;
        if (!dbus_message_iter_init(m, &it) || !read_i64(&it, &off)) {
            reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad offset");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        double sec = (double)off / 1e6;
        seek_rel(!strcmp(member, "SeekBackward") ? -sec : sec);
        reply_ok(c, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "SetPosition")) {
        if (!dbus_message_iter_init(m, &it)) {
            reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "no args");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_next(&it); /* TrackId: our ids are informational */
        long long pos = 0;
        if (!read_i64(&it, &pos)) {
            reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad position");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        seek_abs((double)pos / 1e6);
        reply_ok(c, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "GetPosition")) {
        DBusMessage *r = dbus_message_new_method_return(m);
        DBusMessageIter ri;
        dbus_int64_t pos = (dbus_int64_t)pos_us(mp);
        dbus_message_iter_init_append(r, &ri);
        dbus_message_iter_append_basic(&ri, DBUS_TYPE_INT64, &pos);
        dbus_connection_send(c, r, NULL);
        dbus_connection_flush(c);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "OpenUri")) {
        if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
            reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad uri");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        const char *uri;
        dbus_message_iter_get_basic(&it, &uri);
        const char *pfx = "file://";
        if (strncmp(uri, pfx, 7)) {
            reply_err(c, m, "org.mpris.MediaPlayer2.Error.NotSupported", "only file:// uris");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        const char *fpath = uri + 7;
        if (!*fpath) {
            reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "empty uri");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        char path[LV_MAX_PATH];
        uri_unescape(fpath, path, sizeof path);
        pthread_mutex_lock(&mp->mtx);
        pl_add(mp->pl, path);
        lib_fill_playlist(mp->pl);
        int idx = mp->pl->count - 1;
        pthread_mutex_unlock(&mp->mtx);
        if (idx < 0) {
            reply_err(c, m, "org.mpris.MediaPlayer2.Error.NotAllowed", "cannot add");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        pl_post(mp, CMD_PLAY, idx);
        reply_ok(c, m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void emit_changed(const char *iface, const char **names, int n) {
    DBusMessage *s = dbus_message_new_signal(MP_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!s) return;
    DBusMessageIter it, d, inv;
    dbus_message_iter_init_append(s, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &d);
    for (int i = 0; i < n; i++)
        append_prop_dict(&d, iface, names[i]);
    dbus_message_iter_close_container(&it, &d);
    const char *isig = "s";
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, isig, &inv);
    dbus_message_iter_close_container(&it, &inv);
    dbus_connection_send(bus, s, NULL);
    dbus_connection_flush(bus);
    dbus_message_unref(s);
}

static unsigned long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
}

/* diff the player state against the last snapshot and push the matching
   PropertiesChanged signals; Position refreshes once a second while playing */
static void check_changes(void) {
    static int last_state = -1, last_cur = -2, last_repeat = -1, last_count = -1;
    static unsigned long long last_pos_ms;
    Player *p = mp;
    int st = p->state, cur = p->cur, rep = p->repeat;
    pthread_mutex_lock(&p->mtx);
    int count = p->pl->count;
    pthread_mutex_unlock(&p->mtx);
    if (st != last_state) {
        last_state = st;
        const char *n[] = {"PlaybackStatus"};
        emit_changed(IF_PLAYER, n, 1);
    }
    if (cur != last_cur) {
        last_cur = cur;
        const char *n[] = {"Metadata"};
        emit_changed(IF_PLAYER, n, 1);
    }
    if (rep != last_repeat) {
        last_repeat = rep;
        const char *n[] = {"LoopStatus"};
        emit_changed(IF_PLAYER, n, 1);
    }
    if (count != last_count) {
        last_count = count;
        const char *n[] = {"CanGoNext", "CanGoPrevious"};
        emit_changed(IF_PLAYER, n, 2);
    }
    if (st == ST_PLAY) {
        unsigned long long t = now_ms();
        if (t - last_pos_ms >= 1000) {
            last_pos_ms = t;
            const char *n[] = {"Position"};
            emit_changed(IF_PLAYER, n, 1);
        }
    }
}

static void *mp_thread(void *arg) {
    (void)arg;
    while (!mp_quit) {
        dbus_connection_read_write(bus, 200);
        while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
        check_changes();
    }
    return NULL;
}

int mpris_start(Player *p) {
    mp = p;
    DBusError err;
    dbus_error_init(&err);
    bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "levari: mpris: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    int rr = dbus_bus_request_name(bus, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "levari: mpris: %s\n", err.message);
        dbus_error_free(&err);
        dbus_connection_unref(bus);
        bus = NULL;
        return -1;
    }
    if (rr != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "levari: another instance owns the MPRIS name; disabled\n");
        dbus_connection_unref(bus);
        bus = NULL;
        return -1;
    }
    dbus_connection_add_filter(bus, filter, NULL, NULL);
    mp_quit = 0;
    pthread_create(&mp_th, NULL, mp_thread, NULL);
    return 0;
}

void mpris_stop(void) {
    if (!bus) return;
    mp_quit = 1;
    pthread_join(mp_th, NULL);
    /* shared connection from dbus_bus_get: unref only; close() on a shared
       connection is a libdbus misuse that abort()s */
    dbus_connection_unref(bus);
    bus = NULL;
}

int mpris_take_raise(void) {
    int r = raise_req;
    raise_req = 0;
    return r;
}

#endif
