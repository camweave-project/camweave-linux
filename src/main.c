#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

typedef struct {
    GMutex mutex;
    GCond frame_ready;
    GBytes *last_frame;
    gint64 last_frame_us;
    gint64 last_preview_us;
    guint64 frames;
    guint viewers;
    guint clients;
    gboolean running;
    gchar *token;
    gchar *session;
    guint quality;
    guint fps;
    GstElement *pipeline;
    guint bus_watch;
    GSocketService *service;
    GPtrArray *devices;
    gchar *html;
    GtkWidget *window;
    GtkWidget *camera_list;
    GtkWidget *host_entry;
    GtkWidget *code_entry;
    GtkWidget *preview;
    GtkWidget *status;
    GtkWidget *link;
    GtkWidget *start_button;
    GtkWidget *stop_button;
    GtkWidget *refresh_button;
} App;

typedef struct { App *app; GBytes *bytes; } PreviewUpdate;
typedef struct { App *app; GSocketConnection *connection; } Client;

static gboolean valid_code(const gchar *code) {
    if (!code) return FALSE;
    gsize len = strlen(code);
    if (len < 5 || len > 128) return FALSE;
    for (const guchar *c = (const guchar *)code; *c; c++)
        if (!(g_ascii_isalnum(*c) || *c == '-' || *c == '_')) return FALSE;
    return TRUE;
}

static gboolean valid_host(const gchar *host) {
    if (!host || !*host || strlen(host) > 253 || host[0] == '.') return FALSE;
    for (const guchar *c = (const guchar *)host; *c; c++)
        if (!(g_ascii_isalnum(*c) || *c == '-' || *c == '.')) return FALSE;
    return TRUE;
}

static gchar *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "camweave", "camera.ini", NULL);
}

static void save_config(App *app) {
    g_autofree gchar *path = config_path();
    g_autofree gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    GKeyFile *file = g_key_file_new();
    g_key_file_set_string(file, "camera", "host", gtk_editable_get_text(GTK_EDITABLE(app->host_entry)));
    g_key_file_set_string(file, "camera", "access_code", gtk_editable_get_text(GTK_EDITABLE(app->code_entry)));
    g_key_file_save_to_file(file, path, NULL);
    g_key_file_free(file);
}

static void set_status(App *app, const gchar *message) {
    gtk_label_set_text(GTK_LABEL(app->status), message);
}

static void update_link(App *app) {
    const gchar *host = gtk_editable_get_text(GTK_EDITABLE(app->host_entry));
    const gchar *code = gtk_editable_get_text(GTK_EDITABLE(app->code_entry));
    gboolean valid = valid_host(host) && valid_code(code);
    gtk_widget_set_sensitive(app->start_button, !app->running && valid && app->devices->len > 0);
    gtk_widget_set_sensitive(app->stop_button, app->running);
    gtk_widget_set_sensitive(app->camera_list, !app->running);
    gtk_widget_set_sensitive(app->code_entry, !app->running);
    gtk_widget_set_sensitive(app->refresh_button, !app->running);
    if (app->running && valid) {
        g_autofree gchar *link = g_strdup_printf("http://%s:8080/watch/%s/", host, code);
        gtk_label_set_text(GTK_LABEL(app->link), link);
    } else gtk_label_set_text(GTK_LABEL(app->link), "Start the camera to get a viewing link.");
}

static gboolean show_preview(gpointer data) {
    PreviewUpdate *update = data;
    GError *error = NULL;
    GdkTexture *texture = gdk_texture_new_from_bytes(update->bytes, &error);
    if (texture) {
        gtk_picture_set_paintable(GTK_PICTURE(update->app->preview), GDK_PAINTABLE(texture));
        g_object_unref(texture);
    } else g_clear_error(&error);
    g_bytes_unref(update->bytes);
    g_free(update);
    return G_SOURCE_REMOVE;
}

