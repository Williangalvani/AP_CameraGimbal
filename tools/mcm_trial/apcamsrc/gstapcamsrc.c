/* apcamsrc: camera-app's encoded video for GStreamer, read from its frame
 * socket (camera_app/include/camera_app/frame_socket.h) instead of its RTSP.
 *
 *   gst-launch-1.0 apcamsrc stream=0 ! h264parse ! ...
 *
 * Buffers are whole access units in Annex B, timestamped from the encoder's
 * clock mapped onto the pipeline's running time. A force-key-unit event from
 * downstream (a new WebRTC peer, an RTCP PLI) asks camera-app for an IDR.
 * When camera-app goes away the element reconnects every half second and
 * marks the next buffer as a discontinuity; the caps follow the stream. */
#define PACKAGE "apcam"

#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET "@apcam/video"
#define DEFAULT_STREAM 0
#define HELLO_SIZE 8
#define INFO_SIZE 16
#define HEADER_SIZE 16
#define FRAME_KEY 1u
#define FRAME_DISCONT 2u
#define MAX_FRAME (32u * 1024u * 1024u)
#define RECONNECT_INTERVAL (500 * GST_MSECOND)
#define STALL_TIMEOUT (3 * GST_SECOND)
#define DRIFT_WINDOW (10 * GST_SECOND)

GST_DEBUG_CATEGORY_STATIC(apcamsrc_debug);
#define GST_CAT_DEFAULT apcamsrc_debug

#define GST_TYPE_APCAM_SRC (gst_apcam_src_get_type())
G_DECLARE_FINAL_TYPE(GstApcamSrc, gst_apcam_src, GST, APCAM_SRC, GstPushSrc)

struct _GstApcamSrc {
    GstPushSrc parent;

    gchar *socket_name;
    guint stream;

    GstPoll *poll;
    GstPollFD pollfd;
    gint fd;
    GstCaps *caps;
    gboolean discont;
    gint keyframe_wanted;
    gboolean warned_unavailable;

    /* running time = encoder pts + offset, kept at the earliest arrival */
    gboolean have_offset;
    GstClockTimeDiff offset;
    GstClockTime window_start;
    GstClockTimeDiff window_lateness;

    GstClockTime position; /* last frame's timestamp, under the object lock */
};

enum { PROP_0, PROP_SOCKET, PROP_STREAM };

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-h264, stream-format=(string)byte-stream, alignment=(string)au; "
                    "video/x-h265, stream-format=(string)byte-stream, alignment=(string)au"));

G_DEFINE_TYPE(GstApcamSrc, gst_apcam_src, GST_TYPE_PUSH_SRC)

static guint16 get_u16(const guint8 *p) { return (guint16)(p[0] | p[1] << 8); }
static guint32 get_u32(const guint8 *p)
{
    return (guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16 | (guint32)p[3] << 24;
}

static void disconnect(GstApcamSrc *self)
{
    if (self->fd < 0) return;
    gst_poll_remove_fd(self->poll, &self->pollfd);
    close(self->fd);
    self->fd = -1;
    gst_poll_fd_init(&self->pollfd);
    self->discont = TRUE;
    self->have_offset = FALSE;
}

typedef enum { READ_OK, READ_CLOSED, READ_FLUSHING } ReadResult;

static ReadResult read_full(GstApcamSrc *self, guint8 *out, gsize length)
{
    while (length > 0) {
        const gint ready = gst_poll_wait(self->poll, STALL_TIMEOUT);
        if (ready < 0) {
            if (errno == EBUSY) return READ_FLUSHING;
            if (errno == EINTR || errno == EAGAIN) continue;
            GST_WARNING_OBJECT(self, "poll failed: %s", g_strerror(errno));
            return READ_CLOSED;
        }
        if (ready == 0) {
            GST_WARNING_OBJECT(self, "no video for %" GST_TIME_FORMAT, GST_TIME_ARGS(STALL_TIMEOUT));
            return READ_CLOSED;
        }
        const ssize_t bytes = recv(self->fd, out, length, MSG_DONTWAIT);
        if (bytes == 0) {
            GST_INFO_OBJECT(self, "camera-app closed the frame socket");
            return READ_CLOSED;
        }
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            GST_WARNING_OBJECT(self, "read failed: %s", g_strerror(errno));
            return READ_CLOSED;
        }
        out += bytes;
        length -= (gsize)bytes;
    }
    return READ_OK;
}

static GstCaps *caps_from_info(const guint8 *info)
{
    const guint codec = info[6];
    const guint width = get_u16(info + 8), height = get_u16(info + 10), rate = get_u16(info + 12);
    if ((codec != 1 && codec != 2) || width == 0 || height == 0 || rate == 0) return NULL;
    return gst_caps_new_simple(codec == 2 ? "video/x-h265" : "video/x-h264",
                               "stream-format", G_TYPE_STRING, "byte-stream",
                               "alignment", G_TYPE_STRING, "au",
                               "width", G_TYPE_INT, (gint)width,
                               "height", G_TYPE_INT, (gint)height,
                               "framerate", GST_TYPE_FRACTION, (gint)rate, 1, NULL);
}

