 
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <glib.h>
#include <spice-client.h>

#define CHANNELID_MAX 4
#define MONITORID_MAX 4

static GMainLoop     *mainloop = NULL;

static int connections = 0;

#define SPICE_TYPE_WINDOW                  (spice_window_get_type ())
 

typedef struct _SpiceWindow SpiceWindow;
typedef struct _SpiceWindowClass SpiceWindowClass;
typedef struct spice_connection spice_connection;

struct _SpiceWindowClass
{
  GObjectClass parent_class;
};

struct _SpiceWindow {
    GObject          object;
    spice_connection *conn;
    gint             id;
    gint             monitor_id;
    bool             fullscreen;
    bool             mouse_grabbed;
    SpiceChannel     *display_channel;
#ifdef G_OS_WIN32
    gint             win_x;
    gint             win_y;
#endif
    gboolean         enable_accels_save;
    gboolean         enable_mnemonics_save;
};


struct spice_connection {
    SpiceSession     *session;
    SpiceWindow     *wins[CHANNELID_MAX * MONITORID_MAX];
    SpiceMainChannel *main;
    SpiceAudio       *audio;
    const char       *mouse_state;
    const char       *agent_state;
    gboolean         agent_connected;
    int              disconnecting;

    /* key: SpiceFileTransferTask, value: TransferTaskWidgets */
    GHashTable *transfers;
};

static GType spice_window_get_type(void);


G_DEFINE_TYPE (SpiceWindow, spice_window, G_TYPE_OBJECT)

static void
spice_window_class_init (SpiceWindowClass *klass)
{
}

static void
spice_window_init (SpiceWindow *self)
{
}

static SpiceWindow *create_spice_window(spice_connection *conn, SpiceChannel *channel, int id, gint monitor_id)
{
    char title[32];
    SpiceWindow *win;
    gboolean state;
    GError *err = NULL;
    int i;

    win = g_object_new(SPICE_TYPE_WINDOW, NULL);
    win->id = id;
    win->monitor_id = monitor_id;
    win->conn = conn;
    win->display_channel = channel;

    return win;
}

static void destroy_spice_window(SpiceWindow *win)
{
    if (win == NULL)
        return;

    SPICE_DEBUG("destroy window (#%d:%d)", win->id, win->monitor_id);
    g_object_unref(win);
}


static void del_window(spice_connection *conn, SpiceWindow *win)
{
    if (win == NULL)
        return;

    g_return_if_fail(win->id < CHANNELID_MAX);
    g_return_if_fail(win->monitor_id < MONITORID_MAX);

    g_debug("del display monitor %d:%d", win->id, win->monitor_id);
    conn->wins[win->id * CHANNELID_MAX + win->monitor_id] = NULL;
    if (win->id > 0)
        spice_main_channel_update_display_enabled(conn->main, win->id, FALSE, TRUE);
    else
        spice_main_channel_update_display_enabled(conn->main, win->monitor_id, FALSE, TRUE);
    spice_main_channel_send_monitor_config(conn->main);

    destroy_spice_window(win);
}

static SpiceWindow* get_window(spice_connection *conn, int channel_id, int monitor_id)
{
    g_return_val_if_fail(channel_id < CHANNELID_MAX, NULL);
    g_return_val_if_fail(monitor_id < MONITORID_MAX, NULL);

    return conn->wins[channel_id * CHANNELID_MAX + monitor_id];
}


static void add_window(spice_connection *conn, SpiceWindow *win)
{
    g_return_if_fail(win != NULL);
    g_return_if_fail(win->id < CHANNELID_MAX);
    g_return_if_fail(win->monitor_id < MONITORID_MAX);
    g_return_if_fail(conn->wins[win->id * CHANNELID_MAX + win->monitor_id] == NULL);

    SPICE_DEBUG("add display monitor %d:%d", win->id, win->monitor_id);
    conn->wins[win->id * CHANNELID_MAX + win->monitor_id] = win;
}