static GstFlowReturn on_sample(GstAppSink *sink, gpointer data) {
    App *app = data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        GBytes *bytes = g_bytes_new(map.data, map.size);
        gint64 now = g_get_monotonic_time();
        gboolean preview = FALSE;
        g_mutex_lock(&app->mutex);
        if (app->running) {
            g_clear_pointer(&app->last_frame, g_bytes_unref);
            app->last_frame = g_bytes_ref(bytes);
            app->last_frame_us = now;
            app->frames++;
            g_cond_broadcast(&app->frame_ready);
            if (now - app->last_preview_us >= G_USEC_PER_SEC) {
                app->last_preview_us = now;
                preview = TRUE;
            }
        }
        g_mutex_unlock(&app->mutex);
        if (preview && !g_getenv("CAMWEAVE_TEST_SOURCE")) {
            PreviewUpdate *update = g_new0(PreviewUpdate, 1);
            update->app = app;
            update->bytes = g_bytes_ref(bytes);
            g_main_context_invoke(NULL, show_preview, update);
        }
        g_bytes_unref(bytes);
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void stop_pipeline(App *app) {
    if (app->bus_watch) { g_source_remove(app->bus_watch); app->bus_watch = 0; }
    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }
}

static void app_stop(App *app);

static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data) {
    (void)bus;
    App *app = data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        g_warning("Camera pipeline error: %s (%s)", error ? error->message : "unknown", debug ? debug : "no detail");
        g_autofree gchar *text = g_strdup_printf("Camera error: %s", error ? error->message : "unknown error");
        app_stop(app);
        set_status(app, text);
        g_clear_error(&error);
        g_free(debug);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean start_pipeline(App *app, const gchar *device, GError **error) {
    guint quality, fps;
    g_mutex_lock(&app->mutex);
    quality = app->quality;
    fps = app->fps;
    g_mutex_unlock(&app->mutex);
    gboolean test_source = g_strcmp0(device, "Test pattern") == 0;
    gchar *source = test_source ? g_strdup("videotestsrc is-live=true") :
        g_strdup_printf("v4l2src device=%s ! decodebin", device);
    g_autofree gchar *description = g_strdup_printf(
        "%s ! videoconvert ! videoscale ! videorate ! video/x-raw,height=%u,framerate=%u/1 "
        "! jpegenc quality=70 ! appsink name=sink emit-signals=false max-buffers=1 drop=true sync=false",
        source, quality, fps);
    g_free(source);
    app->pipeline = gst_parse_launch(description, error);
    if (!app->pipeline) return FALSE;
    GstElement *sink = gst_bin_get_by_name(GST_BIN(app->pipeline), "sink");
    if (!sink) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Camera pipeline has no JPEG sink");
        stop_pipeline(app);
        return FALSE;
    }
    GstAppSinkCallbacks callbacks = { .eos = NULL, .new_preroll = NULL, .new_sample = on_sample };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, app, NULL);
    gst_object_unref(sink);
    GstBus *bus = gst_element_get_bus(app->pipeline);
    app->bus_watch = gst_bus_add_watch(bus, bus_message, app);
    gst_object_unref(bus);
    if (gst_element_set_state(app->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unable to start the camera pipeline");
        stop_pipeline(app);
        return FALSE;
    }
    return TRUE;
}

static gboolean reconfigure(gpointer data) {
    App *app = data;
    if (!app->running) return G_SOURCE_REMOVE;
    const gchar *device = app->devices->pdata[gtk_drop_down_get_selected(GTK_DROP_DOWN(app->camera_list))];
    stop_pipeline(app);
    GError *error = NULL;
    if (!start_pipeline(app, device, &error)) {
        g_autofree gchar *text = g_strdup_printf("Cannot apply settings: %s", error ? error->message : "unknown error");
        app_stop(app);
        set_status(app, text);
        g_clear_error(&error);
    }
    return G_SOURCE_REMOVE;
}

