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

class AppWidgets {
public:
    GtkWidget *stream_spin;
    GtkWidget *endpoint_spin;
    GtkWidget *group_combo;
    GtkWidget *sync_check;
    GtkWidget *model_combo;
    GtkWidget *no_bbox_check;
    GtkWidget *no_osd_stats_check;
    GtkWidget *only_bbox_check;
    GtkWidget *status_label;
    GtkWidget *launch_button;
    GtkWidget *stop_button;
    GtkWidget *download_models_button;
    GtkWidget *models_status_label;
    GtkWidget *download_videos_button;
    GtkWidget *videos_status_label;
};
// Forward declaration
static void kill_multistream_app();
static void update_launch_button_state(AppWidgets *widgets, int max_endpoints, bool models_available);
static void update_status_label(AppWidgets *widgets, int max_endpoints, bool models_available);
static bool check_videos_downloaded();
static void on_download_videos_clicked(GtkWidget *button, gpointer user_data);
static gboolean on_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data);

static int get_endpoint_count() {
    FILE *fp;
    char buffer[256];
    int endpoint_count = 0; // Default to 0 if detection fails
    bool found = false; // Flag to track if we found the endpoint info
    
    // Run the hw_metrics script and capture output
    fp = popen("timeout 2 /usr/share/rt-sdk-ara240/scripts/ara2_metrics_bin/hw_metrics.out 2>/dev/null | grep 'Found endpoints'", "r");
    if (fp == nullptr) {
        g_warning("Failed to run hw_metrics.out, using default endpoint count");
        return endpoint_count;
    }
    
    // Parse the output to find "Found endpoints: count=X"
    while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
        char *count_str = strstr(buffer, "count=");
        if (count_str != nullptr) {
            if (sscanf(count_str, "count=%d", &endpoint_count) == 1) {
                found = true;
            }
            break;
        }
    }
    
    pclose(fp);
    
    // If we didn't find the endpoint count, return 0
    if (!found) {
        endpoint_count = 0;
    }
    
    // Ensure endpoint_count is not negative
    if (endpoint_count < 0) {
        endpoint_count = 0;
    }
    
    return endpoint_count;
}

static bool check_models_downloaded() {
    FILE *fp;
    char buffer[256];
    bool models_found = false;
    
    // Check if yolov8 models directory exists and has content
    fp = popen("ls -lh /usr/share/cnn/detection/yolov8* 2>/dev/null", "r");
    if (fp == nullptr) {
        return false;
    }
    
    // If we get any output, models exist
    if (fgets(buffer, sizeof(buffer), fp) != nullptr) {
        models_found = true;
    }
    
    pclose(fp);
    return models_found;
}

static void update_launch_button_state(AppWidgets *widgets, int max_endpoints, bool models_available) {
    // Enable launch button only if both endpoints are available AND models are downloaded
    if (max_endpoints > 0 && models_available) {
        gtk_widget_set_sensitive(widgets->launch_button, TRUE);
    } else {
        gtk_widget_set_sensitive(widgets->launch_button, FALSE);
    }
}

static void update_status_label(AppWidgets *widgets, int max_endpoints, bool models_available) {
    std::string status_text;
    if (max_endpoints > 0 && models_available) {
        status_text = "Ready to launch (Detected " + std::to_string(max_endpoints) + 
                     " endpoint" + (max_endpoints == 1 ? "" : "s") + ", models available)";
    } else if (max_endpoints == 0 && !models_available) {
        status_text = "Warning: No ARA2 endpoints detected and models are not present.";
    } else if (max_endpoints == 0) {
        status_text = "Warning: No ARA2 endpoints detected.";
    } else {
        status_text = "Warning: Models are not present. Please download models to launch.";
    }
    gtk_label_set_text(GTK_LABEL(widgets->status_label), status_text.c_str());
}