static void display_monitors(SpiceChannel *display, GParamSpec *pspec,
                             spice_connection *conn)
{
    GArray *monitors = NULL;
    int id;
    guint i;

    g_object_get(display,
                 "channel-id", &id,
                 "monitors", &monitors,
                 NULL);
    g_return_if_fail(monitors != NULL);

    for (i = 0; i < monitors->len; i++) {
        SpiceWindow *w;

        if (!get_window(conn, id, i)) {
            w = create_spice_window(conn, display, id, i);
            add_window(conn, w);
            // spice_g_signal_connect_object(display, "display-mark",
            //                               G_CALLBACK(display_mark), w, 0);
        }
    }

    for (; i < MONITORID_MAX; i++)
        del_window(conn, get_window(conn, id, i));

    g_clear_pointer(&monitors, g_array_unref);
}


static void main_channel_event(SpiceChannel *channel, SpiceChannelEvent event,
                               gpointer data)
{
    const GError *error = NULL;
    spice_connection *conn = data;
    char password[64];
     

    switch (event) {
    case SPICE_CHANNEL_OPENED:
        g_message("main channel: opened");
        break;
    case SPICE_CHANNEL_SWITCHING:
        g_message("main channel: switching host");
        break;
    case SPICE_CHANNEL_CLOSED:
        /* this event is only sent if the channel was succesfully opened before */
        g_message("main channel: closed");
         spice_session_disconnect(conn->session);
        break;
    case SPICE_CHANNEL_ERROR_IO:
         spice_session_disconnect(conn->session);
        break;
    case SPICE_CHANNEL_ERROR_TLS:
    case SPICE_CHANNEL_ERROR_LINK:
    case SPICE_CHANNEL_ERROR_CONNECT:
        error = spice_channel_get_error(channel);
        g_message("main channel: failed to connect");
        if (error) {
            g_message("channel error: %s", error->message);
        }
      
        break;
    case SPICE_CHANNEL_ERROR_AUTH:
        g_warning("main channel: auth failure (wrong password?)");
        strcpy(password, "");      
        break;
    default:
        /* TODO: more sophisticated error handling */
        g_warning("unknown main channel event: %u", event);
        /* connection_disconnect(conn); */
        break;
    }
}

static void main_mouse_update(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    gint mode;

    g_object_get(channel, "mouse-mode", &mode, NULL);
    switch (mode) {
    case SPICE_MOUSE_MODE_SERVER:
        conn->mouse_state = "server";
        break;
    case SPICE_MOUSE_MODE_CLIENT:
        conn->mouse_state = "client";
        break;
    default:
        conn->mouse_state = "?";
        break;
    }
    // update_status(conn);
}

static void main_agent_update(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;

    g_object_get(channel, "agent-connected", &conn->agent_connected, NULL);
    conn->agent_state = conn->agent_connected ? "yes" : "no";
    // update_status(conn);
    // update_edit_menu(conn);
}