static gboolean write_bytes(GOutputStream *out, const void *bytes, gsize size) {
    gsize written = 0;
    return g_output_stream_write_all(out, bytes, size, &written, NULL, NULL) && written == size;
}

static gboolean write_text(GOutputStream *out, const gchar *text) {
    return write_bytes(out, text, strlen(text));
}

static void reply(GOutputStream *out, guint status, const gchar *reason,
                  const gchar *content_type, const gchar *body) {
    gsize length = strlen(body);
    g_autofree gchar *header = g_strdup_printf(
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %" G_GSIZE_FORMAT "\r\n"
        "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; img-src 'self'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\nConnection: close\r\n\r\n",
        status, reason, content_type, length);
    gboolean wrote = write_text(out, header);
    if (wrote) wrote = write_bytes(out, body, length);
    if (g_getenv("CAMWEAVE_TEST_SOURCE")) g_message("HTTP %u written=%d", status, wrote);
}

static gchar *read_header(GInputStream *in) {
    gchar buffer[8193] = {0};
    gsize count = 0;
    while (count < 8192) {
        gssize read = g_input_stream_read(in, buffer + count, 1, NULL, NULL);
        if (read <= 0) return NULL;
        count += (gsize)read;
        if (count >= 4 && memcmp(buffer + count - 4, "\r\n\r\n", 4) == 0) {
            buffer[count - 4] = 0;
            return g_strdup(buffer);
        }
    }
    return NULL;
}

static gboolean parse_settings(const gchar *query, guint *quality, guint *fps) {
    if (!query) return FALSE;
    g_auto(GStrv) pairs = g_strsplit(query, "&", -1);
    if (g_strv_length(pairs) != 2) return FALSE;
    gboolean have_quality = FALSE, have_fps = FALSE;
    for (guint i = 0; i < 2; i++) {
        g_auto(GStrv) parts = g_strsplit(pairs[i], "=", -1);
        if (g_strv_length(parts) != 2 || !*parts[1]) return FALSE;
        gchar *end = NULL;
        gint64 number = g_ascii_strtoll(parts[1], &end, 10);
        if (!end || *end || number < 0 || number > 10000) return FALSE;
        if (g_strcmp0(parts[0], "quality") == 0 && !have_quality) {
            *quality = (guint)number; have_quality = TRUE;
        } else if (g_strcmp0(parts[0], "fps") == 0 && !have_fps) {
            *fps = (guint)number; have_fps = TRUE;
        } else return FALSE;
    }
    return have_quality && have_fps && (*quality == 720 || *quality == 1080) &&
           (*fps == 5 || *fps == 10 || *fps == 15);
}

static void stream_frames(App *app, GSocketConnection *connection, GOutputStream *out) {
    g_mutex_lock(&app->mutex);
    if (app->viewers >= 4) {
        g_mutex_unlock(&app->mutex);
        reply(out, 503, "Service Unavailable", "text/plain; charset=utf-8", "Maximum of 4 viewers");
        return;
    }
    app->viewers++;
    g_mutex_unlock(&app->mutex);
    if (!write_text(out, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                         "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n")) goto done;
    guint64 seen = 0;
    while (TRUE) {
        GBytes *frame = NULL;
        g_mutex_lock(&app->mutex);
        while (app->running && app->frames == seen)
            if (!g_cond_wait_until(&app->frame_ready, &app->mutex, g_get_monotonic_time() + 2 * G_USEC_PER_SEC)) break;
        gboolean running = app->running;
        if (running && app->frames != seen && app->last_frame) {
            seen = app->frames;
            frame = g_bytes_ref(app->last_frame);
        }
        g_mutex_unlock(&app->mutex);
        if (!running) break;
        if (!frame) {
            if (g_socket_condition_check(g_socket_connection_get_socket(connection), G_IO_HUP | G_IO_ERR)) break;
            continue;
        }
        gsize size;
        const guint8 *bytes = g_bytes_get_data(frame, &size);
        g_autofree gchar *part = g_strdup_printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %" G_GSIZE_FORMAT "\r\n\r\n", size);
        gboolean okay = write_text(out, part) && write_bytes(out, bytes, size) && write_text(out, "\r\n");
        g_bytes_unref(frame);
        if (!okay) break;
    }
done:
    g_mutex_lock(&app->mutex);
    app->viewers--;
    g_mutex_unlock(&app->mutex);
}