/* One attempt. Returns FALSE when camera-app is not there (yet). */
static gboolean try_connect(GstApcamSrc *self, GstCaps **caps)
{
    struct sockaddr_un address = {0};
    const gsize name_length = strlen(self->socket_name);
    if (name_length < 2 || name_length >= sizeof(address.sun_path)) return FALSE;
    address.sun_family = AF_UNIX;
    gboolean abstract = self->socket_name[0] == '@';
    if (abstract) memcpy(address.sun_path + 1, self->socket_name + 1, name_length - 1);
    else memcpy(address.sun_path, self->socket_name, name_length);
    const socklen_t address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length + (abstract ? 0 : 1));

    const gint fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return FALSE;
    if (connect(fd, (const struct sockaddr *)&address, address_length) < 0) {
        if (!self->warned_unavailable) {
            GST_WARNING_OBJECT(self, "cannot connect to %s: %s; retrying", self->socket_name,
                               g_strerror(errno));
            self->warned_unavailable = TRUE;
        }
        close(fd);
        return FALSE;
    }
    const guint8 hello[HELLO_SIZE] = {'A', 'P', 'V', 'R', 1, (guint8)self->stream, 0, 0};
    if (send(fd, hello, sizeof(hello), MSG_NOSIGNAL) != (ssize_t)sizeof(hello)) {
        close(fd);
        return FALSE;
    }
    self->fd = fd;
    self->pollfd.fd = fd;
    gst_poll_add_fd(self->poll, &self->pollfd);
    gst_poll_fd_ctl_read(self->poll, &self->pollfd, TRUE);

    guint8 info[INFO_SIZE];
    if (read_full(self, info, sizeof(info)) != READ_OK) {
        disconnect(self);
        return FALSE;
    }
    if (memcmp(info, "APVS", 4) != 0 || info[4] != 1 || info[5] != 0 ||
        (*caps = caps_from_info(info)) == NULL) {
        GST_WARNING_OBJECT(self, "camera-app refused stream %u (status %u)", self->stream,
                           (guint)info[5]);
        disconnect(self);
        return FALSE;
    }
    self->warned_unavailable = FALSE;
    GST_INFO_OBJECT(self, "connected to %s stream %u: %" GST_PTR_FORMAT, self->socket_name,
                    self->stream, *caps);
    return TRUE;
}

static GstClockTime running_time_now(GstApcamSrc *self)
{
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(self));
    if (clock == NULL) return GST_CLOCK_TIME_NONE;
    const GstClockTime now = gst_clock_get_time(clock);
    const GstClockTime base = gst_element_get_base_time(GST_ELEMENT(self));
    gst_object_unref(clock);
    return now > base ? now - base : 0;
}

/* Keeps the encoder's frame spacing while following the pipeline clock: the
 * offset moves earlier as soon as a frame would land in the future, and later
 * once per window by the smallest lateness seen, which absorbs clock drift. */
static GstClockTime timestamp(GstApcamSrc *self, guint64 pts_us)
{
    const GstClockTime now = running_time_now(self);
    if (!GST_CLOCK_TIME_IS_VALID(now)) return GST_CLOCK_TIME_NONE;
    const GstClockTimeDiff pts = (GstClockTimeDiff)(pts_us * GST_USECOND);
    if (!self->have_offset) {
        self->offset = (GstClockTimeDiff)now - pts;
        self->have_offset = TRUE;
        self->window_start = now;
        self->window_lateness = G_MAXINT64;
    }
    GstClockTimeDiff lateness = (GstClockTimeDiff)now - (pts + self->offset);
    if (lateness < 0) {
        self->offset += lateness;
        lateness = 0;
    }
    self->window_lateness = MIN(self->window_lateness, lateness);
    if (now - self->window_start >= DRIFT_WINDOW) {
        if (self->window_lateness > 0) self->offset += self->window_lateness;
        self->window_start = now;
        self->window_lateness = G_MAXINT64;
    }
    const GstClockTimeDiff time = pts + self->offset;
    return time > 0 ? (GstClockTime)time : 0;
}