static void on_download_models_clicked(GtkWidget *button, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
    (void)button;
    
    gtk_label_set_text(GTK_LABEL(widgets->models_status_label), 
                      "Downloading models... This may take some time. Please wait...");
    gtk_widget_set_sensitive(widgets->download_models_button, FALSE);
    
    // Force UI update
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    
    // Execute download command
    int result = system("fetch_models --repo-id nxp/YOLOv8");
    
    if (result == 0) {
        // Verify models were actually downloaded
        bool models_available = check_models_downloaded();
        
        if (models_available) {
            gtk_label_set_markup(GTK_LABEL(widgets->models_status_label), 
                                "<span foreground='green'><b>✓ Models downloaded successfully!</b></span>");
            
            // Update launch button state - models are now available
            int max_endpoints = get_endpoint_count();
            update_launch_button_state(widgets, max_endpoints, true);
            
            // Update the main status label
            update_status_label(widgets, max_endpoints, true);
            
            // Keep download button disabled since models are now available
            gtk_widget_set_sensitive(widgets->download_models_button, FALSE);
        } else {
            gtk_label_set_markup(GTK_LABEL(widgets->models_status_label), 
                                "<span foreground='orange'><b>⚠ Download completed but models not detected. Please check manually.</b></span>");
            gtk_widget_set_sensitive(widgets->download_models_button, TRUE);
        }
    } else {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label), 
                            "<span foreground='red'><b>✗ Failed to download models. Check console for errors.</b></span>");
        gtk_widget_set_sensitive(widgets->download_models_button, TRUE);
    }
}

static void kill_multistream_app() {
    // Kill only the multistream_yolo process with arguments (not the launcher)
    // This matches processes that have " -s " in their command line (the launched app with parameters)
    int result = system("pkill -9 -f 'multistream_yolo -s'");
    (void)result;
}

static void on_stop_clicked(GtkWidget *button, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
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

static void on_quit_clicked(GtkWidget *button, gpointer user_data) {
    auto *window = static_cast<GtkWidget*>(user_data);
    (void)button;
    
    // Kill the multistream application before quitting
    kill_multistream_app();
    
    // Close the window
    gtk_window_close(GTK_WINDOW(window));
}

static void on_group_changed(GtkWidget *combo_box, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
    (void)combo_box;
    
    const char *group = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widgets->group_combo));
    
    if (group != nullptr) {
        // Disable endpoint selection when "pcie" is selected
        if (strcmp(group, "pcie") == 0) {
            gtk_widget_set_sensitive(widgets->endpoint_spin, FALSE);
        } else {
            gtk_widget_set_sensitive(widgets->endpoint_spin, TRUE);
        }
    }
}

static void on_only_bbox_toggled(GtkWidget *check_button, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
    (void)check_button;
    
    gboolean only_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check));
    gboolean no_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check));
    
    // If only-bbox is enabled, disable no-bbox
    if (only_bbox_active && no_bbox_active) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check), FALSE);
    }
}

static void on_no_bbox_toggled(GtkWidget *check_button, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
    (void)check_button;
    
    gboolean no_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check));
    gboolean only_bbox_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check));
    
    // If no-bbox is enabled, disable only-bbox
    if (no_bbox_active && only_bbox_active) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check), FALSE);
    }
}

static void on_launch_clicked(GtkWidget *button, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
    (void)button;
    
    std::string command = "multistream_yolo";
    
    // Get values from widgets
    int streams = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets->stream_spin));
    int endpoint = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widgets->endpoint_spin));
    const char *group = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widgets->group_combo));
    gboolean sync = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->sync_check));
    const char *model = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widgets->model_combo));
    gboolean no_bbox = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_bbox_check));
    gboolean no_osd_stats = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->no_osd_stats_check));
    gboolean only_bbox = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets->only_bbox_check));
    
    // Build command using string concatenation
    command += " -s " + std::to_string(streams);
    
    // Add endpoint parameter only if group is not "pcie"
    if (strcmp(group, "pcie") != 0) {
        command += " -e " + std::to_string(endpoint);
    }
    
    // Add group parameter
    command += " -g " + std::string(group);
    
    // Add sync parameter
    command += " -y " + std::string(sync ? "true" : "false");
    
    // Add model parameter
    command += " -m " + std::string(model);
    
    // Force disable console stats refresh (set -t to 0)
    command += " -t 0";
    
    // Add display options
    if (no_bbox) {
        command += " --no-bbox";
    }
    
    if (no_osd_stats) {
        command += " --no-osd-stats";
    }
    
    if (only_bbox) {
        command += " --only-bbox";
    }
    
    // Add background execution
    command += " &";
    
    // Update status
    std::string status_msg = "Launching: " + command;
    gtk_label_set_text(GTK_LABEL(widgets->status_label), status_msg.c_str());
    
    // Execute command
    int result = system(command.c_str());
    if (result == 0) {
        gtk_label_set_text(GTK_LABEL(widgets->status_label), "Application launched successfully!");
        // Disable launch button and enable stop button
        gtk_widget_set_sensitive(widgets->launch_button, FALSE);
        gtk_widget_set_sensitive(widgets->stop_button, TRUE);
    } else {
        gtk_label_set_text(GTK_LABEL(widgets->status_label), "Failed to launch application!");
    }
}