static gpointer client_thread(gpointer data) {
    Client *client = data;
    App *app = client->app;
    GSocketConnection *connection = client->connection;
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 10);
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_autofree gchar *header = read_header(in);
    if (!header) { reply(out, 431, "Request Header Fields Too Large", "text/plain; charset=utf-8", "Request too large"); goto finish; }
    g_auto(GStrv) lines = g_strsplit(header, "\r\n", -1);
    g_auto(GStrv) first = g_strsplit(lines[0], " ", -1);
    if (g_strv_length(first) != 3 || g_strcmp0(first[2], "HTTP/1.1") != 0 || first[1][0] != '/') {
        reply(out, 400, "Bad Request", "text/plain; charset=utf-8", "Bad request"); goto finish;
    }
    g_auto(GStrv) target = g_strsplit(first[1], "?", 2);
    if (g_getenv("CAMWEAVE_TEST_SOURCE")) g_message("Request %s %s", first[0], target[0]);
    g_autofree gchar *token = NULL;
    g_mutex_lock(&app->mutex);
    token = g_strdup(app->token);
    g_mutex_unlock(&app->mutex);
    g_autofree gchar *base = g_strdup_printf("/watch/%s", token ? token : "");
    g_autofree gchar *prefix = g_strconcat(base, "/", NULL);
    if (!token || (g_strcmp0(target[0], base) != 0 && !g_str_has_prefix(target[0], prefix))) {
        reply(out, 404, "Not Found", "text/plain; charset=utf-8", "Use the complete viewing link"); goto finish;
    }
    g_autofree gchar *settings = g_strconcat(base, "/settings", NULL);
    if (g_strcmp0(target[0], settings) == 0) {
        if (g_strcmp0(first[0], "POST") != 0) {
            reply(out, 405, "Method Not Allowed", "text/plain; charset=utf-8", "POST required"); goto finish;
        }
        gboolean control = FALSE;
        for (guint i = 1; lines[i]; i++) {
            if (g_ascii_strncasecmp(lines[i], "X-Camera-Control:", 17) == 0) {
                g_autofree gchar *value = g_strdup(lines[i] + 17);
                control = g_strcmp0(g_strstrip(value), "1") == 0;
            }
        }
        if (!control) { reply(out, 403, "Forbidden", "text/plain; charset=utf-8", "Control header required"); goto finish; }
        guint quality = 0, fps = 0;
        if (!parse_settings(target[1], &quality, &fps)) {
            reply(out, 400, "Bad Request", "text/plain; charset=utf-8", "Unsupported quality or FPS"); goto finish;
        }
        g_mutex_lock(&app->mutex);
        app->quality = quality;
        app->fps = fps;
        g_mutex_unlock(&app->mutex);
        g_main_context_invoke(NULL, reconfigure, app);
        reply(out, 202, "Accepted", "application/json", "{\"accepted\":true}");
        goto finish;
    }
    if (g_strcmp0(first[0], "GET") != 0) {
        reply(out, 405, "Method Not Allowed", "text/plain; charset=utf-8", "GET required"); goto finish;
    }
    if (g_strcmp0(target[0], base) == 0 || g_strcmp0(target[0], prefix) == 0) {
        g_auto(GStrv) pieces = g_strsplit(app->html, "__BASE__", -1);
        g_autofree gchar *page = g_strjoinv(base, pieces);
        reply(out, 200, "OK", "text/html; charset=utf-8", page);
    } else {
        g_autofree gchar *stream = g_strconcat(base, "/stream", NULL);
        g_autofree gchar *status = g_strconcat(base, "/status", NULL);
        if (g_strcmp0(target[0], stream) == 0) stream_frames(app, connection, out);
        else if (g_strcmp0(target[0], status) == 0) {
            guint quality, fps, viewers;
            guint64 frames;
            gint64 last_frame_us;
            gchar *session;
            g_mutex_lock(&app->mutex);
            quality = app->quality; fps = app->fps; viewers = app->viewers;
            frames = app->frames; last_frame_us = app->last_frame_us;
            session = g_strdup(app->session);
            g_mutex_unlock(&app->mutex);
            double age = last_frame_us ? (g_get_monotonic_time() - last_frame_us) / 1000000.0 : -1;
            gchar age_text[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd(age_text, sizeof age_text, "%.3f", age);
            g_autofree gchar *json = g_strdup_printf(
                "{\"quality\":%u,\"fps\":%u,\"session\":\"%s\",\"viewers\":%u,\"frames\":%" G_GUINT64_FORMAT ",\"frameAge\":%s}",
                quality, fps, session ? session : "", viewers, frames, age_text);
            g_free(session);
            reply(out, 200, "OK", "application/json", json);
        } else reply(out, 404, "Not Found", "text/plain; charset=utf-8", "Not found");
    }
finish:
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
    g_mutex_lock(&app->mutex);
    app->clients--;
    guint remaining = app->clients;
    g_cond_broadcast(&app->frame_ready);
    g_mutex_unlock(&app->mutex);
    if (g_getenv("CAMWEAVE_TEST_SOURCE")) g_message("Client done; remaining=%u", remaining);
    g_free(client);
    return NULL;
}

