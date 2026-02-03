/*
	UDP and ADIF Broadcast Settings UI

	This dialog allows configuration of:
	- ADIF broadcast enable/disable and destinations
	- UDP (WSJT-X protocol) broadcast enable/disable and destinations

	Destinations are managed as individual entries with add/remove functionality.
	Multicast addresses (224.0.0.0 - 239.255.255.255) are supported.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include "sdr_ui.h"

// Re-initialize broadcast sockets after settings change
extern int adif_broadcast_init(void);
extern int udp_broadcast_init(void);

// Maximum number of destinations per broadcast type
#define MAX_DESTINATIONS 10

// Struct to hold list UI components
typedef struct {
    GtkWidget *list_box;
    GtkWidget *entry;
    GtkWidget *add_button;
    GtkWidget *remove_button;
    int count;
} DestinationList;

// Validate a single destination: IP:port format
static gboolean validate_single_destination(const char *dest) {
    if (!dest || !*dest) return FALSE;

    // Trim whitespace
    while (*dest == ' ') dest++;
    if (!*dest) return FALSE;

    // Check for IP:port format
    const char *colon = strrchr(dest, ':');
    if (!colon || colon == dest || !*(colon + 1)) {
        return FALSE;
    }

    // Check port is numeric and in valid range
    for (const char *p = colon + 1; *p && *p != ' '; p++) {
        if (!isdigit(*p)) {
            return FALSE;
        }
    }

    int port = atoi(colon + 1);
    if (port < 1 || port > 65535) {
        return FALSE;
    }

    return TRUE;
}

// Create a row widget for the list
static GtkWidget *create_list_row(const char *text) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 10);
    gtk_widget_set_margin_end(label, 10);
    gtk_widget_set_margin_top(label, 5);
    gtk_widget_set_margin_bottom(label, 5);
    gtk_container_add(GTK_CONTAINER(row), label);
    return row;
}

// Get the text from a list row
static const char *get_row_text(GtkListBoxRow *row) {
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(row));
    return gtk_label_get_text(GTK_LABEL(label));
}

// Update remove button sensitivity based on selection
static void on_list_selection_changed(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data) {
    GtkWidget *remove_button = GTK_WIDGET(user_data);
    gtk_widget_set_sensitive(remove_button, row != NULL);
}

// Add button callback
static void on_add_clicked(GtkButton *button, gpointer user_data) {
    DestinationList *dl = (DestinationList *)user_data;

    const char *text = gtk_entry_get_text(GTK_ENTRY(dl->entry));
    if (!text || !*text) return;

    // Trim whitespace
    char trimmed[256];
    strncpy(trimmed, text, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    char *start = trimmed;
    while (*start == ' ') start++;
    char *end = start + strlen(start) - 1;
    while (end > start && *end == ' ') *end-- = '\0';

    if (!*start) return;

    // Validate
    if (!validate_single_destination(start)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Invalid format. Use IP:port (e.g., 127.0.0.1:2237)");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    // Check max entries
    if (dl->count >= MAX_DESTINATIONS) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Maximum %d destinations allowed.", MAX_DESTINATIONS);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    // Check for duplicates
    GList *children = gtk_container_get_children(GTK_CONTAINER(dl->list_box));
    for (GList *iter = children; iter != NULL; iter = iter->next) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(iter->data);
        if (strcmp(get_row_text(row), start) == 0) {
            g_list_free(children);
            GtkWidget *dialog = gtk_message_dialog_new(
                GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
                GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                "This destination already exists.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return;
        }
    }
    g_list_free(children);

    // Add to list
    GtkWidget *row = create_list_row(start);
    gtk_list_box_insert(GTK_LIST_BOX(dl->list_box), row, -1);
    gtk_widget_show_all(row);
    dl->count++;

    // Clear entry
    gtk_entry_set_text(GTK_ENTRY(dl->entry), "");
}

// Remove button callback
static void on_remove_clicked(GtkButton *button, gpointer user_data) {
    DestinationList *dl = (DestinationList *)user_data;

    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(dl->list_box));
    if (row) {
        gtk_container_remove(GTK_CONTAINER(dl->list_box), GTK_WIDGET(row));
        dl->count--;
        gtk_widget_set_sensitive(dl->remove_button, FALSE);
    }
}

// Handle Enter key in entry field
static gboolean on_entry_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        DestinationList *dl = (DestinationList *)user_data;
        on_add_clicked(GTK_BUTTON(dl->add_button), user_data);
        return TRUE;
    }
    return FALSE;
}

// Create a destination list widget group
static GtkWidget *create_destination_list(DestinationList *dl, const char *current_value) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    // Scrolled window for the list
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 100);
    gtk_widget_set_vexpand(scrolled, TRUE);

    // List box
    dl->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dl->list_box), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(scrolled), dl->list_box);

    // Frame around the list
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(frame), scrolled);
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);

    // Populate with current values
    dl->count = 0;
    if (current_value && *current_value) {
        char *copy = g_strdup(current_value);
        char *token = strtok(copy, ";");
        while (token && dl->count < MAX_DESTINATIONS) {
            // Trim whitespace
            while (*token == ' ') token++;
            char *end = token + strlen(token) - 1;
            while (end > token && *end == ' ') *end-- = '\0';

            if (*token) {
                GtkWidget *row = create_list_row(token);
                gtk_list_box_insert(GTK_LIST_BOX(dl->list_box), row, -1);
                dl->count++;
            }
            token = strtok(NULL, ";");
        }
        g_free(copy);
    }

    // Entry + Add button row
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    dl->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dl->entry), "IP:port (e.g., 127.0.0.1:2237)");
    gtk_widget_set_hexpand(dl->entry, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), dl->entry, TRUE, TRUE, 0);

    dl->add_button = gtk_button_new_with_label("Add");
    gtk_box_pack_start(GTK_BOX(hbox), dl->add_button, FALSE, FALSE, 0);

    dl->remove_button = gtk_button_new_with_label("Remove");
    gtk_widget_set_sensitive(dl->remove_button, FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), dl->remove_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    // Connect signals
    g_signal_connect(dl->list_box, "row-selected",
                     G_CALLBACK(on_list_selection_changed), dl->remove_button);
    g_signal_connect(dl->add_button, "clicked", G_CALLBACK(on_add_clicked), dl);
    g_signal_connect(dl->remove_button, "clicked", G_CALLBACK(on_remove_clicked), dl);
    g_signal_connect(dl->entry, "key-press-event", G_CALLBACK(on_entry_key_press), dl);

    return vbox;
}

// Build semicolon-delimited string from list
static char *build_destinations_string(GtkListBox *list_box) {
    GString *result = g_string_new("");
    GList *children = gtk_container_get_children(GTK_CONTAINER(list_box));

    for (GList *iter = children; iter != NULL; iter = iter->next) {
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(iter->data);
        const char *text = get_row_text(row);
        if (result->len > 0) {
            g_string_append_c(result, ';');
        }
        g_string_append(result, text);
    }

    g_list_free(children);
    return g_string_free(result, FALSE);
}

// Create the UDP/ADIF settings dialog
void udp_settings_ui(GtkWidget *parent) {
    GtkWidget *dialog, *notebook, *grid, *label;
    GtkWidget *adif_enable_check, *udp_enable_check;
    GtkWidget *help_label;
    DestinationList adif_list = {0};
    DestinationList udp_list = {0};

    // Create dialog
    dialog = gtk_dialog_new_with_buttons("UDP/ADIF Broadcast Settings",
        GTK_WINDOW(parent),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "OK", GTK_RESPONSE_ACCEPT,
        "Cancel", GTK_RESPONSE_CANCEL,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 450, 400);

    // Create notebook for tabbed interface
    notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(notebook), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       notebook, TRUE, TRUE, 0);

    // === ADIF Tab ===
    GtkWidget *adif_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(adif_page), 10);

    // ADIF Enable checkbox
    adif_enable_check = gtk_check_button_new_with_label("Enable ADIF Broadcast");
    const char *adif_enabled = field_str("ADIF_ENABLE");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(adif_enable_check),
        adif_enabled && g_ascii_strcasecmp(adif_enabled, "ON") == 0);
    gtk_box_pack_start(GTK_BOX(adif_page), adif_enable_check, FALSE, FALSE, 0);

    // ADIF Destinations label
    label = gtk_label_new("Destinations:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(adif_page), label, FALSE, FALSE, 0);

    // ADIF Destination list
    GtkWidget *adif_list_widget = create_destination_list(&adif_list,
        field_str("ADIF_DESTINATIONS"));
    gtk_box_pack_start(GTK_BOX(adif_page), adif_list_widget, TRUE, TRUE, 0);

    // ADIF help text
    help_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(help_label),
        "<small>ADIF records are broadcast when QSOs are logged.</small>");
    gtk_widget_set_halign(help_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(adif_page), help_label, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), adif_page,
                             gtk_label_new("ADIF"));

    // === UDP Tab ===
    GtkWidget *udp_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(udp_page), 10);

    // UDP Enable checkbox
    udp_enable_check = gtk_check_button_new_with_label("Enable UDP Broadcast (WSJT-X Protocol)");
    const char *udp_enabled = field_str("UDP_ENABLE");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(udp_enable_check),
        udp_enabled && g_ascii_strcasecmp(udp_enabled, "ON") == 0);
    gtk_box_pack_start(GTK_BOX(udp_page), udp_enable_check, FALSE, FALSE, 0);

    // UDP Destinations label
    label = gtk_label_new("Destinations:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(udp_page), label, FALSE, FALSE, 0);

    // UDP Destination list
    GtkWidget *udp_list_widget = create_destination_list(&udp_list,
        field_str("UDP_DESTINATIONS"));
    gtk_box_pack_start(GTK_BOX(udp_page), udp_list_widget, TRUE, TRUE, 0);

    // UDP help text
    help_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(help_label),
        "<small>UDP broadcasts FT8/FT4 decodes using WSJT-X protocol.\n"
        "Compatible with GridTracker and similar apps. Default port: 2237</small>");
    gtk_widget_set_halign(help_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(udp_page), help_label, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), udp_page,
                             gtk_label_new("UDP"));

    // Show dialog
    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_ACCEPT) {
        // Save ADIF settings
        gboolean adif_on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(adif_enable_check));
        field_set("ADIF_ENABLE", adif_on ? "ON" : "OFF");

        char *adif_dest = build_destinations_string(GTK_LIST_BOX(adif_list.list_box));
        field_set("ADIF_DESTINATIONS", adif_dest ? adif_dest : "");
        g_free(adif_dest);

        // Save UDP settings
        gboolean udp_on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(udp_enable_check));
        field_set("UDP_ENABLE", udp_on ? "ON" : "OFF");

        char *udp_dest = build_destinations_string(GTK_LIST_BOX(udp_list.list_box));
        if (udp_dest && *udp_dest) {
            field_set("UDP_DESTINATIONS", udp_dest);
        } else {
            field_set("UDP_DESTINATIONS", "127.0.0.1:2237");
        }
        g_free(udp_dest);

        // Reinitialize broadcast sockets with new settings
        adif_broadcast_init();
        udp_broadcast_init();

        // Notify user
        write_console(STYLE_LOG, "\n[UDP/ADIF settings updated]\n");
    }

    gtk_widget_destroy(dialog);
}