static GstFlowReturn gst_apcam_src_create(GstPushSrc *push, GstBuffer **result)
{
    GstApcamSrc *self = GST_APCAM_SRC(push);
    for (;;) {
        if (self->fd < 0) {
            GstCaps *caps = NULL;
            if (!try_connect(self, &caps)) {
                if (gst_poll_wait(self->poll, RECONNECT_INTERVAL) < 0 && errno == EBUSY) {
                    return GST_FLOW_FLUSHING;
                }
                continue;
            }
            GST_OBJECT_LOCK(self);
            const gboolean changed = self->caps == NULL || !gst_caps_is_equal(self->caps, caps);
            if (changed) gst_caps_replace(&self->caps, caps);
            GST_OBJECT_UNLOCK(self);
            if (changed) {
                if (!gst_base_src_set_caps(GST_BASE_SRC(self), caps)) {
                    GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, (NULL),
                                      ("downstream refused %" GST_PTR_FORMAT, caps));
                    gst_caps_unref(caps);
                    return GST_FLOW_NOT_NEGOTIATED;
                }
            }
            gst_caps_unref(caps);
        }
        if (g_atomic_int_compare_and_exchange(&self->keyframe_wanted, 1, 0)) {
            (void)send(self->fd, "K", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
        }

        guint8 header[HEADER_SIZE];
        ReadResult read = read_full(self, header, sizeof(header));
        if (read == READ_FLUSHING) return GST_FLOW_FLUSHING;
        const guint32 length = read == READ_OK ? get_u32(header) : 0;
        if (read == READ_OK && (length == 0 || length > MAX_FRAME)) {
            GST_WARNING_OBJECT(self, "bad frame length %u", length);
            read = READ_CLOSED;
        }
        if (read != READ_OK) {
            disconnect(self);
            continue;
        }
        const guint32 flags = get_u32(header + 4);
        const guint64 pts_us = (guint64)get_u32(header + 8) | (guint64)get_u32(header + 12) << 32;

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, length, NULL);
        GstMapInfo map;
        if (buffer == NULL || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            if (buffer != NULL) gst_buffer_unref(buffer);
            return GST_FLOW_ERROR;
        }
        read = read_full(self, map.data, length);
        gst_buffer_unmap(buffer, &map);
        if (read != READ_OK) {
            gst_buffer_unref(buffer);
            if (read == READ_FLUSHING) return GST_FLOW_FLUSHING;
            disconnect(self);
            continue;
        }
        GST_BUFFER_PTS(buffer) = timestamp(self, pts_us);
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_OBJECT_LOCK(self);
        self->position = GST_BUFFER_PTS(buffer);
        GST_OBJECT_UNLOCK(self);
        GST_LOG_OBJECT(self, "frame pts %" G_GUINT64_FORMAT " us, %u bytes%s, pts %" GST_TIME_FORMAT,
                       pts_us, length, (flags & FRAME_KEY) ? ", key" : "",
                       GST_TIME_ARGS(GST_BUFFER_PTS(buffer)));
        if (!(flags & FRAME_KEY)) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        if ((flags & FRAME_DISCONT) || self->discont) {
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
            self->discont = FALSE;
        }
        *result = buffer;
        return GST_FLOW_OK;
    }
}

static gboolean gst_apcam_src_event(GstBaseSrc *base, GstEvent *event)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CUSTOM_UPSTREAM &&
        gst_event_has_name(event, "GstForceKeyUnit")) {
        GST_DEBUG_OBJECT(self, "key frame requested downstream");
        g_atomic_int_set(&self->keyframe_wanted, 1);
        return TRUE;
    }
    return GST_BASE_SRC_CLASS(gst_apcam_src_parent_class)->event(base, event);
}

static GstCaps *gst_apcam_src_get_caps(GstBaseSrc *base, GstCaps *filter)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    GstCaps *caps = NULL;
    GST_OBJECT_LOCK(self);
    if (self->caps != NULL) caps = gst_caps_ref(self->caps);
    GST_OBJECT_UNLOCK(self);
    if (caps == NULL) caps = gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(base));
    if (filter != NULL) {
        GstCaps *both = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(caps);
        caps = both;
    }
    return caps;
}

/* GstBaseSrc leaves POSITION unanswered for a live source, and MCM waits 3 s
 * for it to advance before adding a WebRTC session to an idle stream. */
static gboolean gst_apcam_src_query(GstBaseSrc *base, GstQuery *query)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    if (GST_QUERY_TYPE(query) == GST_QUERY_POSITION) {
        GstFormat format;
        gst_query_parse_position(query, &format, NULL);
        GST_OBJECT_LOCK(self);
        const GstClockTime position = self->position;
        GST_OBJECT_UNLOCK(self);
        if (format == GST_FORMAT_TIME && GST_CLOCK_TIME_IS_VALID(position)) {
            gst_query_set_position(query, GST_FORMAT_TIME, (gint64)position);
            return TRUE;
        }
    }
    return GST_BASE_SRC_CLASS(gst_apcam_src_parent_class)->query(base, query);
}

/* Caps come with the stream, from create(). */
static gboolean gst_apcam_src_negotiate(GstBaseSrc *base)
{
    (void)base;
    return TRUE;
}