static gboolean on_run(GThreadedSocketService *service, GSocketConnection *connection, GObject *source, gpointer data) {
    (void)service; (void)source;
    App *app = data;
    g_mutex_lock(&app->mutex);
    gboolean allowed = app->running && app->clients < 24;
    if (allowed) app->clients++;
    guint clients = app->clients;
    g_mutex_unlock(&app->mutex);
    if (g_getenv("CAMWEAVE_TEST_SOURCE")) g_message("Incoming allowed=%d clients=%u", allowed, clients);
    if (!allowed) { g_io_stream_close(G_IO_STREAM(connection), NULL, NULL); return TRUE; }
    Client *client = g_new0(Client, 1);
    client->app = app;
    client->connection = g_object_ref(connection);
    client_thread(client);
    return TRUE;
}

static gboolean heartbeat(gpointer data) {
    App *app = data;
    g_mutex_lock(&app->mutex);
    guint clients = app->clients;
    gboolean running = app->running;
    g_mutex_unlock(&app->mutex);
    g_message("Heartbeat running=%d service=%d clients=%u", running,
              app->service ? g_socket_service_is_active(app->service) : -1, clients);
    return G_SOURCE_CONTINUE;
}

static gboolean is_capture_device(const gchar *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return FALSE;
    struct v4l2_capability info = {0};
    gboolean capture = FALSE;
    if (ioctl(fd, VIDIOC_QUERYCAP, &info) == 0) {
        guint32 capabilities = (info.capabilities & V4L2_CAP_DEVICE_CAPS) ? info.device_caps : info.capabilities;
        capture = (capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) != 0;
    }
    close(fd);
    return capture;
}