static void on_activate(GtkApplication *app, gpointer user_data) {
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
    gtk_window_set_default_size(GTK_WINDOW(window), 550, 650);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);

    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete_event), nullptr);
    
    // Create main vertical box
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(window), main_box);
    
    // Create title label
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), "<b><big>Multistream YOLO Configuration</big></b>");
    gtk_box_pack_start(GTK_BOX(main_box), label, FALSE, FALSE, 5);
    
    // Add Models section
    GtkWidget *models_frame = gtk_frame_new("Model Management");
    gtk_box_pack_start(GTK_BOX(main_box), models_frame, FALSE, FALSE, 5);
    
    GtkWidget *models_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(models_box), 10);
    gtk_container_add(GTK_CONTAINER(models_frame), models_box);
    
    // Models status label - set initial status based on startup check
    widgets->models_status_label = gtk_label_new(nullptr);
    if (models_available) {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label), 
                            "<span foreground='green'><b>✓ Models are downloaded</b></span>");
    } else {
        gtk_label_set_markup(GTK_LABEL(widgets->models_status_label), 
                            "<span foreground='red'><b>✗ Models not found - download required</b></span>");
    }
    gtk_label_set_line_wrap(GTK_LABEL(widgets->models_status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(models_box), widgets->models_status_label, FALSE, FALSE, 5);
    
    // Download models button - enable only if models are not available
    widgets->download_models_button = gtk_button_new_with_label("Download Models");
    g_signal_connect(widgets->download_models_button, "clicked", G_CALLBACK(on_download_models_clicked), widgets.get());
    gtk_widget_set_sensitive(widgets->download_models_button, !models_available);
    gtk_box_pack_start(GTK_BOX(models_box), widgets->download_models_button, FALSE, FALSE, 0);
    
    // Add Videos section
    GtkWidget *videos_frame = gtk_frame_new("Video Management");
    gtk_box_pack_start(GTK_BOX(main_box), videos_frame, FALSE, FALSE, 5);
    
    GtkWidget *videos_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(videos_box), 10);
    gtk_container_add(GTK_CONTAINER(videos_frame), videos_box);
    
    // Videos status label - set initial status based on startup check
    widgets->videos_status_label = gtk_label_new(nullptr);
    if (videos_available) {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label), 
                            "<span foreground='green'><b>✓ Sample videos are downloaded</b></span>");
    } else {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label), 
                            "<span foreground='red'><b>✗ Sample videos not found - download required</b></span>");
    }
    gtk_label_set_line_wrap(GTK_LABEL(widgets->videos_status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(videos_box), widgets->videos_status_label, FALSE, FALSE, 5);
    
    // Download videos button - enable only if videos are not available
    widgets->download_videos_button = gtk_button_new_with_label("Download Sample Videos");
    g_signal_connect(widgets->download_videos_button, "clicked", G_CALLBACK(on_download_videos_clicked), widgets.get());
    gtk_widget_set_sensitive(widgets->download_videos_button, !videos_available);
    gtk_box_pack_start(GTK_BOX(videos_box), widgets->download_videos_button, FALSE, FALSE, 0);

    // Create grid for parameters
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(main_box), grid, TRUE, TRUE, 0);
    
    int row = 0;
    
    // Stream parameter (1-8)
    label = gtk_label_new("Number of Streams (1-8):");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    
    widgets->stream_spin = gtk_spin_button_new_with_range(1, 8, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->stream_spin), 8);
    gtk_grid_attach(GTK_GRID(grid), widgets->stream_spin, 1, row, 1, 1);
    row++;
    
    // Endpoint parameter (0 to detected max, up to 10)
    std::string endpoint_label_text;
    if (max_endpoints > 0) {
        endpoint_label_text = "ARA2 Endpoint (0-" + 
                             std::to_string(max_endpoints > 10 ? 10 : max_endpoints - 1) + "):";
    } else {
        endpoint_label_text = "ARA2 Endpoint (No endpoints detected):";
    }
    label = gtk_label_new(endpoint_label_text.c_str());
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    
    int max_endpoint_value = (max_endpoints > 0) ? (max_endpoints > 10 ? 10 : max_endpoints - 1) : 0;
    widgets->endpoint_spin = gtk_spin_button_new_with_range(0, max_endpoint_value, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->endpoint_spin), 0);
    // Disable endpoint spin if no endpoints detected
    if (max_endpoints == 0) {
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
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo), "yolov8n");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo), "yolov8s");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo), "yolov8m");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo), "yolov8l");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets->model_combo), "yolov8x");
    gtk_combo_box_set_active(GTK_COMBO_BOX(widgets->model_combo), 0);
    gtk_grid_attach(GTK_GRID(grid), widgets->model_combo, 1, row, 1, 1);
    row++;
    
    // Add separator for display options
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), separator, 0, row, 2, 1);
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
                          [](gpointer data) { delete static_cast<AppWidgets*>(data); });
    
    // Show all widgets
    gtk_widget_show_all(window);
}