static void channel_new(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    g_message("channel new");
    spice_connection *conn = data;
    int id;

    g_object_get(channel, "channel-id", &id, NULL);
    SPICE_DEBUG("new channel (#%d)", id);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        SPICE_DEBUG("new main channel");
        conn->main = SPICE_MAIN_CHANNEL(channel);
        g_signal_connect(channel, "channel-event",
                         G_CALLBACK(main_channel_event), conn);
        g_signal_connect(channel, "main-mouse-update",
                         G_CALLBACK(main_mouse_update), conn);
        g_signal_connect(channel, "main-agent-update",
                         G_CALLBACK(main_agent_update), conn);
        // g_signal_connect(channel, "new-file-transfer",
        //                  G_CALLBACK(new_file_transfer), conn);
        main_mouse_update(channel, conn);
        main_agent_update(channel, conn);
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        if (id >= SPICE_N_ELEMENTS(conn->wins))
            return;
        if (conn->wins[id] != NULL)
            return;
        SPICE_DEBUG("new display channel (#%d)", id);
        g_signal_connect(channel, "notify::monitors",
                         G_CALLBACK(display_monitors), conn);
        spice_channel_connect(channel);
    }

    if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        SPICE_DEBUG("new inputs channel");
        // g_signal_connect(channel, "inputs-modifiers",
        //                  G_CALLBACK(inputs_modifiers), conn);
    }

    if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        SPICE_DEBUG("new audio channel");
        conn->audio = spice_audio_get(s, NULL);
    }

    if (SPICE_IS_USBREDIR_CHANNEL(channel)) {
        // update_auto_usbredir_sensitive(conn);
    }

    if (SPICE_IS_PORT_CHANNEL(channel)) {
        // g_signal_connect(channel, "notify::port-opened",
        //                  G_CALLBACK(port_opened), conn);
        // g_signal_connect(channel, "port-data",
        //                  G_CALLBACK(port_data), conn);
        spice_channel_connect(channel);
    }
}



static spice_connection *connection_new(void)
{
    spice_connection *conn;
    // SpiceUsbDeviceManager *manager;

    g_message("connection_new");

    conn = g_new0(spice_connection, 1);
    conn->session = spice_session_new();
    g_signal_connect(conn->session, "channel-new",
                     G_CALLBACK(channel_new), conn);
    // g_signal_connect(conn->session, "channel-destroy",
    //                  G_CALLBACK(channel_destroy), conn);
    // g_signal_connect(conn->session, "notify::migration-state",
    //                  G_CALLBACK(migration_state), conn);
    // g_signal_connect(conn->session, "disconnected",
    //                  G_CALLBACK(connection_destroy), conn);

    // manager = spice_usb_device_manager_get(conn->session, NULL);
    // if (manager) {
    //     g_signal_connect(manager, "auto-connect-failed",
    //                      G_CALLBACK(usb_connect_failed), NULL);
    //     g_signal_connect(manager, "device-error",
    //                      G_CALLBACK(usb_connect_failed), NULL);
    // }

    // conn->transfers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
    //                                         g_object_unref,
    //                                         (GDestroyNotify)transfer_task_widgets_free);
    connections++;
    SPICE_DEBUG("%s (%d)", __FUNCTION__, connections);
    return conn;
}

static bool resize(gpointer connP)
{
    spice_connection *conn = connP;
   

    g_message("spice_main_channel_update_display ");

    for (int i = 0; i < SPICE_N_ELEMENTS(conn->wins); i++) {
        // g_message("trying display %d ", i);   

        if (conn->wins[i] == NULL)
            continue;            

         g_message("available display : %d ", i);   

        spice_main_channel_update_display_enabled( conn->main,
            conn->wins[0]->id + conn->wins[0]->monitor_id,
             TRUE, FALSE);

         spice_main_channel_update_display(
            conn->main,
            conn->wins[0]->id + conn->wins[0]->monitor_id,
            0, 0, 1400, 800, TRUE);
    }
   

    spice_main_channel_send_monitor_config(conn->main);

    return false;
}

int main(int argc, char *argv[])
{ 
   
    spice_connection *conn;
    char *host = "localhost", *port = "5930";

    mainloop = g_main_loop_new(NULL, false);

    g_message("host : %s", host);
 
    conn = connection_new(); 
 
    g_object_set(conn->session,"host", host, NULL); 
    g_object_set(conn->session, "port", port, NULL);

     if (!spice_session_connect(conn->session)) {
        printf("spice_session_connect failed\n");
        exit(1);
    }
    else{
        printf("spice_session_connect OK\n");
    } 
     
    g_timeout_add(2*1000, resize, conn);
  
    g_main_loop_run(mainloop);
    

     printf("coucouz \n");
    
    return 0;
}