static gint device_compare(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

static void refresh_cameras(App *app) {
    if (app->running) return;
    g_ptr_array_set_size(app->devices, 0);
    if (g_strcmp0(g_getenv("CAMWEAVE_TEST_SOURCE"), "1") == 0)
        g_ptr_array_add(app->devices, g_strdup("Test pattern"));
    else {
        GDir *dir = g_dir_open("/dev", 0, NULL);
        if (dir) {
            const gchar *name;
            while ((name = g_dir_read_name(dir))) {
                if (!g_str_has_prefix(name, "video") || !name[5]) continue;
                gboolean digits = TRUE;
                for (const gchar *p = name + 5; *p; p++) if (!g_ascii_isdigit(*p)) { digits = FALSE; break; }
                if (!digits) continue;
                g_autofree gchar *path = g_build_filename("/dev", name, NULL);
                if (is_capture_device(path)) g_ptr_array_add(app->devices, g_strdup(path));
            }
            g_dir_close(dir);
        }
    }
    g_ptr_array_sort(app->devices, device_compare);
    GtkStringList *list = gtk_string_list_new(NULL);
    for (guint i = 0; i < app->devices->len; i++)
        gtk_string_list_append(list, app->devices->pdata[i]);
    gtk_drop_down_set_model(GTK_DROP_DOWN(app->camera_list), G_LIST_MODEL(list));
    g_object_unref(list);
    if (app->devices->len) gtk_drop_down_set_selected(GTK_DROP_DOWN(app->camera_list), 0);
    else set_status(app, "No accessible V4L2 camera found. Connect one and refresh.");
    update_link(app);
}

static void app_stop(App *app) {
    if (g_getenv("CAMWEAVE_TEST_SOURCE")) g_message("App stop requested");
    g_mutex_lock(&app->mutex);
    app->running = FALSE;
    g_cond_broadcast(&app->frame_ready);
    g_clear_pointer(&app->last_frame, g_bytes_unref);
    app->last_frame_us = 0;
    app->frames = 0;
    g_mutex_unlock(&app->mutex);
    if (app->service) {
        g_socket_service_stop(app->service);
        g_clear_object(&app->service);
    }
    stop_pipeline(app);
    gtk_picture_set_paintable(GTK_PICTURE(app->preview), NULL);
    set_status(app, "Camera is off");
    update_link(app);
}

static void app_start(App *app) {
    if (app->running) return;
    const gchar *host = gtk_editable_get_text(GTK_EDITABLE(app->host_entry));
    const gchar *code = gtk_editable_get_text(GTK_EDITABLE(app->code_entry));
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->camera_list));
    if (!valid_host(host) || !valid_code(code) || selected >= app->devices->len) {
        set_status(app, "Choose a camera, a reachable host and a valid access code."); return;
    }
    g_mutex_lock(&app->mutex);
    app->running = TRUE;
    app->frames = 0;
    app->last_frame_us = 0;
    g_free(app->token); app->token = g_strdup(code);
    g_free(app->session); app->session = g_uuid_string_random();
    g_mutex_unlock(&app->mutex);
    GError *error = NULL;
    const gchar *device = app->devices->pdata[selected];
    if (!start_pipeline(app, device, &error)) {
        g_autofree gchar *message = g_strdup_printf("Cannot start camera: %s", error ? error->message : "unknown error");
        app_stop(app); set_status(app, message); g_clear_error(&error); return;
    }
    app->service = G_SOCKET_SERVICE(g_threaded_socket_service_new(24));
    g_signal_connect(app->service, "run", G_CALLBACK(on_run), app);
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(app->service), 8080, NULL, &error)) {
        g_autofree gchar *message = g_strdup_printf("Cannot listen on port 8080: %s", error ? error->message : "unknown error");
        app_stop(app); set_status(app, message); g_clear_error(&error); return;
    }
    g_socket_service_start(app->service);
    save_config(app);
    set_status(app, "Camera is live on port 8080");
    update_link(app);
}