static bool check_videos_downloaded() {
    FILE *fp;
    char buffer[256];
    bool videos_found = false;
    
    // Check if sample videos directory exists and has video files
    fp = popen("ls -lh /usr/share/ara2-vision-examples/sample_videos/video_*.mp4 2>/dev/null", "r");
    if (fp == nullptr) {
        return false;
    }
    
    // If we get any output, videos exist
    if (fgets(buffer, sizeof(buffer), fp) != nullptr) {
        videos_found = true;
    }
    
    pclose(fp);
    return videos_found;
}

static void on_download_videos_clicked(GtkWidget *button, gpointer user_data) {
    auto *widgets = static_cast<AppWidgets*>(user_data);
    (void)button;
    
    gtk_label_set_text(GTK_LABEL(widgets->videos_status_label), 
                      "Downloading videos... This may take some time. Please wait...");
    gtk_widget_set_sensitive(widgets->download_videos_button, FALSE);
    
    // Force UI update
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    
    // Execute download command
    int result = system("fetch_videos.sh");
    
    if (result == 0) {
        // Verify videos were actually downloaded
        bool videos_available = check_videos_downloaded();
        
        if (videos_available) {
            gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label), 
                                "<span foreground='green'><b>✓ Videos downloaded successfully!</b></span>");
            
            // Keep download button disabled since videos are now available
            gtk_widget_set_sensitive(widgets->download_videos_button, FALSE);
        } else {
            gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label), 
                                "<span foreground='orange'><b>⚠ Download completed but videos not detected. Please check manually.</b></span>");
            gtk_widget_set_sensitive(widgets->download_videos_button, TRUE);
        }
    } else {
        gtk_label_set_markup(GTK_LABEL(widgets->videos_status_label), 
                            "<span foreground='red'><b>✗ Failed to download videos. Check console for errors.</b></span>");
        gtk_widget_set_sensitive(widgets->download_videos_button, TRUE);
    }
}

static gboolean on_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget;
    (void)event;
    (void)user_data;
    
    // Kill the multistream application before closing
    kill_multistream_app();
    
    // Return FALSE to allow the window to close
    // Return TRUE would prevent the window from closing
    return FALSE;
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.yololauncher", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}