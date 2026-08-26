/*
 * Copyright 2026 NXP
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtk/gtk.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <memory>
#include <csignal>
#include <vector>
#include <unistd.h>
#include <sys/prctl.h>
#include <dvapi.h>

class AppWidgets
{
public:
    GtkWidget *stream_spin;
    GtkWidget *endpoint_spin;
    GtkWidget *group_combo;
    GtkWidget *sync_check;
    GtkWidget *model_combo;
    GtkWidget *no_bbox_check;
    GtkWidget *no_osd_stats_check;
    GtkWidget *only_bbox_check;
    GtkWidget *window_width_spin;
    GtkWidget *window_height_spin;
    GtkWidget *resolution_preset_combo;
    GtkWidget *status_label;
    GtkWidget *launch_button;
    GtkWidget *stop_button;
    GtkWidget *download_yolov8_button;
    GtkWidget *download_yolox_button;
    GtkWidget *download_yolo11_button;
    GtkWidget *models_status_label;
    GtkWidget *download_videos_button;
    GtkWidget *videos_status_label;
    pid_t child_pid;
};

struct ResolutionPreset
{
    const char *name;
    int width;
    int height;
};

static const ResolutionPreset RESOLUTION_PRESETS[] = {
    {"Custom", 0, 0},
    {"HD (1280x720)", 1280, 720},
    {"Full HD (1920x1080)", 1920, 1080},
    {"WQHD (2560x1440)", 2560, 1440},
    {"4K UHD (3840x2160)", 3840, 2160},
};

// Forward declaration
static void setup_signal_handlers();
static void kill_multistream_app();
static void update_launch_button_state(AppWidgets *widgets, int max_endpoints, bool models_available);
static void update_status_label(AppWidgets *widgets, int max_endpoints, bool models_available);
static bool check_videos_downloaded();
static void on_download_videos_clicked(GtkWidget *button, gpointer user_data);
static gboolean on_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void on_resolution_preset_changed(GtkWidget *combo_box, gpointer user_data);
static void on_window_dimension_changed(GtkWidget *spin_button, gpointer user_data);

// Global pointer to the main window for signal handler
static GtkWidget *g_main_window = nullptr;

// Signal handler for SIGINT and SIGTERM
static void signal_handler(int signum)
{
    (void)signum;

    // Kill the multistream application
    kill_multistream_app();

    // If we have a main window, close it gracefully
    if (g_main_window != nullptr)
    {
        // Schedule the window close on the GTK main loop
        g_idle_add([](gpointer data) -> gboolean
                   {
            GtkWidget *window = static_cast<GtkWidget*>(data);
            gtk_window_close(GTK_WINDOW(window));
            return G_SOURCE_REMOVE; }, g_main_window);
    }
    else
    {
        // If no window, just exit
        exit(0);
    }
}

// Setup signal handlers
static void setup_signal_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Handle SIGINT (Ctrl+C)
    sigaction(SIGINT, &sa, nullptr);

    // Handle SIGTERM (kill command without -9)
    sigaction(SIGTERM, &sa, nullptr);

    // Handle SIGHUP (terminal hangup)
    sigaction(SIGHUP, &sa, nullptr);
}
static int get_endpoint_count()
{
    dv_session_t *session = NULL;
    dv_endpoint_t *endpoint_list = NULL;
    int endpoint_count = 0;
    int status;

    status = dv_session_create_via_unix_socket("/var/run/proxy.sock", &session);
    if (status != DV_SUCCESS || !session)
    {
        printf("Failed to create session (status=%d)\n", status);
        return 0;
    }
    
    status = dv_endpoint_get_list(session, &endpoint_list, &endpoint_count);
    if (status != DV_SUCCESS)
    {
        printf("dv_endpoint_get_list failed (status=%d)\n", status);
        dv_session_close(session);
        return 0;
    }

    printf("Endpoints detected: %d\n", endpoint_count);
    dv_session_close(session);

    return endpoint_count;
}

// Returns list of model names available on disk under /usr/share/cnn/detection/
static std::vector<std::string> get_available_models()
{
    // All supported models in display order
    static const char *ALL_MODELS[] = {
        "yolov8n", "yolov8s", "yolov8m", "yolov8l", "yolov8x",
        "yoloxs",  "yoloxt",  "yoloxm",  "yoloxl",  "yoloxx",
        "yolo11n", "yolo11s", "yolo11m", "yolo11l", "yolo11x",
        nullptr
    };

    std::vector<std::string> available;
    for (int i = 0; ALL_MODELS[i] != nullptr; i++)
    {
        std::string path = std::string("/usr/share/cnn/detection/") + ALL_MODELS[i];
        if (access(path.c_str(), F_OK) == 0)
        {
            available.push_back(ALL_MODELS[i]);
        }
    }
    return available;
}

static bool check_models_downloaded()
{
    return !get_available_models().empty();
}

// Repopulate the model combo box with currently available models.
// Shows "--- No models ---" when none are found; selects yolov8n when present.
static void refresh_model_combo(AppWidgets *widgets)
{
    // Remove all existing entries
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(widgets->model_combo));

    std::vector<std::string> avail_models = get_available_models();
    if (avail_models.empty())
    {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo), "--- No models ---");
        gtk_combo_box_set_active(GTK_COMBO_BOX(widgets->model_combo), 0);
        gtk_widget_set_sensitive(widgets->model_combo, FALSE);
        return;
    }

    gtk_widget_set_sensitive(widgets->model_combo, TRUE);
    int default_index = 0;
    for (int mi = 0; mi < static_cast<int>(avail_models.size()); mi++)
    {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo),
                                       avail_models[mi].c_str());
        if (avail_models[mi] == "yolov8n")
        {
            default_index = mi;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(widgets->model_combo), default_index);
}

static void update_launch_button_state(AppWidgets *widgets, int max_endpoints, bool models_available)
{
    // Enable launch button only if both endpoints are available AND models are downloaded
    if (max_endpoints > 0 && models_available)
    {
        gtk_widget_set_sensitive(widgets->launch_button, TRUE);
    }
    else
    {
        gtk_widget_set_sensitive(widgets->launch_button, FALSE);
    }
}

static void update_status_label(AppWidgets *widgets, int max_endpoints, bool models_available)
{
    std::string status_text;
    if (max_endpoints > 0 && models_available)
    {
        status_text = "Ready to launch (Detected " + std::to_string(max_endpoints) +
                      " endpoint" + (max_endpoints == 1 ? "" : "s") + ", models available)";
    }
    else if (max_endpoints == 0 && !models_available)
    {
        status_text = "Warning: No ARA2 endpoints detected and models are not present.";
    }
    else if (max_endpoints == 0)
    {
        status_text = "Warning: No ARA2 endpoints detected.";
    }
    else
    {
        status_text = "Warning: Models are not present. Please download models to launch.";
    }
    gtk_label_set_text(GTK_LABEL(widgets->status_label), status_text.c_str());
}
// Payload passed to the generic model download thread
struct DownloadTask
{
    AppWidgets *widgets;
    std::string command;  // full shell command to run
    std::string label;    // short label for thread name
};

static gboolean check_download_models_complete(gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);

    // Re-enable all three download buttons regardless of outcome
    gtk_widget_set_sensitive(widgets->download_yolov8_button, TRUE);
    gtk_widget_set_sensitive(widgets->download_yolox_button, TRUE);
    gtk_widget_set_sensitive(widgets->download_yolo11_button, TRUE);

    // Check if at least one model is now available
    bool models_available = check_models_downloaded();

    if (models_available)
    {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label),
                             "<span foreground='green'><b>✓ Models ok</b></span>");

        // Update launch button state
        int max_endpoints = get_endpoint_count();
        update_launch_button_state(widgets, max_endpoints, true);

        // Update the main status label
        update_status_label(widgets, max_endpoints, true);
    }
    else
    {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label),
                             "<span foreground='orange'><b>⚠ Check manually</b></span>");
    }

    // Always refresh model combo to reflect current disk state
    refresh_model_combo(widgets);

    return G_SOURCE_REMOVE;
}

// Callback scheduled on the GTK main thread when download fails
static gboolean on_download_failed(gpointer user_data)
{
    auto *task = static_cast<DownloadTask *>(user_data);
    AppWidgets *w = task->widgets;

    gtk_label_set_markup(GTK_LABEL(w->models_status_label),
                         "<span foreground='red'><b>✗ Download failed</b></span>");
    gtk_widget_set_sensitive(w->download_yolov8_button, TRUE);
    gtk_widget_set_sensitive(w->download_yolox_button, TRUE);
    gtk_widget_set_sensitive(w->download_yolo11_button, TRUE);

    delete task;
    return G_SOURCE_REMOVE;
}

static gpointer download_models_thread(gpointer user_data)
{
    auto *task = static_cast<DownloadTask *>(user_data);
    AppWidgets *widgets = task->widgets;

    int result = system(task->command.c_str());

    if (result != 0)
    {
        g_idle_add(on_download_failed, task);
    }
    else
    {
        delete task;
        g_idle_add(check_download_models_complete, widgets);
    }

    return nullptr;
}

// Helper: disable all download buttons, update status, launch thread
static void start_model_download(AppWidgets *widgets, const char *cmd, const char *thread_name)
{
    gtk_label_set_markup(GTK_LABEL(widgets->models_status_label),
                         "<span foreground='blue'><b>⏳ Downloading...</b></span>");
    gtk_widget_set_sensitive(widgets->download_yolov8_button, FALSE);
    gtk_widget_set_sensitive(widgets->download_yolox_button, FALSE);
    gtk_widget_set_sensitive(widgets->download_yolo11_button, FALSE);

    auto *task = new DownloadTask{widgets, cmd, thread_name};
    g_thread_new(thread_name, download_models_thread, task);
}

static void on_download_yolov8_clicked(GtkWidget *button, gpointer user_data)
{
    (void)button;
    start_model_download(static_cast<AppWidgets *>(user_data),
                         "ara2-fetch-models --repo-id nxp/Ultralytics-YOLOv8-detection-Ara240",
                         "dl_yolov8");
}

static void on_download_yolox_clicked(GtkWidget *button, gpointer user_data)
{
    (void)button;
    start_model_download(static_cast<AppWidgets *>(user_data),
                         "ara2-fetch-models --repo-id nxp/YOLOX-Detection-Ara240",
                         "dl_yolox");
}

static void on_download_yolo11_clicked(GtkWidget *button, gpointer user_data)
{
    (void)button;
    start_model_download(static_cast<AppWidgets *>(user_data),
                         "ara2-fetch-models --repo-id nxp/Ultralytics-YOLO11-Detection-Ara240",
                         "dl_yolo11");
}

static void kill_multistream_app()
{
    // Kill only the multistream_yolo process with arguments (not the launcher)
    // This matches processes that have " -s " in their command line (the launched app with parameters)
    int result = system("pkill -9 -f 'multistream_yolo -s'");
    (void)result;
}

static void on_stop_clicked(GtkWidget *button, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)button;

    kill_multistream_app();

    gtk_label_set_text(GTK_LABEL(widgets->status_label), "Application stopped successfully!");

    // Re-enable launch button and disable stop button
    // Check if models are available before enabling launch button
    bool models_available = check_models_downloaded();
    int max_endpoints = get_endpoint_count();
    update_launch_button_state(widgets, max_endpoints, models_available);
    gtk_widget_set_sensitive(widgets->stop_button, FALSE);
}

static void on_quit_clicked(GtkWidget *button, gpointer user_data)
{
    auto *window = static_cast<GtkWidget *>(user_data);
    (void)button;

    // Kill the multistream application before quitting
    kill_multistream_app();

    // Close the window
    gtk_window_close(GTK_WINDOW(window));
}

static void on_group_changed(GtkWidget *combo_box, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)combo_box;

    const char *group = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widgets->group_combo));

    if (group != nullptr)
    {
        // Disable endpoint selection when "pcie" is selected
        if (strcmp(group, "pcie") == 0)
        {
            gtk_widget_set_sensitive(widgets->endpoint_spin, FALSE);
        }
        else
        {
            gtk_widget_set_sensitive(widgets->endpoint_spin, TRUE);
        }
    }
}

static void on_only_bbox_toggled(GtkWidget *check_button, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)check_button;

    gboolean only_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check));
    gboolean no_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check));

    // If only-bbox is enabled, disable no-bbox
    if (only_bbox_active && no_bbox_active)
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check), FALSE);
    }
}

static void on_no_bbox_toggled(GtkWidget *check_button, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)check_button;

    gboolean no_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check));
    gboolean only_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check));

    // If no-bbox is enabled, disable only-bbox
    if (no_bbox_active && only_bbox_active)
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check), FALSE);
    }
}
static void on_launch_clicked(GtkWidget *button, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)button;

    // Get values from widgets
    int streams = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets->stream_spin));
    int endpoint = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets->endpoint_spin));
    const char *group = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widgets->group_combo));
    gboolean sync = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->sync_check));
    const char *model = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widgets->model_combo));
    gboolean no_bbox = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check));
    gboolean no_osd_stats = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_osd_stats_check));
    gboolean only_bbox = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check));
    int window_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets->window_width_spin));
    int window_height = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets->window_height_spin));

    // Update status
    gtk_label_set_text(GTK_LABEL(widgets->status_label), "Launching application...");

    // Execute command
    pid_t pid = fork();

    if (pid == 0)
    {
        // CHILD PROCESS

        // Ensure child dies if parent dies (IMPORTANT)
        prctl(PR_SET_PDEATHSIG, SIGKILL);

        // Build argv instead of using shell
        std::vector<std::string> args = {
            "multistream_yolo",
            "-s", std::to_string(streams),
            "-g", group,
            "-y", (sync ? "true" : "false"),
            "-m", model,
            "-t", "0",
            "--width", std::to_string(window_width),
            "--height", std::to_string(window_height)};

        if (strcmp(group, "pcie") != 0)
        {
            args.push_back("-e");
            args.push_back(std::to_string(endpoint));
        }

        if (no_bbox)
            args.push_back("--no-bbox");
        if (no_osd_stats)
            args.push_back("--no-osd-stats");
        if (only_bbox)
            args.push_back("--only-bbox");

        // Convert to char*
        std::vector<char *> argv;
        for (auto &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);

        execvp("multistream_yolo", argv.data());

        // If exec fails
        perror("exec failed");
        _exit(1);
    }
    else if (pid > 0)
    {
        // PARENT PROCESS

        gtk_label_set_text(GTK_LABEL(widgets->status_label),
                           "Application launched successfully!");

        gtk_widget_set_sensitive(widgets->launch_button, FALSE);
        gtk_widget_set_sensitive(widgets->stop_button, TRUE);

        // Store PID for stop button
        widgets->child_pid = pid;
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(widgets->status_label),
                           "Failed to launch application!");
    }
}
static void on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    auto widgets = std::make_unique<AppWidgets>();

    // Detect available endpoints
    int max_endpoints = get_endpoint_count();

    // Check if models are downloaded at startup
    bool models_available = check_models_downloaded();

    // Check if videos are downloaded at startup
    bool videos_available = check_videos_downloaded();

    // Create main window
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Multistream YOLO Launcher");
    gtk_window_set_default_size(GTK_WINDOW(window), 550, 600);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);

    // Store window globally for signal handler
    g_main_window = window;

    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete_event), nullptr);

    // Create main vertical box
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(window), main_box);

    // Create horizontal box for Models and Videos sections side by side
    GtkWidget *management_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(main_box), management_hbox, FALSE, FALSE, 5);

    // Add Models section (left side)
    GtkWidget *models_frame = gtk_frame_new("Models");
    gtk_box_pack_start(GTK_BOX(management_hbox), models_frame, TRUE, TRUE, 0);

    GtkWidget *models_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_set_border_width(GTK_CONTAINER(models_box), 5);
    gtk_container_add(GTK_CONTAINER(models_frame), models_box);

    // Models status label - set initial status based on startup check
    widgets->models_status_label = gtk_label_new(nullptr);
    if (models_available)
    {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label),
                             "<span foreground='green'><b>✓ Models ok</b></span>");
    }
    else
    {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label),
                             "<span foreground='red'><b>✗ Models not found</b></span>");
    }
    gtk_label_set_line_wrap(GTK_LABEL(widgets->models_status_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widgets->models_status_label), 20);
    gtk_box_pack_start(GTK_BOX(models_box), widgets->models_status_label, FALSE, FALSE, 3);

    // Three download buttons side by side to keep the Models frame compact
    GtkWidget *dl_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(models_box), dl_btn_box, FALSE, FALSE, 0);

    widgets->download_yolov8_button = gtk_button_new_with_label("YOLOv8");
    g_signal_connect(widgets->download_yolov8_button, "clicked",
                     G_CALLBACK(on_download_yolov8_clicked), widgets.get());
    gtk_box_pack_start(GTK_BOX(dl_btn_box), widgets->download_yolov8_button, TRUE, TRUE, 0);

    widgets->download_yolox_button = gtk_button_new_with_label("YOLOX");
    g_signal_connect(widgets->download_yolox_button, "clicked",
                     G_CALLBACK(on_download_yolox_clicked), widgets.get());
    gtk_box_pack_start(GTK_BOX(dl_btn_box), widgets->download_yolox_button, TRUE, TRUE, 0);

    widgets->download_yolo11_button = gtk_button_new_with_label("YOLO11");
    g_signal_connect(widgets->download_yolo11_button, "clicked",
                     G_CALLBACK(on_download_yolo11_clicked), widgets.get());
    gtk_box_pack_start(GTK_BOX(dl_btn_box), widgets->download_yolo11_button, TRUE, TRUE, 0);

    // Add Videos section (right side)
    GtkWidget *videos_frame = gtk_frame_new("Videos");
    gtk_box_pack_start(GTK_BOX(management_hbox), videos_frame, TRUE, TRUE, 0);

    GtkWidget *videos_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_set_border_width(GTK_CONTAINER(videos_box), 5);
    gtk_container_add(GTK_CONTAINER(videos_frame), videos_box);

    // Videos status label - set initial status based on startup check
    widgets->videos_status_label = gtk_label_new(nullptr);
    if (videos_available)
    {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label),
                             "<span foreground='green'><b>✓ Sample videos ok</b></span>");
    }
    else
    {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label),
                             "<span foreground='red'><b>✗ Sample videos not found</b></span>");
    }
    gtk_label_set_line_wrap(GTK_LABEL(widgets->videos_status_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widgets->videos_status_label), 15);
    gtk_box_pack_start(GTK_BOX(videos_box), widgets->videos_status_label, FALSE, FALSE, 3);

    // Download videos button - enable only if videos are not available
    widgets->download_videos_button = gtk_button_new_with_label("Download");
    g_signal_connect(widgets->download_videos_button, "clicked", G_CALLBACK(on_download_videos_clicked), widgets.get());
    gtk_widget_set_sensitive(widgets->download_videos_button, !videos_available);
    gtk_box_pack_start(GTK_BOX(videos_box), widgets->download_videos_button, FALSE, FALSE, 0);

    // Create grid for parameters
    GtkWidget *label = nullptr;
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(main_box), grid, TRUE, TRUE, 0);

    int row = 0;

    // Stream parameter (1-16)
    label = gtk_label_new("Number of Streams (1-16):");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->stream_spin = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->stream_spin), 8);
    gtk_grid_attach(GTK_GRID(grid), widgets->stream_spin, 1, row, 1, 1);
    row++;

    // Endpoint parameter (0 to detected max, up to 10)
    std::string endpoint_label_text;
    if (max_endpoints > 0)
    {
        endpoint_label_text = "ARA2 Endpoint (0-" +
                              std::to_string(max_endpoints > 10 ? 10 : max_endpoints - 1) + "):";
    }
    else
    {
        endpoint_label_text = "ARA2 Endpoint (No endpoints detected):";
    }
    label = gtk_label_new(endpoint_label_text.c_str());
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    int max_endpoint_value = (max_endpoints > 0) ? (max_endpoints > 10 ? 10 : max_endpoints - 1) : 0;
    widgets->endpoint_spin = gtk_spin_button_new_with_range(0, max_endpoint_value, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->endpoint_spin), 0);
    // Disable endpoint spin if no endpoints detected
    if (max_endpoints == 0)
    {
        gtk_widget_set_sensitive(widgets->endpoint_spin, FALSE);
    }
    gtk_grid_attach(GTK_GRID(grid), widgets->endpoint_spin, 1, row, 1, 1);
    row++;

    // Group parameter
    label = gtk_label_new("Device Group:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->group_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->group_combo), "all");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->group_combo), "pcie");
    gtk_combo_box_set_active(GTK_COMBO_BOX(widgets->group_combo), 0); // Default to "all"
    g_signal_connect(widgets->group_combo, "changed", G_CALLBACK(on_group_changed), widgets.get());
    gtk_grid_attach(GTK_GRID(grid), widgets->group_combo, 1, row, 1, 1);
    row++;

    // Sync parameter
    label = gtk_label_new("Waylandsink Synchronization:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->sync_check = gtk_check_button_new_with_label("Enable");
    gtk_grid_attach(GTK_GRID(grid), widgets->sync_check, 1, row, 1, 1);
    row++;

    // Model selection
    label = gtk_label_new("Model:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->model_combo = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(grid), widgets->model_combo, 1, row, 1, 1);
    row++;

    // Add separator for display options
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), separator, 0, row, 2, 1);
    row++;

    // Window configuration header
    label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), "<b>Window Configuration</b>");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 2, 1);
    row++;

    // Resolution preset (width/height spin buttons)
    label = gtk_label_new("Resolution Preset:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->resolution_preset_combo = gtk_combo_box_text_new();
    for (size_t i = 0; i < sizeof(RESOLUTION_PRESETS) / sizeof(RESOLUTION_PRESETS[0]); i++)
    {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->resolution_preset_combo),
                                       RESOLUTION_PRESETS[i].name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(widgets->resolution_preset_combo), 2); // Default to Full HD
    g_signal_connect(widgets->resolution_preset_combo, "changed",
                     G_CALLBACK(on_resolution_preset_changed), widgets.get());
    gtk_grid_attach(GTK_GRID(grid), widgets->resolution_preset_combo, 1, row, 1, 1);
    row++;

    // Window width
    label = gtk_label_new("Window Width (320-7680):");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->window_width_spin = gtk_spin_button_new_with_range(320, 7680, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->window_width_spin), 1920);
    gtk_widget_set_sensitive(widgets->window_width_spin, FALSE);
    g_signal_connect(widgets->window_width_spin, "value-changed",
                     G_CALLBACK(on_window_dimension_changed), widgets.get());
    gtk_grid_attach(GTK_GRID(grid), widgets->window_width_spin, 1, row, 1, 1);
    row++;

    // Window height
    label = gtk_label_new("Window Height (240-4320):");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->window_height_spin = gtk_spin_button_new_with_range(240, 4320, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->window_height_spin), 1080);
    gtk_widget_set_sensitive(widgets->window_height_spin, FALSE);
    g_signal_connect(widgets->window_height_spin, "value-changed",
                     G_CALLBACK(on_window_dimension_changed), widgets.get());
    gtk_grid_attach(GTK_GRID(grid), widgets->window_height_spin, 1, row, 1, 1);
    row++;

    // Display options header
    label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), "<b>Display Options</b>");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 2, 1);
    row++;

    // No bounding boxes option
    label = gtk_label_new("Bounding Boxes:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->no_bbox_check = gtk_check_button_new_with_label("No bounding boxes");
    g_signal_connect(widgets->no_bbox_check, "toggled", G_CALLBACK(on_no_bbox_toggled), widgets.get());
    gtk_grid_attach(GTK_GRID(grid), widgets->no_bbox_check, 1, row, 1, 1);
    row++;

    // Only bounding boxes option
    label = gtk_label_new("");
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->only_bbox_check = gtk_check_button_new_with_label("Only bounding boxes (no labels)");
    g_signal_connect(widgets->only_bbox_check, "toggled", G_CALLBACK(on_only_bbox_toggled), widgets.get());
    gtk_grid_attach(GTK_GRID(grid), widgets->only_bbox_check, 1, row, 1, 1);
    row++;

    // No OSD stats option
    label = gtk_label_new("On-Screen Stats:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

    widgets->no_osd_stats_check = gtk_check_button_new_with_label("Disable OSD stats");
    gtk_grid_attach(GTK_GRID(grid), widgets->no_osd_stats_check, 1, row, 1, 1);
    row++;

    // Status label - use helper function to set initial status
    widgets->status_label = gtk_label_new(nullptr);
    update_status_label(widgets.get(), max_endpoints, models_available);
    gtk_label_set_line_wrap(GTK_LABEL(widgets->status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), widgets->status_label, FALSE, FALSE, 5);

    // Button box
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_box), button_box, FALSE, FALSE, 0);

    // Launch button
    widgets->launch_button = gtk_button_new_with_label("Launch Application");
    g_signal_connect(widgets->launch_button, "clicked", G_CALLBACK(on_launch_clicked), widgets.get());
    // Disable launch button if no endpoints detected OR models not available
    update_launch_button_state(widgets.get(), max_endpoints, models_available);
    gtk_box_pack_start(GTK_BOX(button_box), widgets->launch_button, TRUE, TRUE, 0);

    // Stop button
    widgets->stop_button = gtk_button_new_with_label("Stop Application");
    g_signal_connect(widgets->stop_button, "clicked", G_CALLBACK(on_stop_clicked), widgets.get());
    gtk_widget_set_sensitive(widgets->stop_button, FALSE); // Initially disabled
    gtk_box_pack_start(GTK_BOX(button_box), widgets->stop_button, TRUE, TRUE, 0);

    // Quit button
    GtkWidget *quit_button = gtk_button_new_with_label("Quit");
    g_signal_connect(quit_button, "clicked", G_CALLBACK(on_quit_clicked), window);
    gtk_box_pack_start(GTK_BOX(button_box), quit_button, TRUE, TRUE, 0);

    // Transfer ownership to GObject system
    AppWidgets *widgets_ptr = widgets.release();
    g_object_set_data_full(G_OBJECT(window), "widgets", widgets_ptr,
                           [](gpointer data)
                           { delete static_cast<AppWidgets *>(data); });

    // Populate model list based on what is currently installed on disk
    refresh_model_combo(widgets_ptr);

    // Show all widgets
    gtk_widget_show_all(window);
}

static bool check_videos_downloaded()
{
    FILE *fp;
    char buffer[256];
    bool videos_found = false;

    // Check if sample videos directory exists and has video files
    fp = popen("ls -lh /usr/share/ara2-vision-examples/sample_videos/video_*.ts 2>/dev/null", "r");
    if (fp == nullptr)
    {
        return false;
    }

    // If we get any output, videos exist
    if (fgets(buffer, sizeof(buffer), fp) != nullptr)
    {
        videos_found = true;
    }

    pclose(fp);
    return videos_found;
}
static gboolean check_download_videos_complete(gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);

    // Verify videos were actually downloaded
    bool videos_available = check_videos_downloaded();

    if (videos_available)
    {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label),
                             "<span foreground='green'><b>✓ Downloaded</b></span>");

        // Keep download button disabled since videos are now available
        gtk_widget_set_sensitive(widgets->download_videos_button, FALSE);
    }
    else
    {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label),
                             "<span foreground='orange'><b>⚠ Check manually</b></span>");
        gtk_widget_set_sensitive(widgets->download_videos_button, TRUE);
    }

    return G_SOURCE_REMOVE;
}

static gpointer download_videos_thread(gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);

    // Execute download command
    int result = system("fetch_videos.sh");

    if (result != 0)
    {
        // Schedule UI update on main thread for failure
        g_idle_add([](gpointer data) -> gboolean
                   {
            auto *w = static_cast<AppWidgets*>(data);
            gtk_label_set_markup(GTK_LABEL(w->videos_status_label), 
                                "<span foreground='red'><b>✗ Failed</b></span>");
            gtk_widget_set_sensitive(w->download_videos_button, TRUE);
            return G_SOURCE_REMOVE; }, widgets);
    }
    else
    {
        // Schedule completion check on main thread
        g_idle_add(check_download_videos_complete, widgets);
    }

    return nullptr;
}

static void on_download_videos_clicked(GtkWidget *button, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)button;

    gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label),
                         "<span foreground='blue'><b>⏳ Downloading...</b></span>");
    gtk_widget_set_sensitive(widgets->download_videos_button, FALSE);

    // Start download in a separate thread to keep UI responsive
    g_thread_new("download_videos", download_videos_thread, widgets);
}

static gboolean on_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void)widget;
    (void)event;
    (void)user_data;

    // Kill the multistream application before closing
    kill_multistream_app();

    // Return FALSE to allow the window to close
    // Return TRUE would prevent the window from closing
    return FALSE;
}
static void on_resolution_preset_changed(GtkWidget *combo_box, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);

    int active_index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_box));

    if (active_index > 0 && active_index < static_cast<int>(sizeof(RESOLUTION_PRESETS) / sizeof(RESOLUTION_PRESETS[0])))
    {
        // Apply preset resolution
        const ResolutionPreset &preset = RESOLUTION_PRESETS[active_index];

        // Block signals to prevent recursive updates
        g_signal_handlers_block_by_func(widgets->window_width_spin,
                                        (gpointer)on_window_dimension_changed, widgets);
        g_signal_handlers_block_by_func(widgets->window_height_spin,
                                        (gpointer)on_window_dimension_changed, widgets);

        gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->window_width_spin), preset.width);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->window_height_spin), preset.height);

        // Unblock signals
        g_signal_handlers_unblock_by_func(widgets->window_width_spin,
                                          (gpointer)on_window_dimension_changed, widgets);
        g_signal_handlers_unblock_by_func(widgets->window_height_spin,
                                          (gpointer)on_window_dimension_changed, widgets);

        // Disable spin buttons when using preset
        gtk_widget_set_sensitive(widgets->window_width_spin, FALSE);
        gtk_widget_set_sensitive(widgets->window_height_spin, FALSE);
    }
    else if (active_index == 0)
    {
        // Custom resolution - enable spin buttons
        gtk_widget_set_sensitive(widgets->window_width_spin, TRUE);
        gtk_widget_set_sensitive(widgets->window_height_spin, TRUE);
    }
}

static void on_window_dimension_changed(GtkWidget *spin_button, gpointer user_data)
{
    auto *widgets = static_cast<AppWidgets *>(user_data);
    (void)spin_button;

    // When user manually changes dimensions, switch to "Custom" preset
    int current_preset = gtk_combo_box_get_active(GTK_COMBO_BOX(widgets->resolution_preset_combo));

    // Only switch to Custom if we're not already on Custom and spin buttons are enabled
    // (enabled means user is manually editing, not a preset change)
    if (current_preset != 0 && gtk_widget_get_sensitive(widgets->window_width_spin))
    {
        // Block signal to prevent recursion
        g_signal_handlers_block_by_func(widgets->resolution_preset_combo,
                                        (gpointer)on_resolution_preset_changed, widgets);

        gtk_combo_box_set_active(GTK_COMBO_BOX(widgets->resolution_preset_combo), 0);

        // Unblock signal
        g_signal_handlers_unblock_by_func(widgets->resolution_preset_combo,
                                          (gpointer)on_resolution_preset_changed, widgets);
    }
}

int main(int argc, char **argv)
{
    // Setup signal handlers for Ctrl+C and kill
    setup_signal_handlers();

    GtkApplication *app = gtk_application_new("com.example.yololauncher", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}