static void refresh_clicked(GtkButton *button, gpointer data) { (void)button; refresh_cameras(data); }
static void start_clicked(GtkButton *button, gpointer data) { (void)button; app_start(data); }
static void stop_clicked(GtkButton *button, gpointer data) { (void)button; app_stop(data); }
static void entry_changed(GtkEditable *editable, gpointer data) { (void)editable; update_link(data); }
static void generate_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    g_autofree gchar *code = g_uuid_string_random();
    gtk_editable_set_text(GTK_EDITABLE(app->code_entry), code);
}
static void copy_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->running || !valid_host(gtk_editable_get_text(GTK_EDITABLE(app->host_entry)))) return;
    GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(app->window));
    gdk_clipboard_set_text(clipboard, gtk_label_get_text(GTK_LABEL(app->link)));
    set_status(app, "Viewing link copied. Keep it private.");
}
static void open_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->running || !valid_host(gtk_editable_get_text(GTK_EDITABLE(app->host_entry)))) return;
    GError *error = NULL;
    g_app_info_launch_default_for_uri(gtk_label_get_text(GTK_LABEL(app->link)), NULL, &error);
    if (error) { set_status(app, error->message); g_error_free(error); }
}
static gboolean closing(GtkWindow *window, gpointer data) { (void)window; app_stop(data); return FALSE; }

static GtkWidget *label(const gchar *text, gboolean wrap) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_wrap(GTK_LABEL(widget), wrap);
    return widget;
}

static GtkWidget *card(GtkWidget *parent, const gchar *title) {
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_box_append(GTK_BOX(parent), frame);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    GtkWidget *heading = label(title, TRUE);
    gtk_widget_add_css_class(heading, "title-3");
    gtk_box_append(GTK_BOX(box), heading);
    return box;
}