static gboolean gst_apcam_src_start(GstBaseSrc *base)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    self->poll = gst_poll_new(TRUE);
    if (self->poll == NULL) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ, (NULL), ("cannot create a poll"));
        return FALSE;
    }
    gst_poll_fd_init(&self->pollfd);
    self->fd = -1;
    self->discont = TRUE;
    self->have_offset = FALSE;
    self->warned_unavailable = FALSE;
    g_atomic_int_set(&self->keyframe_wanted, 0);
    GST_OBJECT_LOCK(self);
    self->position = GST_CLOCK_TIME_NONE;
    GST_OBJECT_UNLOCK(self);
    return TRUE;
}

static gboolean gst_apcam_src_stop(GstBaseSrc *base)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    disconnect(self);
    GST_OBJECT_LOCK(self);
    gst_caps_replace(&self->caps, NULL);
    GST_OBJECT_UNLOCK(self);
    if (self->poll != NULL) gst_poll_free(self->poll);
    self->poll = NULL;
    return TRUE;
}

static gboolean gst_apcam_src_unlock(GstBaseSrc *base)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    gst_poll_set_flushing(self->poll, TRUE);
    return TRUE;
}

static gboolean gst_apcam_src_unlock_stop(GstBaseSrc *base)
{
    GstApcamSrc *self = GST_APCAM_SRC(base);
    gst_poll_set_flushing(self->poll, FALSE);
    return TRUE;
}

static void gst_apcam_src_set_property(GObject *object, guint id, const GValue *value,
                                       GParamSpec *spec)
{
    GstApcamSrc *self = GST_APCAM_SRC(object);
    switch (id) {
    case PROP_SOCKET:
        g_free(self->socket_name);
        self->socket_name = g_value_dup_string(value);
        if (self->socket_name == NULL) self->socket_name = g_strdup(DEFAULT_SOCKET);
        break;
    case PROP_STREAM:
        self->stream = g_value_get_uint(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

static void gst_apcam_src_get_property(GObject *object, guint id, GValue *value,
                                       GParamSpec *spec)
{
    GstApcamSrc *self = GST_APCAM_SRC(object);
    switch (id) {
    case PROP_SOCKET:
        g_value_set_string(value, self->socket_name);
        break;
    case PROP_STREAM:
        g_value_set_uint(value, self->stream);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

static void gst_apcam_src_finalize(GObject *object)
{
    GstApcamSrc *self = GST_APCAM_SRC(object);
    g_free(self->socket_name);
    G_OBJECT_CLASS(gst_apcam_src_parent_class)->finalize(object);
}

static void gst_apcam_src_class_init(GstApcamSrcClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass *base_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass *push_class = GST_PUSH_SRC_CLASS(klass);

    object_class->set_property = gst_apcam_src_set_property;
    object_class->get_property = gst_apcam_src_get_property;
    object_class->finalize = gst_apcam_src_finalize;
    g_object_class_install_property(
        object_class, PROP_SOCKET,
        g_param_spec_string("socket", "Socket",
                            "camera-app's frame socket; a leading @ is the abstract namespace",
                            DEFAULT_SOCKET,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                GST_PARAM_MUTABLE_READY));
    g_object_class_install_property(
        object_class, PROP_STREAM,
        g_param_spec_uint("stream", "Stream", "0 for the main stream, 1 for the sub stream", 0, 1,
                          DEFAULT_STREAM,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                              GST_PARAM_MUTABLE_READY));

    gst_element_class_add_static_pad_template(element_class, &src_template);
    gst_element_class_set_static_metadata(element_class, "AP_CameraGimbal source",
                                          "Source/Video",
                                          "Encoded video from camera-app's frame socket",
                                          "AP_CameraGimbal");

    base_class->start = gst_apcam_src_start;
    base_class->stop = gst_apcam_src_stop;
    base_class->unlock = gst_apcam_src_unlock;
    base_class->unlock_stop = gst_apcam_src_unlock_stop;
    base_class->get_caps = gst_apcam_src_get_caps;
    base_class->negotiate = gst_apcam_src_negotiate;
    base_class->event = gst_apcam_src_event;
    base_class->query = gst_apcam_src_query;
    push_class->create = gst_apcam_src_create;
}

static void gst_apcam_src_init(GstApcamSrc *self)
{
    self->socket_name = g_strdup(DEFAULT_SOCKET);
    self->stream = DEFAULT_STREAM;
    self->fd = -1;
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(apcamsrc_debug, "apcamsrc", 0, "camera-app frame socket source");
    return gst_element_register(plugin, "apcamsrc", GST_RANK_NONE, GST_TYPE_APCAM_SRC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, apcam,
                  "camera-app frame socket source", plugin_init, "1.0", "GPL",
                  "AP_CameraGimbal", "https://github.com/ArduPilot/AP_CameraGimbal")