static void activate(GtkApplication *application, gpointer data) {
    App *app = data;
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "CamWeave Camera");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 740, 780);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_window_set_child(GTK_WINDOW(app->window), scroll);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(body, 28); gtk_widget_set_margin_bottom(body, 28);
    gtk_widget_set_margin_start(body, 28); gtk_widget_set_margin_end(body, 28);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), body);
    GtkWidget *eyebrow = label("CAMWEAVE / CAMERA", FALSE);
    gtk_widget_add_css_class(eyebrow, "accent");
    gtk_box_append(GTK_BOX(body), eyebrow);
    GtkWidget *title = label("Give your camera a second life.", TRUE);
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(body), title);
    gtk_box_append(GTK_BOX(body), label("Use an existing Linux webcam. Watch in any browser on your private network.", TRUE));

    GtkWidget *first = card(body, "1  Choose a camera");
    app->camera_list = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_hexpand(app->camera_list, TRUE);
    gtk_box_append(GTK_BOX(first), app->camera_list);
    app->refresh_button = gtk_button_new_with_label("Refresh cameras");
    gtk_box_append(GTK_BOX(first), app->refresh_button);
    app->preview = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(app->preview), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(app->preview, -1, 240);
    gtk_box_append(GTK_BOX(first), app->preview);
    app->status = label("Camera is off", TRUE);
    gtk_box_append(GTK_BOX(first), app->status);

    GtkWidget *second = card(body, "2  Choose your private link");
    gtk_box_append(GTK_BOX(second), label("Same Wi-Fi needs no extra software. For remote access, connect both devices with Tailscale or another private network, then enter this computer's private address or domain.", TRUE));
    gtk_box_append(GTK_BOX(second), label("Reachable IP or private domain", FALSE));
    app->host_entry = gtk_entry_new();
    gtk_box_append(GTK_BOX(second), app->host_entry);
    gtk_box_append(GTK_BOX(second), label("Access code (5–128 letters, numbers, - or _)", TRUE));
    app->code_entry = gtk_entry_new();
    gtk_box_append(GTK_BOX(second), app->code_entry);
    GtkWidget *generate = gtk_button_new_with_label("Generate a new UUID code");
    gtk_box_append(GTK_BOX(second), generate);
    gtk_box_append(GTK_BOX(second), label("Anyone with the full link can watch and change settings. Keep it private; do not forward port 8080 publicly.", TRUE));

    GtkWidget *third = card(body, "3  Start viewing");
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(third), buttons);
    app->start_button = gtk_button_new_with_label("Start camera");
    gtk_widget_add_css_class(app->start_button, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), app->start_button);
    app->stop_button = gtk_button_new_with_label("Stop");
    gtk_box_append(GTK_BOX(buttons), app->stop_button);
    app->link = label("Start the camera to get a viewing link.", TRUE);
    gtk_label_set_selectable(GTK_LABEL(app->link), TRUE);
    gtk_box_append(GTK_BOX(third), app->link);
    GtkWidget *link_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(third), link_buttons);
    GtkWidget *copy = gtk_button_new_with_label("Copy link");
    GtkWidget *open = gtk_button_new_with_label("Open in browser");
    gtk_box_append(GTK_BOX(link_buttons), copy);
    gtk_box_append(GTK_BOX(link_buttons), open);
    gtk_box_append(GTK_BOX(third), label("Video only · no recording or audio · maximum 4 viewers", TRUE));

    g_autofree gchar *path = config_path();
    GKeyFile *config = g_key_file_new();
    g_key_file_load_from_file(config, path, G_KEY_FILE_NONE, NULL);
    g_autofree gchar *host = g_key_file_get_string(config, "camera", "host", NULL);
    g_autofree gchar *code = g_key_file_get_string(config, "camera", "access_code", NULL);
    gtk_editable_set_text(GTK_EDITABLE(app->host_entry), valid_host(host) ? host : g_get_host_name());
    const gchar *test_code = g_getenv("CAMWEAVE_TEST_CODE");
    g_autofree gchar *generated = g_uuid_string_random();
    gtk_editable_set_text(GTK_EDITABLE(app->code_entry), valid_code(test_code) ? test_code : valid_code(code) ? code : generated);
    g_key_file_free(config);

    g_signal_connect(app->refresh_button, "clicked", G_CALLBACK(refresh_clicked), app);
    g_signal_connect(app->start_button, "clicked", G_CALLBACK(start_clicked), app);
    g_signal_connect(app->stop_button, "clicked", G_CALLBACK(stop_clicked), app);
    g_signal_connect(generate, "clicked", G_CALLBACK(generate_clicked), app);
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_clicked), app);
    g_signal_connect(open, "clicked", G_CALLBACK(open_clicked), app);
    g_signal_connect(app->host_entry, "changed", G_CALLBACK(entry_changed), app);
    g_signal_connect(app->code_entry, "changed", G_CALLBACK(entry_changed), app);
    g_signal_connect(app->window, "close-request", G_CALLBACK(closing), app);
    refresh_cameras(app);
    update_link(app);
    gtk_window_present(GTK_WINDOW(app->window));
    if (g_strcmp0(g_getenv("CAMWEAVE_AUTOSTART_TEST"), "1") == 0 &&
        g_strcmp0(g_getenv("CAMWEAVE_TEST_SOURCE"), "1") == 0) app_start(app);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    App app = {0};
    g_mutex_init(&app.mutex);
    g_cond_init(&app.frame_ready);
    app.quality = 720;
    app.fps = 10;
    app.devices = g_ptr_array_new_with_free_func(g_free);
    if (!g_file_get_contents("web/watch.html", &app.html, NULL, NULL))
        g_file_get_contents(APP_DATA_DIR "/watch.html", &app.html, NULL, NULL);
    if (!app.html) app.html = g_strdup("<html><body><h1>CamWeave viewer unavailable</h1></body></html>");
    GtkApplication *application = gtk_application_new("com.camweave.Camera", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    if (g_getenv("CAMWEAVE_TEST_SOURCE")) g_timeout_add_seconds(1, heartbeat, &app);
    int result = g_application_run(G_APPLICATION(application), argc, argv);
    g_mutex_lock(&app.mutex);
    while (app.clients > 0) g_cond_wait(&app.frame_ready, &app.mutex);
    g_mutex_unlock(&app.mutex);
    g_object_unref(application);
    g_ptr_array_unref(app.devices);
    g_clear_pointer(&app.last_frame, g_bytes_unref);
    g_free(app.token); g_free(app.session); g_free(app.html);
    g_cond_clear(&app.frame_ready);
    g_mutex_clear(&app.mutex);
    return result;
}
