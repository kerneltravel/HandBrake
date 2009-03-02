/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * callbacks.c
 * Copyright (C) John Stebbins 2008 <stebbins@stebbins>
 * 
 * callbacks.c is free software.
 * 
 * You may redistribute it and/or modify it under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <libhal-storage.h>
#include <gtk/gtk.h>
#include <gtkhtml/gtkhtml.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <gio/gio.h>

#include "hb.h"
#include "callbacks.h"
#include "queuehandler.h"
#include "audiohandler.h"
#include "resources.h"
#include "settings.h"
#include "presets.h"
#include "preview.h"
#include "values.h"
#include "plist.h"
#include "appcast.h"
#include "hb-backend.h"
#include "ghb-dvd.h"
#include "ghbcellrenderertext.h"

static void update_chapter_list(signal_user_data_t *ud);
static GList* dvd_device_list();
static void prune_logs(signal_user_data_t *ud);

// This is a dependency map used for greying widgets
// that are dependent on the state of another widget.
// The enable_value comes from the values that are
// obtained from ghb_widget_value().  For combo boxes
// you will have to look further to combo box options
// maps in hb-backend.c

GValue *dep_map;
GValue *rev_map;

void
ghb_init_dep_map()
{
	dep_map = ghb_resource_get("widget-deps");
	rev_map = ghb_resource_get("widget-reverse-deps");
}

static gboolean
dep_check(signal_user_data_t *ud, const gchar *name, gboolean *out_hide)
{
	GtkWidget *widget;
	GObject *dep_object;
	gint ii;
	gint count;
	gboolean result = TRUE;
	GValue *array, *data;
	gchar *widget_name;
	
	g_debug("dep_check () %s", name);

	array = ghb_dict_lookup(rev_map, name);
	count = ghb_array_len(array);
	*out_hide = FALSE;
	for (ii = 0; ii < count; ii++)
	{
		data = ghb_array_get_nth(array, ii);
		widget_name = ghb_value_string(ghb_array_get_nth(data, 0));
		widget = GHB_WIDGET(ud->builder, widget_name);
		dep_object = gtk_builder_get_object(ud->builder, name);
		g_free(widget_name);
		if (!GTK_WIDGET_SENSITIVE(widget))
			continue;
		if (dep_object == NULL)
		{
			g_message("Failed to find widget");
		}
		else
		{
			gchar *value;
			gint jj = 0;
			gchar **values;
			gboolean sensitive = FALSE;
			gboolean die, hide;

			die = ghb_value_boolean(ghb_array_get_nth(data, 2));
			hide = ghb_value_boolean(ghb_array_get_nth(data, 3));
			value = ghb_value_string(ghb_array_get_nth(data, 1));
			values = g_strsplit(value, "|", 10);
			g_free(value);

			if (widget)
				value = ghb_widget_string(widget);
			else
				value = ghb_settings_get_string(ud->settings, name);
			while (values && values[jj])
			{
				if (values[jj][0] == '>')
				{
					gdouble dbl = g_strtod (&values[jj][1], NULL);
					gdouble dvalue = ghb_widget_double(widget);
					if (dvalue > dbl)
					{
						sensitive = TRUE;
						break;
					}
				}
				else if (values[jj][0] == '<')
				{
					gdouble dbl = g_strtod (&values[jj][1], NULL);
					gdouble dvalue = ghb_widget_double(widget);
					if (dvalue < dbl)
					{
						sensitive = TRUE;
						break;
					}
				}
				if (strcmp(values[jj], value) == 0)
				{
					sensitive = TRUE;
					break;
				}
				jj++;
			}
			sensitive = die ^ sensitive;
			if (!sensitive)
			{
				result = FALSE;
				*out_hide |= hide;
			}
			g_strfreev (values);
			g_free(value);
		}
	}
	return result;
}

void
ghb_check_dependency(signal_user_data_t *ud, GtkWidget *widget)
{
	GObject *dep_object;
	const gchar *name;
	GValue *array, *data;
	gint count, ii;
	gchar *dep_name;
	GType type;

	type = GTK_WIDGET_TYPE(widget);
	if (type == GTK_TYPE_COMBO_BOX || type == GTK_TYPE_COMBO_BOX_ENTRY)
		if (gtk_combo_box_get_active(GTK_COMBO_BOX(widget)) < 0) return;

	name = gtk_widget_get_name(widget);
	g_debug("ghb_check_dependency () %s", name);

	array = ghb_dict_lookup(dep_map, name);
	count = ghb_array_len(array);
	for (ii = 0; ii < count; ii++)
	{
		gboolean sensitive;
		gboolean hide;

		data = ghb_array_get_nth(array, ii);
		dep_name = ghb_value_string(data);
		dep_object = gtk_builder_get_object(ud->builder, dep_name);
		if (dep_object == NULL)
		{
			g_message("Failed to find dependent widget %s", dep_name);
			g_free(dep_name);
			continue;
		}
		sensitive = dep_check(ud, dep_name, &hide);
		g_free(dep_name);
		if (GTK_IS_ACTION(dep_object))
		{
			gtk_action_set_sensitive(GTK_ACTION(dep_object), sensitive);
			gtk_action_set_visible(GTK_ACTION(dep_object), sensitive || !hide);
		}
		else
		{
			gtk_widget_set_sensitive(GTK_WIDGET(dep_object), sensitive);
			if (!sensitive && hide)
			{
				gtk_widget_hide(GTK_WIDGET(dep_object));
			}
			else
			{
				gtk_widget_show_now(GTK_WIDGET(dep_object));
			}
		}
	}
}

void
ghb_check_all_depencencies(signal_user_data_t *ud)
{
	GHashTableIter iter;
	gchar *dep_name;
	GValue *value;
	GObject *dep_object;

	g_debug("ghb_check_all_depencencies ()");
	ghb_dict_iter_init(&iter, rev_map);
	// middle (void*) cast prevents gcc warning "defreferencing type-punned
	// pointer will break strict-aliasing rules"
	while (g_hash_table_iter_next(
			&iter, (gpointer*)(void*)&dep_name, (gpointer*)(void*)&value))
	{
		gboolean sensitive;
		gboolean hide;

		dep_object = gtk_builder_get_object (ud->builder, dep_name);
		if (dep_object == NULL)
		{
			g_message("Failed to find dependent widget %s", dep_name);
			continue;
		}
		sensitive = dep_check(ud, dep_name, &hide);
		if (GTK_IS_ACTION(dep_object))
		{
			gtk_action_set_sensitive(GTK_ACTION(dep_object), sensitive);
			gtk_action_set_visible(GTK_ACTION(dep_object), sensitive || !hide);
		}
		else
		{
			gtk_widget_set_sensitive(GTK_WIDGET(dep_object), sensitive);
			if (!sensitive && hide)
			{
				gtk_widget_hide(GTK_WIDGET(dep_object));
			}
			else
			{
				gtk_widget_show_now(GTK_WIDGET(dep_object));
			}
		}
	}
}

void
on_quit1_activate(GtkMenuItem *quit, signal_user_data_t *ud)
{
	gint state = ghb_get_queue_state();
	g_debug("on_quit1_activate ()");
	if (state & GHB_STATE_WORKING)
   	{
		if (ghb_cancel_encode("Closing HandBrake will terminate encoding.\n"))
		{
			ghb_hb_cleanup(FALSE);
			prune_logs(ud);
			gtk_main_quit();
			return;
		}
		return;
	}
	ghb_hb_cleanup(FALSE);
	prune_logs(ud);
	gtk_main_quit();
}

static void
set_destination(signal_user_data_t *ud)
{
	g_debug("set_destination");
	if (ghb_settings_get_boolean(ud->settings, "use_source_name"))
	{
		GString *str = g_string_new("");
		gchar *vol_name, *filename, *extension;
		gchar *new_name;
		gint title;
		
		filename = ghb_settings_get_string(ud->settings, "dest_file");
		extension = ghb_settings_get_string(ud->settings, "FileFormat");
		vol_name = ghb_settings_get_string(ud->settings, "volume_label");
		g_string_append_printf(str, "%s", vol_name);
		title = ghb_settings_combo_int(ud->settings, "title");
		if (title >= 0)
		{
			if (ghb_settings_get_boolean(
					ud->settings, "title_no_in_destination"))
			{

				title = ghb_settings_combo_int(ud->settings, "title");
				g_string_append_printf(str, " - %d", title+1);
			}
			if (ghb_settings_get_boolean(
					ud->settings, "chapters_in_destination"))
			{
				gint start, end;

				if (!ghb_settings_get_boolean(
						ud->settings, "title_no_in_destination"))
				{
					g_string_append_printf(str, " -");
				}
				start = ghb_settings_get_int(ud->settings, "start_chapter");
				end = ghb_settings_get_int(ud->settings, "end_chapter");
				if (start == end)
					g_string_append_printf(str, " Ch %d", start);
				else
					g_string_append_printf(str, " Ch %d-%d", start, end);
			}
		}
		g_string_append_printf(str, ".%s", extension);
		new_name = g_string_free(str, FALSE);
		ghb_ui_update(ud, "dest_file", ghb_string_value(new_name));
		g_free(filename);
		g_free(extension);
		g_free(vol_name);
		g_free(new_name);
	}
}

gboolean
uppers_and_unders(const gchar *str)
{
	if (str == NULL) return FALSE;
	while (*str)
	{
		if (*str == ' ')
		{
			return FALSE;
		}
		if (*str >= 'a' && *str <= 'z')
		{
			return FALSE;
		}
		str++;
	}
	return TRUE;
}

enum
{
	CAMEL_FIRST_UPPER,
	CAMEL_OTHER
};

void
camel_convert(gchar *str)
{
	gint state = CAMEL_OTHER;
	
	if (str == NULL) return;
	while (*str)
	{
		if (*str == '_') *str = ' ';
		switch (state)
		{
			case CAMEL_OTHER:
			{
				if (*str >= 'A' && *str <= 'Z')
					state = CAMEL_FIRST_UPPER;
				else
					state = CAMEL_OTHER;
				
			} break;
			case CAMEL_FIRST_UPPER:
			{
				if (*str >= 'A' && *str <= 'Z')
					*str = *str - 'A' + 'a';
				else
					state = CAMEL_OTHER;
			} break;
		}
		str++;
	}
}

static gchar*
get_file_label(const gchar *filename)
{
	static gchar *containers[] = 
		{".vob", ".mpg", ".m2ts", ".mkv", ".mp4", ".m4v", ".avi", ".ogm", NULL};
	gchar *base;
	gint ii;

	base = g_path_get_basename(filename);
	for (ii = 0; containers[ii] != NULL; ii++)
	{
		if (g_str_has_suffix(base, containers[ii]))
		{
			gchar *pos;
			pos = strrchr(base, '.');
			*pos = 0;
			break;
		}
	}
	return base;
}

static gboolean
update_source_label(signal_user_data_t *ud, const gchar *source)
{
	gchar *label = NULL;
	gint len;
	gchar **path;
	gchar *filename = g_strdup(source);
	
	len = strlen(filename);
	if (filename[len-1] == '/') filename[len-1] = 0;
	if (g_file_test(filename, G_FILE_TEST_IS_DIR))
	{
		path = g_strsplit(filename, "/", -1);
		len = g_strv_length (path);
		if ((len > 1) && (strcmp("VIDEO_TS", path[len-1]) == 0))
		{
			label = g_strdup(path[len-2]);
		}
		else
		{
			label = g_strdup(path[len-1]);
		}
		g_strfreev (path);
	}
	else
	{
		// Is regular file or block dev.
		// Check to see if it is a dvd image
		label = ghb_dvd_volname (filename);
		if (label == NULL)
		{
			label = get_file_label(filename);
		}
		else
		{
			if (uppers_and_unders(label))
			{
				camel_convert(label);
			}
		}
	}
	g_free(filename);
	GtkWidget *widget = GHB_WIDGET (ud->builder, "source_title");
	if (label != NULL)
	{
		gtk_label_set_text (GTK_LABEL(widget), label);
		ghb_settings_set_string(ud->settings, "volume_label", label);
		g_free(label);
		set_destination(ud);
	}
	else
	{
		label = "No Title Found";
		gtk_label_set_text (GTK_LABEL(widget), label);
		ghb_settings_set_string(ud->settings, "volume_label", label);
		return FALSE;
	}
	return TRUE;
}

void
chooser_file_selected_cb(GtkFileChooser *dialog, signal_user_data_t *ud)
{
	const gchar *name = gtk_file_chooser_get_filename (dialog);
	GtkTreeModel *store;
	GtkTreeIter iter;
	const gchar *device;
	gboolean foundit = FALSE;
	GtkComboBox *combo;
	
	if (name == NULL) return;
	combo = GTK_COMBO_BOX(GHB_WIDGET(ud->builder, "source_device"));
	store = gtk_combo_box_get_model(combo);
	if (gtk_tree_model_get_iter_first(store, &iter))
	{
		do
		{
			gtk_tree_model_get(store, &iter, 0, &device, -1);
			if (strcmp(name, device) == 0)
			{
				foundit = TRUE;
				break;
			}
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter));
	}
	if (foundit)
		gtk_combo_box_set_active_iter (combo, &iter);
	else
		gtk_combo_box_set_active (combo, 0);
}

void
dvd_device_changed_cb(GtkComboBox *combo, signal_user_data_t *ud)
{
	GtkWidget *dialog;
	gint ii;

	ii = gtk_combo_box_get_active (combo);
	if (ii > 0)
	{
		const gchar *device, *name;

		dialog = GHB_WIDGET(ud->builder, "source_dialog");
		device = gtk_combo_box_get_active_text (combo);
		name = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER(dialog));
		if (name == NULL || strcmp(name, device) != 0)
			gtk_file_chooser_select_filename (GTK_FILE_CHOOSER(dialog), device);
	}
}

void
source_type_changed_cb(GtkToggleButton *toggle, signal_user_data_t *ud)
{
	gchar *folder;
	GtkFileChooser *chooser;
	GtkWidget *dvd_device_combo;
	
	g_debug("source_type_changed_cb ()");
	chooser = GTK_FILE_CHOOSER(GHB_WIDGET(ud->builder, "source_dialog"));
	dvd_device_combo = GHB_WIDGET(ud->builder, "source_device");
	folder = gtk_file_chooser_get_current_folder (chooser);
	if (gtk_toggle_button_get_active (toggle))
	{
		gtk_file_chooser_set_action (chooser, 
									GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
		gtk_widget_set_sensitive (dvd_device_combo, FALSE);
		gtk_combo_box_set_active (GTK_COMBO_BOX(dvd_device_combo), 0);
	}
	else
	{
		gtk_file_chooser_set_action (chooser, GTK_FILE_CHOOSER_ACTION_OPEN);
		gtk_widget_set_sensitive (dvd_device_combo, TRUE);
	}
	if (folder != NULL)
	{
		gtk_file_chooser_set_current_folder(chooser, folder);
		g_free(folder);
	}
}

static void
source_dialog_extra_widgets(
	signal_user_data_t *ud,
	GtkWidget *dialog, 
	gboolean checkbutton_active)
{
	GtkToggleButton *checkbutton;
	GtkComboBox *combo;
	GList *drives, *link;
	
	checkbutton = GTK_TOGGLE_BUTTON(
		GHB_WIDGET(ud->builder, "source_folder_flag"));
	gtk_toggle_button_set_active(checkbutton, checkbutton_active);
	combo = GTK_COMBO_BOX(GHB_WIDGET(ud->builder, "source_device"));
	gtk_list_store_clear(GTK_LIST_STORE(
						gtk_combo_box_get_model(combo)));

	link = drives = dvd_device_list();
	gtk_combo_box_append_text (combo, "Not Selected");
	while (link != NULL)
	{
		gchar *name = (gchar*)link->data;
		gtk_combo_box_append_text(combo, name);
		g_free(name);
		link = link->next;
	}
	g_list_free(drives);
}

extern GValue *ghb_queue_edit_settings;
static gchar *last_scan_file = NULL;

void
ghb_do_scan(
	signal_user_data_t *ud, 
	const gchar *filename, 
	gint titlenum, 
	gboolean force)
{
	if (!force && last_scan_file != NULL &&
		strcmp(last_scan_file, filename) == 0)
	{
		if (ghb_queue_edit_settings)
		{
			gint jstatus;

			jstatus = ghb_settings_get_int(ghb_queue_edit_settings, "job_status");
			ghb_settings_to_ui(ud, ghb_queue_edit_settings);
			ghb_set_audio(ud, ghb_queue_edit_settings);
			if (jstatus == GHB_QUEUE_PENDING)
			{
				ghb_value_free(ghb_queue_edit_settings);
			}
			ghb_queue_edit_settings = NULL;
		}
		return;
	}
	if (last_scan_file != NULL)
		g_free(last_scan_file);
	last_scan_file = NULL;
	if (filename != NULL)
	{
		last_scan_file = g_strdup(filename);
		ghb_settings_set_string(ud->settings, "source", filename);
		if (update_source_label(ud, filename))
		{
			GtkProgressBar *progress;
			progress = GTK_PROGRESS_BAR(GHB_WIDGET(ud->builder, "progressbar"));
			gchar *path;
			path = ghb_settings_get_string( ud->settings, "source");
			gtk_progress_bar_set_fraction (progress, 0);
			gtk_progress_bar_set_text (progress, "Scanning ...");
			ghb_hb_cleanup(TRUE);
			prune_logs(ud);
			gint preview_count;
			preview_count = ghb_settings_get_int(ud->settings, "preview_count");
			ghb_backend_scan(path, titlenum, preview_count);
			g_free(path);
		}
		else
		{
			// TODO: error dialog
		}
	}
}

static gboolean 
update_source_name(gpointer data)
{
	signal_user_data_t *ud = (signal_user_data_t*)data;
	GtkWidget *dialog;
	gchar *sourcename;

	sourcename = ghb_settings_get_string(ud->settings, "source");
	dialog = GHB_WIDGET(ud->builder, "source_dialog");
	gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), sourcename);
	g_free(sourcename);
	return FALSE;
}

static void
do_source_dialog(GtkButton *button, gboolean single, signal_user_data_t *ud)
{
	GtkWidget *dialog;
	gchar *sourcename;
	gint	response;
	GtkFileChooserAction action;
	gboolean checkbutton_active;

	g_debug("source_browse_clicked_cb ()");
	sourcename = ghb_settings_get_string(ud->settings, "source");
	checkbutton_active = FALSE;
	if (g_file_test(sourcename, G_FILE_TEST_IS_DIR))
	{
		action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
		checkbutton_active = TRUE;
	}
	else
	{
		action = GTK_FILE_CHOOSER_ACTION_OPEN;
	}
	GtkWidget *widget;
	widget = GHB_WIDGET(ud->builder, "single_title_box");
	if (single)
		gtk_widget_show(widget);
	else
		gtk_widget_hide(widget);
	dialog = GHB_WIDGET(ud->builder, "source_dialog");
	source_dialog_extra_widgets(ud, dialog, checkbutton_active);
	gtk_file_chooser_set_action(GTK_FILE_CHOOSER(dialog), action);
	// Updating the filename in the file chooser dialog doesn't seem
	// to work unless the dialog is running for some reason.
	// So handle it in an "idle" event.
	//gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), sourcename);
	g_idle_add((GSourceFunc)update_source_name, ud);
	response = gtk_dialog_run(GTK_DIALOG (dialog));
	gtk_widget_hide(dialog);
	if (response == GTK_RESPONSE_ACCEPT)
	{
		char *filename;

		filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
		if (filename != NULL)
		{
			gint titlenum;

			if (single)
				titlenum = ghb_settings_get_int(ud->settings, "single_title");
			else
				titlenum = 0;
			ghb_do_scan(ud, filename, titlenum, TRUE);
			if (strcmp(sourcename, filename) != 0)
			{
				ghb_settings_set_string (ud->settings, 
										"default_source", filename);
				ghb_pref_save (ud->settings, "default_source");
				ghb_dvd_set_current (filename, ud);
			}
			g_free(filename);
		}
	}
	g_free(sourcename);
}

void
source_button_clicked_cb(GtkButton *button, signal_user_data_t *ud)
{
	do_source_dialog(button, FALSE, ud);
}

void
single_title_source_cb(GtkButton *button, signal_user_data_t *ud)
{
	do_source_dialog(button, TRUE, ud);
}

void
dvd_source_activate_cb(GtkAction *action, signal_user_data_t *ud)
{
	const gchar *filename;
	gchar *sourcename;

	sourcename = ghb_settings_get_string(ud->settings, "source");
	filename = gtk_action_get_name(action);
	ghb_do_scan(ud, filename, 0, TRUE);
	if (strcmp(sourcename, filename) != 0)
	{
		ghb_settings_set_string (ud->settings, "default_source", filename);
		ghb_pref_save (ud->settings, "default_source");
		ghb_dvd_set_current (filename, ud);
	}
	g_free(sourcename);
}

static void
update_destination_extension(signal_user_data_t *ud)
{
	static gchar *containers[] = {".mkv", ".mp4", ".m4v", ".avi", ".ogm", NULL};
	gchar *filename;
	gchar *extension;
	gint ii;
	GtkEntry *entry;

	g_debug("update_destination_extension ()");
	extension = ghb_settings_get_string(ud->settings, "FileFormat");
	entry = GTK_ENTRY(GHB_WIDGET(ud->builder, "dest_file"));
	filename = g_strdup(gtk_entry_get_text(entry));
	for (ii = 0; containers[ii] != NULL; ii++)
	{
		if (g_str_has_suffix(filename, containers[ii]))
		{
			gchar *pos;
			gchar *new_name;
			
			pos = g_strrstr( filename, "." );
			if (pos == NULL)
			{
				// No period? shouldn't happen
				break;
			}
			*pos = 0;
			if (strcmp(extension, &pos[1]) == 0)
			{
				// Extension is already correct
				break;
			}
			new_name = g_strjoin(".", filename, extension, NULL); 
			ghb_ui_update(ud, "dest_file", ghb_string_value(new_name));
			g_free(new_name);
			break;
		}
	}
	g_free(extension);
	g_free(filename);
}

static void
destination_select_title(GtkEntry *entry)
{
	const gchar *dest;
	gint start, end;

	dest = gtk_entry_get_text(entry);
	for (end = strlen(dest)-1; end > 0; end--)
	{
		if (dest[end] == '.')
		{
			break;
		}
	}
	for (start = end; start >= 0; start--)
	{
		if (dest[start] == '/')
		{
			start++;
			break;
		}
	}
	if (start < 0) start = 0;
	if (start < end)
	{
		gtk_editable_select_region(GTK_EDITABLE(entry), start, end);
	}
}

gboolean
destination_grab_cb(
	GtkEntry *entry, 
	signal_user_data_t *ud)
{
	destination_select_title(entry);
	return FALSE;
}

static gboolean update_default_destination = FALSE;

void
dest_dir_set_cb(GtkFileChooserButton *dest_chooser, signal_user_data_t *ud)
{
	gchar *dest_file, *dest_dir, *dest;
	
	g_debug("dest_dir_set_cb ()");
	ghb_widget_to_setting(ud->settings, (GtkWidget*)dest_chooser);
	dest_file = ghb_settings_get_string(ud->settings, "dest_file");
	dest_dir = ghb_settings_get_string(ud->settings, "dest_dir");
	dest = g_strdup_printf("%s/%s", dest_dir, dest_file);
	ghb_settings_set_string(ud->settings, "destination", dest);
	g_free(dest_file);
	g_free(dest_dir);
	g_free(dest);
	update_default_destination = TRUE;
}

void
dest_file_changed_cb(GtkEntry *entry, signal_user_data_t *ud)
{
	gchar *dest_file, *dest_dir, *dest;
	
	g_debug("dest_file_changed_cb ()");
	update_destination_extension(ud);
	ghb_widget_to_setting(ud->settings, (GtkWidget*)entry);
	// This signal goes off with ever keystroke, so I'm putting this
	// update on the timer.
	dest_file = ghb_settings_get_string(ud->settings, "dest_file");
	dest_dir = ghb_settings_get_string(ud->settings, "dest_dir");
	dest = g_strdup_printf("%s/%s", dest_dir, dest_file);
	ghb_settings_set_string(ud->settings, "destination", dest);
	g_free(dest_file);
	g_free(dest_dir);
	g_free(dest);
	update_default_destination = TRUE;
}

void
destination_browse_clicked_cb(GtkButton *button, signal_user_data_t *ud)
{
	GtkWidget *dialog;
	GtkEntry *entry;
	gchar *destname;
	gchar *basename;

	g_debug("destination_browse_clicked_cb ()");
	destname = ghb_settings_get_string(ud->settings, "destination");
	dialog = gtk_file_chooser_dialog_new ("Choose Destination",
					  NULL,
					  GTK_FILE_CHOOSER_ACTION_SAVE,
					  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					  GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
					  NULL);
	gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), destname);
	basename = g_path_get_basename(destname);
	g_free(destname);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), basename);
	g_free(basename);
	if (gtk_dialog_run(GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
	{
		char *filename, *dirname;
		GtkFileChooser *dest_chooser;
		
		filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
		basename = g_path_get_basename(filename);
		dirname = g_path_get_dirname(filename);
		entry = (GtkEntry*)GHB_WIDGET(ud->builder, "dest_file");
		gtk_entry_set_text(entry, basename);
		dest_chooser = GTK_FILE_CHOOSER(GHB_WIDGET(ud->builder, "dest_dir"));
		gtk_file_chooser_set_filename(dest_chooser, dirname);
		g_free (dirname);
		g_free (basename);
		g_free (filename);
	}
	gtk_widget_destroy(dialog);
}

gboolean
window_destroy_event_cb(GtkWidget *widget, GdkEvent *event, signal_user_data_t *ud)
{
	g_debug("window_destroy_event_cb ()");
	ghb_hb_cleanup(FALSE);
	prune_logs(ud);
	gtk_main_quit();
	return FALSE;
}

gboolean
window_delete_event_cb(GtkWidget *widget, GdkEvent *event, signal_user_data_t *ud)
{
	gint state = ghb_get_queue_state();
	g_debug("window_delete_event_cb ()");
	if (state & GHB_STATE_WORKING)
	{
		if (ghb_cancel_encode("Closing HandBrake will terminate encoding.\n"))
		{
			ghb_hb_cleanup(FALSE);
			prune_logs(ud);
			gtk_main_quit();
			return FALSE;
		}
		return TRUE;
	}
	ghb_hb_cleanup(FALSE);
	prune_logs(ud);
	gtk_main_quit();
	return FALSE;
}

static void
update_acodec_combo(signal_user_data_t *ud)
{
	ghb_grey_combo_options (ud->builder);
}

void
container_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	const GValue *audio_list;
	g_debug("container_changed_cb ()");
	ghb_widget_to_setting(ud->settings, widget);
	update_destination_extension(ud);
	ghb_check_dependency(ud, widget);
	update_acodec_combo(ud);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);

	audio_list = ghb_settings_get_value(ud->settings, "audio_list");
	if (ghb_ac3_in_audio_list (audio_list))
	{
		gchar *container;

		container = ghb_settings_get_string(ud->settings, "FileFormat");
		if (strcmp(container, "mp4") == 0)
		{
			ghb_ui_update(ud, "FileFormat", ghb_string_value("m4v"));
		}
		g_free(container);
	}
}

static gchar*
get_aspect_string(gint aspect_n, gint aspect_d)
{
	gchar *aspect;

	if (aspect_d < 10)
	{
		aspect = g_strdup_printf("%d:%d", aspect_n, aspect_d);
	}
	else
	{
		gdouble aspect_nf = (gdouble)aspect_n / aspect_d;
		aspect = g_strdup_printf("%.2f:1", aspect_nf);
	}
	return aspect;
}

static gchar*
get_rate_string(gint rate_base, gint rate)
{
	gdouble rate_f = (gdouble)rate / rate_base;
	gchar *rate_s;

	rate_s = g_strdup_printf("%.6g", rate_f);
	return rate_s;
}
static void
show_title_info(signal_user_data_t *ud, ghb_title_info_t *tinfo)
{
	GtkWidget *widget;
	gchar *text;

	widget = GHB_WIDGET (ud->builder, "title_duration");
	if (tinfo->duration != 0)
	{
		text = g_strdup_printf ("%02d:%02d:%02d", tinfo->hours, 
				tinfo->minutes, tinfo->seconds);
	}
	else
	{
		text = g_strdup_printf ("Unknown");
	}
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);
	widget = GHB_WIDGET (ud->builder, "source_dimensions");
	text = g_strdup_printf ("%d x %d", tinfo->width, tinfo->height);
	gtk_label_set_text (GTK_LABEL(widget), text);
	ghb_settings_set_int(ud->settings, "source_width", tinfo->width);
	ghb_settings_set_int(ud->settings, "source_height", tinfo->height);
	g_free(text);
	widget = GHB_WIDGET (ud->builder, "source_aspect");
	text = get_aspect_string(tinfo->aspect_n, tinfo->aspect_d);
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);

	widget = GHB_WIDGET (ud->builder, "source_frame_rate");
	text = (gchar*)get_rate_string(tinfo->rate_base, tinfo->rate);
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);

	ghb_ui_update(ud, "scale_width", 
		ghb_int64_value(tinfo->width - tinfo->crop[2] - tinfo->crop[3]));
	// If anamorphic or keep_aspect, the hight will be automatically calculated
	gboolean keep_aspect, anamorphic;
	keep_aspect = ghb_settings_get_boolean(ud->settings, "PictureKeepRatio");
	anamorphic = ghb_settings_get_boolean(ud->settings, "anamorphic");
	if (!(keep_aspect || anamorphic))
	{
		ghb_ui_update(ud, "scale_height", 
			ghb_int64_value(tinfo->height - tinfo->crop[0] - tinfo->crop[1]));
	}

	// Set the limits of cropping.  hb_set_anamorphic_size crashes if
	// you pass it a cropped width or height == 0.
	gint bound;
	bound = tinfo->height / 2 - 2;
	widget = GHB_WIDGET (ud->builder, "PictureTopCrop");
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 0, bound);
	widget = GHB_WIDGET (ud->builder, "PictureBottomCrop");
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 0, bound);
	bound = tinfo->width / 2 - 2;
	widget = GHB_WIDGET (ud->builder, "PictureLeftCrop");
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 0, bound);
	widget = GHB_WIDGET (ud->builder, "PictureRightCrop");
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 0, bound);
	if (ghb_settings_get_boolean(ud->settings, "PictureAutoCrop"))
	{
		ghb_ui_update(ud, "PictureTopCrop", ghb_int64_value(tinfo->crop[0]));
		ghb_ui_update(ud, "PictureBottomCrop", ghb_int64_value(tinfo->crop[1]));
		ghb_ui_update(ud, "PictureLeftCrop", ghb_int64_value(tinfo->crop[2]));
		ghb_ui_update(ud, "PictureRightCrop", ghb_int64_value(tinfo->crop[3]));
	}
	ghb_set_scale (ud, GHB_SCALE_KEEP_NONE);
	gint width, height, crop[4];
	crop[0] = ghb_settings_get_int(ud->settings, "PictureTopCrop");
	crop[1] = ghb_settings_get_int(ud->settings, "PictureBottomCrop");
	crop[2] = ghb_settings_get_int(ud->settings, "PictureLeftCrop");
	crop[3] = ghb_settings_get_int(ud->settings, "PictureRightCrop");
	width = tinfo->width - crop[2] - crop[3];
	height = tinfo->height - crop[0] - crop[1];
	widget = GHB_WIDGET (ud->builder, "crop_dimensions");
	text = g_strdup_printf ("%d x %d", width, height);
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);

	g_debug("setting max end chapter %d", tinfo->num_chapters);
	widget = GHB_WIDGET (ud->builder, "end_chapter");
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 1, tinfo->num_chapters);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON(widget), tinfo->num_chapters);
	widget = GHB_WIDGET (ud->builder, "start_chapter");
	gtk_spin_button_set_value (GTK_SPIN_BUTTON(widget), 1);
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 1, tinfo->num_chapters);
}

static gboolean update_preview = FALSE;

void
title_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	ghb_title_info_t tinfo;
	gint titleindex;
	
	g_debug("title_changed_cb ()");
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);

	titleindex = ghb_settings_combo_int(ud->settings, "title");
	ghb_update_ui_combo_box (ud->builder, "AudioTrack", titleindex, FALSE);
	ghb_update_ui_combo_box (ud->builder, "Subtitles", titleindex, FALSE);

	ghb_update_from_preset(ud, "Subtitles");
	if (ghb_get_title_info (&tinfo, titleindex))
	{
		show_title_info(ud, &tinfo);
	}
	update_chapter_list (ud);
	ghb_adjust_audio_rate_combos(ud);
	ghb_set_pref_audio(titleindex, ud);
	if (ghb_settings_get_boolean(ud->settings, "vquality_type_target"))
	{
		gint bitrate = ghb_calculate_target_bitrate (ud->settings, titleindex);
		ghb_ui_update(ud, "VideoAvgBitrate", ghb_int64_value(bitrate));
	}

	// Unfortunately, there is no way to query how many frames were
	// actually generated during the scan.
	// If I knew how many were generated, I would adjust the spin
	// control range here.
	// I do know how many were asked for.
	gint preview_count;
	preview_count = ghb_settings_get_int(ud->settings, "preview_count");
	widget = GHB_WIDGET(ud->builder, "preview_frame");
	gtk_spin_button_set_range (GTK_SPIN_BUTTON(widget), 1, preview_count);
	ghb_ui_update(ud, "preview_frame", ghb_int64_value(2));

	ghb_set_preview_image (ud);
	if (ghb_settings_get_boolean(ud->settings, "title_no_in_destination"))
	{
		set_destination(ud);
	}
}

void
setting_widget_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);
}

void
vquality_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);

	gint vcodec = ghb_settings_combo_int(ud->settings, "VideoEncoder");
	gdouble step;
	if (vcodec == HB_VCODEC_X264)
	{
		step = ghb_settings_combo_double(ud->settings, 
											"VideoQualityGranularity");
	}
	else
	{
		step = 1;
	}
	gdouble val = gtk_range_get_value(GTK_RANGE(widget));
	val = ((int)((val + step / 2) / step)) * step;
	gtk_range_set_value(GTK_RANGE(widget), val);
}

void
http_opt_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);
	// AC3 is not allowed when Web optimized
	ghb_grey_combo_options (ud->builder);
}

void
vcodec_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	gdouble vqmin, vqmax, step, page;
	gboolean inverted;
	gint digits;

	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);
	ghb_vquality_range(ud, &vqmin, &vqmax, &step, &page, &digits, &inverted);
	GtkWidget *qp = GHB_WIDGET(ud->builder, "VideoQualitySlider");
	gtk_range_set_range (GTK_RANGE(qp), vqmin, vqmax);
	gtk_range_set_increments (GTK_RANGE(qp), step, page);
	gtk_scale_set_digits(GTK_SCALE(qp), digits);
	gtk_range_set_inverted (GTK_RANGE(qp), inverted);
}

void
target_size_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	const gchar *name = gtk_widget_get_name(widget);
	g_debug("target_size_changed_cb () %s", name);
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);
	if (ghb_settings_get_boolean(ud->settings, "vquality_type_target"))
	{
		gint titleindex;
		titleindex = ghb_settings_combo_int(ud->settings, "title");
		gint bitrate = ghb_calculate_target_bitrate (ud->settings, titleindex);
		ghb_ui_update(ud, "VideoAvgBitrate", ghb_int64_value(bitrate));
	}
}

void
start_chapter_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	gint start, end;
	const gchar *name = gtk_widget_get_name(widget);

	g_debug("start_chapter_changed_cb () %s", name);
	ghb_widget_to_setting(ud->settings, widget);
	start = ghb_settings_get_int(ud->settings, "start_chapter");
	end = ghb_settings_get_int(ud->settings, "end_chapter");
	if (start > end)
		ghb_ui_update(ud, "end_chapter", ghb_int_value(start));
	ghb_check_dependency(ud, widget);
	if (ghb_settings_get_boolean(ud->settings, "chapters_in_destination"))
	{
		set_destination(ud);
	}
}

void
end_chapter_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	gint start, end;
	const gchar *name = gtk_widget_get_name(widget);

	g_debug("end_chapter_changed_cb () %s", name);
	ghb_widget_to_setting(ud->settings, widget);
	start = ghb_settings_get_int(ud->settings, "start_chapter");
	end = ghb_settings_get_int(ud->settings, "end_chapter");
	if (start > end)
		ghb_ui_update(ud, "start_chapter", ghb_int_value(end));
	ghb_check_dependency(ud, widget);
	if (ghb_settings_get_boolean(ud->settings, "chapters_in_destination"))
	{
		set_destination(ud);
	}
}

void
scale_width_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("scale_width_changed_cb ()");
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_set_scale (ud, GHB_SCALE_KEEP_WIDTH);
	update_preview = TRUE;
	gchar *text;
	gint width = ghb_settings_get_int(ud->settings, "scale_width");
	gint height = ghb_settings_get_int(ud->settings, "scale_height");
	widget = GHB_WIDGET (ud->builder, "scale_dimensions");
	text = g_strdup_printf ("%d x %d", width, height);
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);
	ghb_live_reset(ud);
}

void
scale_height_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("scale_height_changed_cb ()");
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_set_scale (ud, GHB_SCALE_KEEP_HEIGHT);
	update_preview = TRUE;
	gchar *text;
	gint width = ghb_settings_get_int(ud->settings, "scale_width");
	gint height = ghb_settings_get_int(ud->settings, "scale_height");
	widget = GHB_WIDGET (ud->builder, "scale_dimensions");
	text = g_strdup_printf ("%d x %d", width, height);
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);
	ghb_live_reset(ud);
}

void
crop_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	gint titleindex, crop[4];
	ghb_title_info_t tinfo;
	
	g_debug("crop_changed_cb ()");
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_set_scale (ud, GHB_SCALE_KEEP_NONE);

	crop[0] = ghb_settings_get_int(ud->settings, "PictureTopCrop");
	crop[1] = ghb_settings_get_int(ud->settings, "PictureBottomCrop");
	crop[2] = ghb_settings_get_int(ud->settings, "PictureLeftCrop");
	crop[3] = ghb_settings_get_int(ud->settings, "PictureRightCrop");
	titleindex = ghb_settings_combo_int(ud->settings, "title");
	if (ghb_get_title_info (&tinfo, titleindex))
	{
		gint width, height;
		gchar *text;
		
		width = tinfo.width - crop[2] - crop[3];
		height = tinfo.height - crop[0] - crop[1];
		widget = GHB_WIDGET (ud->builder, "crop_dimensions");
		text = g_strdup_printf ("%d x %d", width, height);
		gtk_label_set_text (GTK_LABEL(widget), text);
		g_free(text);
	}
	gchar *text;
	widget = GHB_WIDGET (ud->builder, "crop_values");
	text = g_strdup_printf ("%d:%d:%d:%d", crop[0], crop[1], crop[2], crop[3]);
	gtk_label_set_text (GTK_LABEL(widget), text);
	g_free(text);
	update_preview = TRUE;
	ghb_live_reset(ud);
}

void
scale_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("scale_changed_cb ()");
	ghb_widget_to_setting(ud->settings, widget);
	ghb_check_dependency(ud, widget);
	ghb_clear_presets_selection(ud);
	ghb_live_reset(ud);
	ghb_set_scale (ud, GHB_SCALE_KEEP_NONE);
	update_preview = TRUE;
	
	gchar *text;
	
	text = ghb_settings_get_boolean(ud->settings, "PictureAutoCrop") ? "On" : "Off";
	widget = GHB_WIDGET (ud->builder, "crop_auto");
	gtk_label_set_text (GTK_LABEL(widget), text);
	text = ghb_settings_get_boolean(ud->settings, "autoscale") ? "On" : "Off";
	widget = GHB_WIDGET (ud->builder, "scale_auto");
	gtk_label_set_text (GTK_LABEL(widget), text);
	text = ghb_settings_get_boolean(ud->settings, "anamorphic") ? "On" : "Off";
	widget = GHB_WIDGET (ud->builder, "scale_anamorphic");
	gtk_label_set_text (GTK_LABEL(widget), text);
}

void
generic_entry_changed_cb(GtkEntry *entry, signal_user_data_t *ud)
{
	// Normally (due to user input) I only want to process the entry
	// when editing is done and the focus-out signal is sent.
	// But... there's always a but.
	// If the entry is changed by software, the focus-out signal is not sent.
	// The changed signal is sent ... so here we are.
	// I don't want to process upon every keystroke, so I prevent processing
	// while the widget has focus.
	g_debug("generic_entry_changed_cb ()");
	if (!GTK_WIDGET_HAS_FOCUS((GtkWidget*)entry))
	{
		ghb_widget_to_setting(ud->settings, (GtkWidget*)entry);
	}
}

void
prefs_dialog_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	GtkWidget *dialog;
	GtkResponseType response;

	g_debug("prefs_dialog_cb ()");
	dialog = GHB_WIDGET(ud->builder, "prefs_dialog");
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_hide(dialog);
}

gboolean
ghb_message_dialog(GtkMessageType type, const gchar *message, const gchar *no, const gchar *yes)
{
	GtkWidget *dialog;
	GtkResponseType response;
			
	// Toss up a warning dialog
	dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
							type, GTK_BUTTONS_NONE,
							"%s", message);
	gtk_dialog_add_buttons( GTK_DIALOG(dialog), 
						   no, GTK_RESPONSE_NO,
						   yes, GTK_RESPONSE_YES, NULL);
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy (dialog);
	if (response == GTK_RESPONSE_NO)
	{
		return FALSE;
	}
	return TRUE;
}

gboolean
ghb_cancel_encode(const gchar *extra_msg)
{
	GtkWidget *dialog;
	GtkResponseType response;
	
	if (extra_msg == NULL) extra_msg = "";
	// Toss up a warning dialog
	dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
				"%sYour movie will be lost if you don't continue encoding.",
				extra_msg);
	gtk_dialog_add_buttons( GTK_DIALOG(dialog), 
						   "Continue Encoding", GTK_RESPONSE_NO,
						   "Stop Encoding", GTK_RESPONSE_YES, NULL);
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy (dialog);
	if (response == GTK_RESPONSE_NO) return FALSE;
	ghb_stop_queue();
	return TRUE;
}

static void
submit_job(GValue *settings)
{
	static gint unique_id = 1;
	gchar *type, *modified, *preset;
	const GValue *path;
	gboolean preset_modified;

	g_debug("submit_job");
	if (settings == NULL) return;
	preset_modified = ghb_settings_get_boolean(settings, "preset_modified");
	path = ghb_settings_get_value(settings, "preset");
	preset = ghb_preset_path_string(path);
	type = ghb_preset_is_custom() ? "Custom " : "";
	modified = preset_modified ? "Modified " : "";
	ghb_log("%s%sPreset: %s", modified, type, preset);
	g_free(preset);

	ghb_settings_set_int(settings, "job_unique_id", unique_id);
	ghb_settings_set_int(settings, "job_status", GHB_QUEUE_RUNNING);
	ghb_add_job (settings, unique_id);
	ghb_start_queue();
	unique_id++;
}

static void
prune_logs(signal_user_data_t *ud)
{
	gchar *dest_dir;

	// Only prune logs stored in the default config dir location
	dest_dir = ghb_get_user_config_dir("EncodeLogs");
	if (g_file_test(dest_dir, G_FILE_TEST_IS_DIR))
	{
		const gchar *file;
		int week = 7*24*60*60;
		GDir *gdir = g_dir_open(dest_dir, 0, NULL);
		time_t now;

		now = time(NULL);
		file = g_dir_read_name(gdir);
		while (file)
		{
			gchar *path;
			struct stat stbuf;

			path = g_strdup_printf("%s/%s", dest_dir, file);
			g_stat(path, &stbuf);
			if (now - stbuf.st_mtime > week)
			{
				g_unlink(path);
			}
			g_free(path);
			file = g_dir_read_name(gdir);
		}
		g_dir_close(gdir);
	}
	g_free(dest_dir);
	ghb_preview_cleanup(ud);
}

static void
queue_scan(signal_user_data_t *ud, GValue *js)
{
	gchar *path;
	gint titlenum;
	time_t	_now;
	struct tm *now;
	gchar *log_path, *pos, *destname, *basename, *dest_dir;

	_now = time(NULL);
	now = localtime(&_now);
	destname = ghb_settings_get_string(js, "destination");
	basename = g_path_get_basename(destname);
	if (ghb_settings_get_boolean(ud->settings, "EncodeLogLocation"))
	{
		dest_dir = g_path_get_dirname (destname);
	}
	else
	{
		dest_dir = ghb_get_user_config_dir("EncodeLogs");
	}
	g_free(destname);
	pos = g_strrstr( basename, "." );
	if (pos != NULL)
	{
		*pos = 0;
	}
	log_path = g_strdup_printf("%s/%d-%02d-%02d %02d-%02d-%02d %s.log",
		dest_dir,
		now->tm_year + 1900, now->tm_mon + 1, now->tm_mday,
		now->tm_hour, now->tm_min, now->tm_sec, basename);
	g_free(basename);
	g_free(dest_dir);
	if (ud->job_activity_log)
		g_io_channel_unref(ud->job_activity_log);
	ud->job_activity_log = g_io_channel_new_file (log_path, "w", NULL);
	if (ud->job_activity_log)
	{
		gchar *ver_str;

		ver_str = g_strdup_printf("Handbrake Version: %s (%d)\n", 
									hb_get_version(NULL), hb_get_build(NULL));
		g_io_channel_write_chars (ud->job_activity_log, ver_str, 
									-1, NULL, NULL);
		g_free(ver_str);
	}
	g_free(log_path);

	path = ghb_settings_get_string( js, "source");
	titlenum = ghb_settings_get_int(js, "titlenum");
	ghb_backend_queue_scan(path, titlenum);
	g_free(path);
}

GValue* 
ghb_start_next_job(signal_user_data_t *ud, gboolean find_first)
{
	static gint current = 0;
	gint count, ii, jj;
	GValue *js;
	gint status;

	g_debug("start_next_job");
	count = ghb_array_len(ud->queue);
	if (find_first)
	{	// Start the first pending item in the queue
		current = 0;
		for (ii = 0; ii < count; ii++)
		{

			js = ghb_array_get_nth(ud->queue, ii);
			status = ghb_settings_get_int(js, "job_status");
			if (status == GHB_QUEUE_PENDING)
			{
				current = ii;
				ghb_inhibit_gpm();
				queue_scan(ud, js);
				return js;
			}
		}
		// Nothing pending
		ghb_uninhibit_gpm();
		return NULL;
	}
	// Find the next pending item after the current running item
	for (ii = 0; ii < count-1; ii++)
	{
		js = ghb_array_get_nth(ud->queue, ii);
		status = ghb_settings_get_int(js, "job_status");
		if (status == GHB_QUEUE_RUNNING)
		{
			for (jj = ii+1; jj < count; jj++)
			{
				js = ghb_array_get_nth(ud->queue, jj);
				status = ghb_settings_get_int(js, "job_status");
				if (status == GHB_QUEUE_PENDING)
				{
					current = jj;
					ghb_inhibit_gpm();
					queue_scan(ud, js);
					return js;
				}
			}
		}
	}
	// No running item found? Maybe it was deleted
	// Look for a pending item starting from the last index we started
	for (ii = current; ii < count; ii++)
	{
		js = ghb_array_get_nth(ud->queue, ii);
		status = ghb_settings_get_int(js, "job_status");
		if (status == GHB_QUEUE_PENDING)
		{
			current = ii;
			ghb_inhibit_gpm();
			queue_scan(ud, js);
			return js;
		}
	}
	// Nothing found
	ghb_uninhibit_gpm();
	return NULL;
}

static gint
find_queue_job(GValue *queue, gint unique_id, GValue **job)
{
	GValue *js;
	gint ii, count;
	gint job_unique_id;
	
	*job = NULL;
	g_debug("find_queue_job");
	count = ghb_array_len(queue);
	for (ii = 0; ii < count; ii++)
	{
		js = ghb_array_get_nth(queue, ii);
		job_unique_id = ghb_settings_get_int(js, "job_unique_id");
		if (job_unique_id == unique_id)
		{
			*job = js;
			return ii;
		}
	}
	return -1;
}

gchar*
working_status_string(signal_user_data_t *ud, ghb_instance_status_t *status)
{
	gchar *task_str, *job_str, *status_str;
	gint qcount;
	gint index;
	GValue *js;

	if (status->job_count > 1)
	{
		task_str = g_strdup_printf("pass %d of %d, ", 
			status->job_cur, status->job_count);
	}
	else
	{
		task_str = g_strdup("");
	}
	qcount = ghb_array_len(ud->queue);
	if (qcount > 1)
	{
		index = find_queue_job(ud->queue, status->unique_id, &js);
		job_str = g_strdup_printf("job %d of %d, ", index+1, qcount);
	}
	else
	{
		job_str = g_strdup("");
	}
	if(status->seconds > -1)
	{
		status_str= g_strdup_printf(
			"Encoding: %s%s%.2f %%"
			" (%.2f fps, avg %.2f fps, ETA %02dh%02dm%02ds)",
			job_str, task_str,
			100.0 * status->progress,
			status->rate_cur, status->rate_avg, status->hours, 
			status->minutes, status->seconds );
	}
	else
	{
		status_str= g_strdup_printf(
			"Encoding: %s%s%.2f %%",
			job_str, task_str,
			100.0 * status->progress );
	}
	g_free(task_str);
	g_free(job_str);
	return status_str;
}

static void
ghb_backend_events(signal_user_data_t *ud)
{
	ghb_status_t status;
	gchar *status_str;
	GtkProgressBar *progress;
	gint titleindex;
	GValue *js;
	gint index;
	GtkTreeView *treeview;
	GtkTreeStore *store;
	GtkTreeIter iter;
	static gint working = 0;
	static gboolean work_started = FALSE;
	
	ghb_track_status();
	ghb_get_status(&status);
	progress = GTK_PROGRESS_BAR(GHB_WIDGET (ud->builder, "progressbar"));
	// First handle the status of title scans
	// Then handle the status of the queue
	if (status.scan.state & GHB_STATE_SCANNING)
	{
		if (status.scan.title_cur == 0)
		{
			status_str = g_strdup ("Scanning...");
		}
		else
		{
			status_str = g_strdup_printf ("Scanning title %d of %d...", 
							  status.scan.title_cur, status.scan.title_count );
		}
		gtk_progress_bar_set_text (progress, status_str);
		g_free(status_str);
		if (status.scan.title_count > 0)
		{
			gtk_progress_bar_set_fraction (progress, 
				(gdouble)status.scan.title_cur / status.scan.title_count);
		}
	}
	else if (status.scan.state & GHB_STATE_SCANDONE)
	{
		status_str = g_strdup_printf ("Scan done"); 
		gtk_progress_bar_set_text (progress, status_str);
		g_free(status_str);
		gtk_progress_bar_set_fraction (progress, 1.0);

		ghb_title_info_t tinfo;
			
		ghb_update_ui_combo_box(ud->builder, "title", 0, FALSE);
		titleindex = ghb_longest_title();
		ghb_ui_update(ud, "title", ghb_int64_value(titleindex));

		// Are there really any titles.
		if (!ghb_get_title_info(&tinfo, titleindex))
		{
			GtkProgressBar *progress;
			progress = GTK_PROGRESS_BAR(GHB_WIDGET (ud->builder, "progressbar"));
			gtk_progress_bar_set_fraction (progress, 0);
			gtk_progress_bar_set_text (progress, "No Source");
		}
		ghb_clear_scan_state(GHB_STATE_SCANDONE);
		ghb_queue_buttons_grey(ud, work_started);
		if (ghb_queue_edit_settings)
		{
			gint jstatus;

			jstatus = ghb_settings_get_int(ghb_queue_edit_settings, "job_status");
			ghb_settings_to_ui(ud, ghb_queue_edit_settings);
			ghb_set_audio(ud, ghb_queue_edit_settings);
			if (jstatus == GHB_QUEUE_PENDING)
			{
				ghb_value_free(ghb_queue_edit_settings);
			}
			ghb_queue_edit_settings = NULL;
		}
	}
	else if (status.queue.state & GHB_STATE_SCANNING)
	{
		status_str = g_strdup_printf ("Scanning ...");
		gtk_progress_bar_set_text (progress, status_str);
		g_free(status_str);
		gtk_progress_bar_set_fraction (progress, 0);
	}
	else if (status.queue.state & GHB_STATE_SCANDONE)
	{
		ghb_clear_queue_state(GHB_STATE_SCANDONE);
		usleep(2000000);
		submit_job(ud->current_job);
	}
	else if (status.queue.state & GHB_STATE_PAUSED)
	{
		status_str = g_strdup_printf ("Paused"); 
		gtk_progress_bar_set_text (progress, status_str);
		g_free(status_str);
	}
	else if (status.queue.state & GHB_STATE_WORKING)
	{
		status_str = working_status_string(ud, &status.queue);
		gtk_progress_bar_set_text (progress, status_str);
		gtk_progress_bar_set_fraction (progress, status.queue.progress);
		g_free(status_str);
	}
	else if (status.queue.state & GHB_STATE_WORKDONE)
	{
		gint qstatus;

		work_started = FALSE;
		ghb_queue_buttons_grey(ud, FALSE);
		index = find_queue_job(ud->queue, status.queue.unique_id, &js);
		treeview = GTK_TREE_VIEW(GHB_WIDGET(ud->builder, "queue_list"));
		store = GTK_TREE_STORE(gtk_tree_view_get_model(treeview));
		if (ud->cancel_encode)
			status.queue.error = GHB_ERROR_CANCELED;
		switch( status.queue.error )
		{
			case GHB_ERROR_NONE:
				gtk_progress_bar_set_text( progress, "Rip done!" );
				qstatus = GHB_QUEUE_DONE;
				if (js != NULL)
				{
					gchar *path = g_strdup_printf ("%d", index);
					if (gtk_tree_model_get_iter_from_string(
							GTK_TREE_MODEL(store), &iter, path))
					{
						gtk_tree_store_set(store, &iter, 0, "hb-complete", -1);
					}
					g_free(path);
				}
				break;
			case GHB_ERROR_CANCELED:
				gtk_progress_bar_set_text( progress, "Rip canceled." );
				qstatus = GHB_QUEUE_CANCELED;
				if (js != NULL)
				{
					gchar *path = g_strdup_printf ("%d", index);
					if (gtk_tree_model_get_iter_from_string(
							GTK_TREE_MODEL(store), &iter, path))
					{
						gtk_tree_store_set(store, &iter, 0, "hb-canceled", -1);
					}
					g_free(path);
				}
				break;
			default:
				gtk_progress_bar_set_text( progress, "Rip failed.");
				qstatus = GHB_QUEUE_CANCELED;
				if (js != NULL)
				{
					gchar *path = g_strdup_printf ("%d", index);
					if (gtk_tree_model_get_iter_from_string(
							GTK_TREE_MODEL(store), &iter, path))
					{
						gtk_tree_store_set(store, &iter, 0, "hb-canceled", -1);
					}
					g_free(path);
				}
		}
		gtk_progress_bar_set_fraction (progress, 1.0);
		ghb_clear_queue_state(GHB_STATE_WORKDONE);
		if (ud->job_activity_log)
			g_io_channel_unref(ud->job_activity_log);
		ud->job_activity_log = NULL;
		if (!ud->cancel_encode)
		{
			ud->current_job = ghb_start_next_job(ud, FALSE);
		}
		else
		{
			ghb_uninhibit_gpm();
			ud->current_job = NULL;
		}
		if (js)
			ghb_settings_set_int(js, "job_status", qstatus);
		ghb_save_queue(ud->queue);
		ud->cancel_encode = FALSE;
	}
	else if (status.queue.state & GHB_STATE_MUXING)
	{
		gtk_progress_bar_set_text(progress, "Muxing: this may take awhile...");
	}
	if (status.queue.state & GHB_STATE_SCANNING)
	{
		// This needs to be in scanning and working since scanning
		// happens fast enough that it can be missed
		if (!work_started)
		{
			work_started = TRUE;
			ghb_queue_buttons_grey(ud, TRUE);
		}
	}
	if (status.queue.state & GHB_STATE_WORKING)
	{
		// This needs to be in scanning and working since scanning
		// happens fast enough that it can be missed
		if (!work_started)
		{
			work_started = TRUE;
			ghb_queue_buttons_grey(ud, TRUE);
		}
		index = find_queue_job(ud->queue, status.queue.unique_id, &js);
		if (status.queue.unique_id != 0 && index >= 0)
		{
			gchar working_icon[] = "hb-working0";
			working_icon[10] = '0' + working;
			working = (working+1) % 6;
			treeview = GTK_TREE_VIEW(GHB_WIDGET(ud->builder, "queue_list"));
			store = GTK_TREE_STORE(gtk_tree_view_get_model(treeview));
			gchar *path = g_strdup_printf ("%d", index);
			if (gtk_tree_model_get_iter_from_string(
					GTK_TREE_MODEL(store), &iter, path))
			{
				gtk_tree_store_set(store, &iter, 0, working_icon, -1);
			}
			g_free(path);
		}
		GtkLabel *label;
		gchar *status_str;

		status_str = working_status_string(ud, &status.queue);
		label = GTK_LABEL(GHB_WIDGET(ud->builder, "queue_status"));
		gtk_label_set_text (label, status_str);
		g_free(status_str);
	}
	if (status.scan.state & GHB_STATE_WORKING)
	{
		GtkProgressBar *live_progress;
		live_progress = GTK_PROGRESS_BAR(
			GHB_WIDGET(ud->builder, "live_encode_progress"));
		status_str = working_status_string(ud, &status.scan);
		gtk_progress_bar_set_text (live_progress, status_str);
		gtk_progress_bar_set_fraction (live_progress, status.scan.progress);
		g_free(status_str);
	}
	if (status.scan.state & GHB_STATE_WORKDONE)
	{
		switch( status.scan.error )
		{
			case GHB_ERROR_NONE:
			{
				ghb_live_encode_done(ud, TRUE);
			} break;
			default:
			{
				ghb_live_encode_done(ud, FALSE);
			} break;
		}
		ghb_clear_scan_state(GHB_STATE_WORKDONE);
	}
}

gboolean
ghb_timer_cb(gpointer data)
{
	signal_user_data_t *ud = (signal_user_data_t*)data;

	ghb_live_preview_progress(ud);
	ghb_backend_events(ud);
	if (update_default_destination)
	{
		gchar *dest, *dest_dir, *def_dest;
		dest = ghb_settings_get_string(ud->settings, "destination");
		dest_dir = g_path_get_dirname (dest);
		def_dest = ghb_settings_get_string(ud->settings, "destination_dir");
		if (strcmp(dest_dir, def_dest) != 0)
		{
			ghb_settings_set_string (ud->settings, "destination_dir", dest_dir);
			ghb_pref_save (ud->settings, "destination_dir");
		}
		g_free(dest);
		g_free(dest_dir);
		g_free(def_dest);
		update_default_destination = FALSE;
	}
	if (update_preview)
	{
		ghb_set_preview_image (ud);
		update_preview = FALSE;
	}
	return TRUE;
}

gboolean
ghb_log_cb(GIOChannel *source, GIOCondition cond, gpointer data)
{
	gchar *text = NULL;
	gsize length;
	GtkTextView *textview;
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	GtkTextMark *mark;
	GError *gerror = NULL;
	GIOStatus status;
	
	signal_user_data_t *ud = (signal_user_data_t*)data;

	status = g_io_channel_read_line (source, &text, &length, NULL, &gerror);
	if (text != NULL)
	{
		GdkWindow *window;
		gint width, height;
		gint x, y;
		gboolean bottom = FALSE;

		textview = GTK_TEXT_VIEW(GHB_WIDGET (ud->builder, "activity_view"));
		buffer = gtk_text_view_get_buffer (textview);
		// I would like to auto-scroll the window when the scrollbar
		// is at the bottom, 
		// must determine whether the insert point is at
		// the bottom of the window 
		window = gtk_text_view_get_window(textview, GTK_TEXT_WINDOW_TEXT);
		if (window != NULL)
		{
			gdk_drawable_get_size(GDK_DRAWABLE(window), &width, &height);
			gtk_text_view_window_to_buffer_coords(textview, 
				GTK_TEXT_WINDOW_TEXT, width, height, &x, &y);
			gtk_text_view_get_iter_at_location(textview, &iter, x, y);
			if (gtk_text_iter_is_end(&iter))
			{
				bottom = TRUE;
			}
		}
		else
		{
			// If the window isn't available, assume bottom
			bottom = TRUE;
		}
		gtk_text_buffer_get_end_iter(buffer, &iter);
		gtk_text_buffer_insert(buffer, &iter, text, -1);
		if (bottom)
		{
			gtk_text_buffer_get_end_iter(buffer, &iter);
			mark = gtk_text_buffer_create_mark(buffer, NULL, &iter, FALSE);
			gtk_text_view_scroll_mark_onscreen(textview, mark);
			gtk_text_buffer_delete_mark(buffer, mark);
		}
		g_io_channel_write_chars (ud->activity_log, text, 
								length, &length, NULL);
		g_io_channel_flush(ud->activity_log, NULL);
		if (ud->job_activity_log)
		{
			g_io_channel_write_chars (ud->job_activity_log, text, 
									length, &length, NULL);
			g_io_channel_flush(ud->job_activity_log, NULL);
		}
		g_free(text);
	}
	if (status != G_IO_STATUS_NORMAL)
	{
		// This should never happen, but if it does I would get into an
		// infinite loop.  Returning false removes this callback.
		g_warning("Error while reading activity from pipe");
		if (gerror != NULL)
		{
			g_warning("%s", gerror->message);
			g_error_free (gerror);
		}
		return FALSE;
	}
	if (gerror != NULL)
		g_error_free (gerror);
	return TRUE;
}

static void
set_visible(GtkWidget *widget, gboolean visible)
{
	if (visible)
	{
		gtk_widget_show_now(widget);
	}
	else
	{
		gtk_widget_hide(widget);
	}
}

void
show_activity_clicked_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	GtkWidget *widget = GHB_WIDGET (ud->builder, "activity_window");
	set_visible(widget, gtk_toggle_tool_button_get_active(
						GTK_TOGGLE_TOOL_BUTTON(xwidget)));
}

void
show_activity_menu_clicked_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	GtkWidget *widget = GHB_WIDGET (ud->builder, "activity_window");
	set_visible(widget, TRUE);
	widget = GHB_WIDGET (ud->builder, "show_activity");
	gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(widget), TRUE);
}

gboolean
activity_window_delete_cb(GtkWidget *xwidget, GdkEvent *event, signal_user_data_t *ud)
{
	set_visible(xwidget, FALSE);
	GtkWidget *widget = GHB_WIDGET (ud->builder, "show_activity");
	gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(widget), FALSE);
	return TRUE;
}

void
ghb_log(gchar *log, ...)
{
	va_list args;
	time_t _now;
    struct tm *now;
	gchar fmt[362];

	_now = time(NULL);
	now = localtime( &_now );
	snprintf(fmt, 362, "[%02d:%02d:%02d] lingui: %s\n", 
			now->tm_hour, now->tm_min, now->tm_sec, log);
	va_start(args, log);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static void
browse_url(const gchar *url)
{
	gboolean result;
	char *argv[] = 
		{"xdg-open",NULL,NULL,NULL};
	argv[1] = (gchar*)url;
	result = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				NULL, NULL, NULL);
	if (result) return;

	argv[0] = "gnome-open";
	result = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				NULL, NULL, NULL);
	if (result) return;

	argv[0] = "kfmclient";
	argv[1] = "exec";
	argv[2] = "http://trac.handbrake.fr/wiki/HandBrakeGuide";
	result = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				NULL, NULL, NULL);
	if (result) return;

	argv[0] = "firefox";
	argv[1] = "http://trac.handbrake.fr/wiki/HandBrakeGuide";
	argv[2] = NULL;
	result = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				NULL, NULL, NULL);
}

void
about_web_hook(GtkAboutDialog *about, const gchar *link, gpointer data)
{
	browse_url(link);
}

void
about_activate_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	GtkWidget *widget = GHB_WIDGET (ud->builder, "hb_about");
	gchar *ver;

	ver = g_strdup_printf("%s (%s)", HB_PROJECT_VERSION, HB_PROJECT_BUILD_ARCH);
	gtk_about_dialog_set_url_hook(about_web_hook, NULL, NULL);
	gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(widget), ver);
	g_free(ver);
	gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(widget), 
								HB_PROJECT_URL_WEBSITE);
	gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(widget), 
										HB_PROJECT_URL_WEBSITE);
	gtk_widget_show (widget);
}

void
guide_activate_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	browse_url("http://trac.handbrake.fr/wiki/HandBrakeGuide");
}

void
hb_about_response_cb(GtkWidget *widget, gint response, signal_user_data_t *ud)
{
	gtk_widget_hide (widget);
}

void
show_queue_clicked_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	GtkWidget *widget = GHB_WIDGET (ud->builder, "queue_window");
	set_visible(widget, gtk_toggle_tool_button_get_active(
						GTK_TOGGLE_TOOL_BUTTON(xwidget)));
}

void
show_queue_menu_clicked_cb(GtkWidget *xwidget, signal_user_data_t *ud)
{
	GtkWidget *widget = GHB_WIDGET (ud->builder, "queue_window");
	set_visible(widget, TRUE);
	widget = GHB_WIDGET (ud->builder, "show_queue");
	gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(widget), TRUE);
}

gboolean
queue_window_delete_cb(GtkWidget *xwidget, GdkEvent *event, signal_user_data_t *ud)
{
	set_visible(xwidget, FALSE);
	GtkWidget *widget = GHB_WIDGET (ud->builder, "show_queue");
	gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(widget), FALSE);
	return TRUE;
}

void
show_presets_toggled_cb(GtkWidget *action, signal_user_data_t *ud)
{
	GtkWidget *widget;
	GtkWindow *hb_window;
	
	g_debug("show_presets_clicked_cb ()");
	widget = GHB_WIDGET (ud->builder, "presets_frame");
	ghb_widget_to_setting(ud->settings, action);
	if (ghb_settings_get_boolean(ud->settings, "show_presets"))
	{
		gtk_widget_show_now(widget);
	}
	else
	{
		gtk_widget_hide(widget);
		hb_window = GTK_WINDOW(GHB_WIDGET (ud->builder, "hb_window"));
		gtk_window_resize(hb_window, 16, 16);
	}
	ghb_pref_save(ud->settings, "show_presets");
}

static void
update_chapter_list(signal_user_data_t *ud)
{
	GtkTreeView *treeview;
	GtkTreeIter iter;
	GtkListStore *store;
	gboolean done;
	GValue *chapters;
	gint titleindex, ii;
	gint count;
	
	g_debug("update_chapter_list ()");
	titleindex = ghb_settings_combo_int(ud->settings, "title");
	chapters = ghb_get_chapters(titleindex);
	count = ghb_array_len(chapters);
	if (chapters)
		ghb_settings_set_value(ud->settings, "chapter_list", chapters);
	
	treeview = GTK_TREE_VIEW(GHB_WIDGET(ud->builder, "chapters_list"));
	store = GTK_LIST_STORE(gtk_tree_view_get_model(treeview));
	ii = 0;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter))
	{
		do
		{

			if (ii < count)
			{
				gchar *chapter;

				// Update row with settings data
				g_debug("Updating row");
				chapter = ghb_value_string(ghb_array_get_nth(chapters, ii));
				gtk_list_store_set(store, &iter, 
					0, ii+1,
					1, chapter,
					2, TRUE,
					-1);
				g_free(chapter);
				ii++;
				done = !gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
			}
			else
			{
				// No more settings data, remove row
				g_debug("Removing row");
				done = !gtk_list_store_remove(store, &iter);
			}
		} while (!done);
	}
	while (ii < count)
	{
		gchar *chapter;

		// Additional settings, add row
		g_debug("Adding row");
		chapter = ghb_value_string(ghb_array_get_nth(chapters, ii));
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, 
			0, ii+1,
			1, chapter,
			2, TRUE,
			-1);
		g_free(chapter);
		ii++;
	}
}

static gint chapter_edit_key = 0;

gboolean
chapter_keypress_cb(
	GhbCellRendererText *cell,
	GdkEventKey *event,
	signal_user_data_t *ud)
{
	chapter_edit_key = event->keyval;
	return FALSE;
}

void
chapter_edited_cb(
	GhbCellRendererText *cell, 
	gchar *path, 
	gchar *text, 
	signal_user_data_t *ud)
{
	GtkTreePath *treepath;
	GtkListStore *store;
	GtkTreeView *treeview;
	GtkTreeIter iter;
	gint index;
	gint *pi;
	gint row;
	
	g_debug("chapter_edited_cb ()");
	g_debug("path (%s)", path);
	g_debug("text (%s)", text);
	treeview = GTK_TREE_VIEW(GHB_WIDGET(ud->builder, "chapters_list"));
	store = GTK_LIST_STORE(gtk_tree_view_get_model(treeview));
	treepath = gtk_tree_path_new_from_string (path);
	pi = gtk_tree_path_get_indices(treepath);
	row = pi[0];
	gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, treepath);
	gtk_list_store_set(store, &iter, 
		1, text,
		2, TRUE,
		-1);
	gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &index, -1);

	const GValue *chapters;
	GValue *chapter;

	chapters = ghb_settings_get_value(ud->settings, "chapter_list");
	chapter = ghb_array_get_nth(chapters, index-1);
	g_value_set_string(chapter, text);
	if ((chapter_edit_key == GDK_Return || chapter_edit_key == GDK_Down) &&
		gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter))
	{
		GtkTreeViewColumn *column;

		gtk_tree_path_next(treepath);
		// When a cell has been edited, I want to advance to the
		// next cell and start editing it automaitcally.
		// Unfortunately, we may not be in a state here where
		// editing is allowed.  This happens when the user selects
		// a new cell with the mouse instead of just hitting enter.
		// Some kind of Gtk quirk.  widget_editable==NULL assertion.
		// Editing is enabled again once the selection event has been
		// processed.  So I'm queueing up a callback to be called
		// when things go idle.  There, I will advance to the next
		// cell and initiate editing.
		//
		// Now, you might be asking why I don't catch the keypress
		// event and determine what action to take based on that.
		// The Gtk developers in their infinite wisdom have made the 
		// actual GtkEdit widget being used a private member of
		// GtkCellRendererText, so it can not be accessed to hang a
		// signal handler off of.  And they also do not propagate the
		// keypress signals in any other way.  So that information is lost.
		//g_idle_add((GSourceFunc)next_cell, ud);
		//
		// Keeping the above comment for posterity.
		// I got industrious and made my own CellTextRendererText that
		// passes on the key-press-event. So now I have much better
		// control of this.
		column = gtk_tree_view_get_column(treeview, 1);
		gtk_tree_view_set_cursor(treeview, treepath, column, TRUE);
	}
	else if (chapter_edit_key == GDK_Up && row > 0)
	{
		GtkTreeViewColumn *column;
		gtk_tree_path_prev(treepath);
		column = gtk_tree_view_get_column(treeview, 1);
		gtk_tree_view_set_cursor(treeview, treepath, column, TRUE);
	}
	gtk_tree_path_free (treepath);
}

void
chapter_list_selection_changed_cb(GtkTreeSelection *selection, signal_user_data_t *ud)
{
	g_debug("chapter_list_selection_changed_cb ()");
	//chapter_selection_changed = TRUE;
}

void
debug_log_handler(const gchar *domain, GLogLevelFlags flags, const gchar *msg, gpointer data)
{
	signal_user_data_t *ud = (signal_user_data_t*)data;
	
	if (ud->debug)
	{
		printf("%s: %s\n", domain, msg);
	}
}

void
warn_log_handler(const gchar *domain, GLogLevelFlags flags, const gchar *msg, gpointer data)
{
	printf("mywarning\n");
	printf("%s: %s\n", domain, msg);
}

void
ghb_hbfd(signal_user_data_t *ud, gboolean hbfd)
{
	GtkWidget *widget;
	g_debug("ghb_hbfd");
	widget = GHB_WIDGET(ud->builder, "queue_pause1");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "queue_add");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "show_queue");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "show_activity");
	set_visible(widget, !hbfd);

	widget = GHB_WIDGET(ud->builder, "chapter_box");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "container_box");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "settings_box");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "presets_save");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET(ud->builder, "presets_remove");
	set_visible(widget, !hbfd);
	widget = GHB_WIDGET (ud->builder, "hb_window");
	gtk_window_resize(GTK_WINDOW(widget), 16, 16);

}

void
hbfd_toggled_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("hbfd_toggled_cb");
	ghb_widget_to_setting (ud->settings, widget);
	gboolean hbfd = ghb_settings_get_boolean(ud->settings, "hbfd");
	ghb_hbfd(ud, hbfd);
	ghb_pref_save(ud->settings, "hbfd");
}

void
pref_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("pref_changed_cb");
	ghb_widget_to_setting (ud->settings, widget);
	ghb_check_dependency(ud, widget);
	const gchar *name = gtk_widget_get_name(widget);
	ghb_pref_save(ud->settings, name);
}

void
vqual_granularity_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("vqual_granularity_changed_cb");
	ghb_widget_to_setting (ud->settings, widget);
	ghb_check_dependency(ud, widget);

	const gchar *name = gtk_widget_get_name(widget);
	ghb_pref_save(ud->settings, name);

	gdouble vqmin, vqmax, step, page;
	gboolean inverted;
	gint digits;

	ghb_vquality_range(ud, &vqmin, &vqmax, &step, &page, &digits, &inverted);
	GtkWidget *qp = GHB_WIDGET(ud->builder, "VideoQualitySlider");
	gtk_range_set_increments (GTK_RANGE(qp), step, page);
}

void
tweaks_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("tweaks_changed_cb");
	ghb_widget_to_setting (ud->settings, widget);
	const gchar *name = gtk_widget_get_name(widget);
	ghb_pref_save(ud->settings, name);
}

void
hbfd_feature_changed_cb(GtkWidget *widget, signal_user_data_t *ud)
{
	g_debug("hbfd_feature_changed_cb");
	ghb_widget_to_setting (ud->settings, widget);
	const gchar *name = gtk_widget_get_name(widget);
	ghb_pref_save(ud->settings, name);

	gboolean hbfd = ghb_settings_get_boolean(ud->settings, "hbfd_feature");
	GtkAction *action;
	if (hbfd)
	{
		const GValue *val;
		val = ghb_settings_get_value(ud->settings, "hbfd");
		ghb_ui_update(ud, "hbfd", val);
	}
	action = GHB_ACTION (ud->builder, "hbfd");
	gtk_action_set_visible(action, hbfd);
}

void
ghb_file_menu_add_dvd(signal_user_data_t *ud)
{
	GList *link, *drives;

	GtkActionGroup *agroup = GTK_ACTION_GROUP(
		gtk_builder_get_object(ud->builder, "actiongroup1"));
	GtkUIManager *ui = GTK_UI_MANAGER(
		gtk_builder_get_object(ud->builder, "uimanager1"));
	guint merge_id = gtk_ui_manager_new_merge_id(ui);

	link = drives = dvd_device_list();
	while (link != NULL)
	{
		gchar *name = (gchar*)link->data;
		// Create action for this drive
		GtkAction *action = gtk_action_new(name, name,
			"Scan this DVD source", "gtk-cdrom");
		// Add action to action group
		gtk_action_group_add_action_with_accel(agroup, action, "");
		// Add to ui manager
		gtk_ui_manager_add_ui(ui, merge_id, 
			"ui/menubar1/menuitem1/quit1", name, name,
			GTK_UI_MANAGER_AUTO, TRUE);
		// Connect signal to action (menu item)
		g_signal_connect(action, "activate", 
			(GCallback)dvd_source_activate_cb, ud);
		g_free(name);
		link = link->next;
	}
	g_list_free(drives);

	// Add separator
	gtk_ui_manager_add_ui(ui, merge_id, 
		"ui/menubar1/menuitem1/quit1", "", NULL,
		GTK_UI_MANAGER_AUTO, TRUE);
}

gboolean ghb_is_cd(GDrive *gd);

static GList*
dvd_device_list()
{
	GVolumeMonitor *gvm;
	GList *drives, *link;
	GList *dvd_devices = NULL;
	
	gvm = g_volume_monitor_get ();
	drives = g_volume_monitor_get_connected_drives (gvm);
	link = drives;
	while (link != NULL)
	{
		GDrive *gd;
		
		gd = (GDrive*)link->data;
		if (ghb_is_cd(gd))
		{
			gchar *device;
			device = g_drive_get_identifier(gd, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
			dvd_devices = g_list_append(dvd_devices, (gpointer)device);
		}
		g_object_unref (gd);
		link = link->next;
	}
	g_list_free(drives);
	return dvd_devices;
}

static LibHalContext *hal_ctx;

gboolean
ghb_is_cd(GDrive *gd)
{
	gchar *device;
	LibHalDrive *halDrive;
	LibHalDriveType dtype;

	device = g_drive_get_identifier(gd, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
	halDrive = libhal_drive_from_device_file (hal_ctx, device);
	dtype = libhal_drive_get_type(halDrive);
	libhal_drive_free(halDrive);
	g_free(device);
	return (dtype == LIBHAL_DRIVE_TYPE_CDROM);
}

void
drive_changed_cb(GVolumeMonitor *gvm, GDrive *gd, signal_user_data_t *ud)
{
	gchar *device;
	gint state = ghb_get_scan_state();
	static gboolean first_time = TRUE;

	if (state != GHB_STATE_IDLE) return;
	if (ud->current_dvd_device == NULL) return;
	// A drive change event happens when the program initially starts
	// and I don't want to automatically scan at that time.
	if (first_time)
	{
		first_time = FALSE;
		return;
	}
	device = g_drive_get_identifier(gd, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
	
	// DVD insertion detected.  Scan it.
	if (strcmp(device, ud->current_dvd_device) == 0)
	{
		if (g_drive_has_media (gd))
		{
			GtkProgressBar *progress;
			progress = GTK_PROGRESS_BAR(GHB_WIDGET (ud->builder, "progressbar"));
			gtk_progress_bar_set_text (progress, "Scanning ...");
			gtk_progress_bar_set_fraction (progress, 0);
 			update_source_label(ud, device);
			ghb_hb_cleanup(TRUE);
			prune_logs(ud);
			gint preview_count;
			preview_count = ghb_settings_get_int(ud->settings, "preview_count");
			ghb_backend_scan(device, 0, preview_count);
		}
		else
		{
			ghb_hb_cleanup(TRUE);
			prune_logs(ud);
			ghb_backend_scan("/dev/null", 0, 1);
		}
	}
	g_free(device);
}


static void
dbus_init (void)
{
	dbus_g_thread_init();
}

#define GPM_DBUS_SERVICE			"org.freedesktop.PowerManagement"
#define GPM_DBUS_INHIBIT_PATH		"/org/freedesktop/PowerManagement/Inhibit"
#define GPM_DBUS_INHIBIT_INTERFACE	"org.freedesktop.PowerManagement.Inhibit" 
static gboolean gpm_inhibited = FALSE;
static guint gpm_cookie = -1;

void
ghb_inhibit_gpm()
{
	DBusGConnection *conn;
	DBusGProxy	*proxy;
	GError *error = NULL;
	gboolean res;
	

	if (gpm_inhibited)
	{
		// Already inhibited
		return;
	}
	g_debug("ghb_inhibit_gpm()");
	conn = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (error != NULL)
	{
		g_debug("DBUS cannot connect: %s", error->message);
		g_error_free(error);
		return;
	}
	proxy = dbus_g_proxy_new_for_name(conn, GPM_DBUS_SERVICE,
							GPM_DBUS_INHIBIT_PATH, GPM_DBUS_INHIBIT_INTERFACE);
	if (proxy == NULL)
	{
		g_debug("Could not get DBUS proxy: %s", GPM_DBUS_SERVICE);
		dbus_g_connection_unref(conn);
		return;
	}
	res = dbus_g_proxy_call(proxy, "Inhibit", &error,
							G_TYPE_STRING, "ghb",
							G_TYPE_STRING, "Encoding",
							G_TYPE_INVALID,
							G_TYPE_UINT, &gpm_cookie,
							G_TYPE_INVALID);
	if (!res)
	{
		g_warning("Inhibit method failed");
		gpm_cookie = -1;
	}
	if (error != NULL)
	{
		g_warning("Inhibit problem: %s", error->message);
		g_error_free(error);
		gpm_cookie = -1;
	}
	gpm_inhibited = TRUE;
	g_object_unref(G_OBJECT(proxy));
	dbus_g_connection_unref(conn);
}

void
ghb_uninhibit_gpm()
{
	DBusGConnection *conn;
	DBusGProxy	*proxy;
	GError *error = NULL;
	gboolean res;
	
	g_debug("ghb_uninhibit_gpm() gpm_cookie %u", gpm_cookie);

	if (!gpm_inhibited)
	{
		// Not inhibited
		return;
	}
	conn = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (error != NULL)
	{
		g_debug("DBUS cannot connect: %s", error->message);
		g_error_free(error);
		return;
	}
	proxy = dbus_g_proxy_new_for_name(conn, GPM_DBUS_SERVICE,
							GPM_DBUS_INHIBIT_PATH, GPM_DBUS_INHIBIT_INTERFACE);
	if (proxy == NULL)
	{
		g_debug("Could not get DBUS proxy: %s", GPM_DBUS_SERVICE);
		dbus_g_connection_unref(conn);
		return;
	}
	res = dbus_g_proxy_call(proxy, "UnInhibit", &error,
							G_TYPE_UINT, gpm_cookie,
							G_TYPE_INVALID,
							G_TYPE_INVALID);
	if (!res)
	{
		g_warning("UnInhibit method failed");
	}
	if (error != NULL)
	{
		g_warning("UnInhibit problem: %s", error->message);
		g_error_free(error);
	}
	gpm_inhibited = FALSE;
	dbus_g_connection_unref(conn);
	g_object_unref(G_OBJECT(proxy));
}

void
ghb_hal_init()
{
	DBusGConnection *gconn;
	DBusConnection *conn;
	GError *gerror = NULL;
	DBusError error;
	char **devices;
	int nr;

	dbus_init ();

	if (!(hal_ctx = libhal_ctx_new ())) {
		g_warning ("failed to create a HAL context!");
		return;
	}

	gconn = dbus_g_bus_get(DBUS_BUS_SYSTEM, &gerror);
	if (gerror != NULL)
	{
		g_warning("DBUS cannot connect: %s", gerror->message);
		g_error_free(gerror);
		return;
	}
	conn = dbus_g_connection_get_connection(gconn);
	libhal_ctx_set_dbus_connection (hal_ctx, conn);
	dbus_error_init (&error);
	if (!libhal_ctx_init (hal_ctx, &error)) {
		g_warning ("libhal_ctx_init failed: %s", error.message ? error.message : "unknown");
		dbus_error_free (&error);
		libhal_ctx_free (hal_ctx);
		dbus_g_connection_unref(gconn);
		return;
	}

	/*
	 * Do something to ping the HAL daemon - the above functions will
	 * succeed even if hald is not running, so long as DBUS is.  But we
	 * want to exit silently if hald is not running, to behave on
	 * pre-2.6 systems.
	 */
	if (!(devices = libhal_get_all_devices (hal_ctx, &nr, &error))) {
		g_warning ("seems that HAL is not running: %s", error.message ? error.message : "unknown");
		dbus_error_free (&error);

		libhal_ctx_shutdown (hal_ctx, NULL);
		libhal_ctx_free (hal_ctx);
		dbus_g_connection_unref(gconn);
		return;
	}

	libhal_free_string_array (devices);
	dbus_g_connection_unref(gconn);
}

gboolean 
tweak_setting_cb(
	GtkWidget *widget, 
	GdkEventButton *event, 
	signal_user_data_t *ud)
{
	const gchar *name;
	gchar *tweak_name;
	gboolean ret = FALSE;
	gboolean allow_tweaks;

	g_debug("press %d %d", event->type, event->button);
	allow_tweaks = ghb_settings_get_boolean(ud->settings, "allow_tweaks");
	if (allow_tweaks && event->type == GDK_BUTTON_PRESS && event->button == 3)
	{ // Its a right mouse click
		GtkWidget *dialog;
		GtkEntry *entry;
		GtkResponseType response;
		gchar *tweak = NULL;

		name = gtk_widget_get_name(widget);
		if (g_str_has_prefix(name, "tweak_"))
		{
			tweak_name = g_strdup(name);
		}
		else
		{
			tweak_name = g_strdup_printf("tweak_%s", name);
		}

		tweak = ghb_settings_get_string (ud->settings, tweak_name);
		dialog = GHB_WIDGET(ud->builder, "tweak_dialog");
		gtk_window_set_title(GTK_WINDOW(dialog), tweak_name);
		entry = GTK_ENTRY(GHB_WIDGET(ud->builder, "tweak_setting"));
		if (tweak)
		{
			gtk_entry_set_text(entry, tweak);
			g_free(tweak);
		}
		response = gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_hide(dialog);
		if (response == GTK_RESPONSE_OK)
		{
			tweak = (gchar*)gtk_entry_get_text(entry);
			if (ghb_validate_filter_string(tweak, -1))
				ghb_settings_set_string(ud->settings, tweak_name, tweak);
			else
			{
				gchar *message;
				message = g_strdup_printf(
							"Invalid Settings:\n%s",
							tweak);
				ghb_message_dialog(GTK_MESSAGE_ERROR, message, "Cancel", NULL);
				g_free(message);
			}
		}
		g_free(tweak_name);
		ret = TRUE;
	}
	return ret;
}

gboolean 
easter_egg_cb(
	GtkWidget *widget, 
	GdkEventButton *event, 
	signal_user_data_t *ud)
{
	g_debug("press %d %d", event->type, event->button);
	if (event->type == GDK_3BUTTON_PRESS && event->button == 1)
	{ // Its a tripple left mouse button click
		GtkWidget *widget;
		widget = GHB_WIDGET(ud->builder, "allow_tweaks");
		gtk_widget_show(widget);
		widget = GHB_WIDGET(ud->builder, "hbfd_feature");
		gtk_widget_show(widget);
	}
	else if (event->type == GDK_BUTTON_PRESS && event->button == 1)
	{
		GtkWidget *widget;
		widget = GHB_WIDGET(ud->builder, "allow_tweaks");
		gtk_widget_hide(widget);
		widget = GHB_WIDGET(ud->builder, "hbfd_feature");
		gtk_widget_hide(widget);
	}
	return FALSE;
}

gchar*
format_deblock_cb(GtkScale *scale, gdouble val, signal_user_data_t *ud)
{
	if (val < 5.0)
	{
		return g_strdup_printf("Off");
	}
	else
	{
		return g_strdup_printf("%d", (gint)val);
	}
}

gchar*
format_drc_cb(GtkScale *scale, gdouble val, signal_user_data_t *ud)
{
	if (val <= 0.0)
	{
		return g_strdup_printf("Off");
	}
	else
	{
		return g_strdup_printf("%.1f", val);
	}
}

gchar*
format_vquality_cb(GtkScale *scale, gdouble val, signal_user_data_t *ud)
{
	gdouble percent;

	gint vcodec = ghb_settings_combo_int(ud->settings, "VideoEncoder");
	switch (vcodec)
	{
		case HB_VCODEC_X264:
		{
			gboolean crf;
			crf = ghb_settings_get_boolean(ud->settings, "constant_rate_factor");
			percent = 100. * (51 - val) / 51.;
			if (crf)
				return g_strdup_printf("RF: %.4g (%.0f%%)", val, percent);
			else
				return g_strdup_printf("QP: %.4g (%.0f%%)", val, percent);
		} break;

		case HB_VCODEC_XVID:
		case HB_VCODEC_FFMPEG:
		{
			percent = 100. * (30 - (val - 1)) / 30.;
			return g_strdup_printf("QP: %d (%.0f%%)", (int)val, percent);
		} break;

		case HB_VCODEC_THEORA:
		{
			percent = 100. * val / 63.;
			return g_strdup_printf("QP: %d (%.0f%%)", (int)val, percent);
		} break;

		default:
		{
			percent = 0;
		} break;
	}
	return g_strdup_printf("QP: %.1f / %.1f%%", val, percent);
}

static void
html_link_cb(GtkHTML *html, const gchar *url, signal_user_data_t *ud)
{
	browse_url(url);
}

static gpointer check_stable_update(signal_user_data_t *ud);
static gboolean stable_update_lock = FALSE;

static void
process_appcast(signal_user_data_t *ud)
{
	gchar *description = NULL, *build = NULL, *version = NULL, *msg;
	GtkWidget *html, *window, *dialog, *label;
	gint	response, ibuild = 0, skip;

	if (ud->appcast == NULL || ud->appcast_len < 15 || 
		strncmp(&(ud->appcast[9]), "200 OK", 6))
	{
		if (!stable_update_lock && hb_get_build(NULL) % 100)
			g_idle_add((GSourceFunc)check_stable_update, ud);
		goto done;
	}
	ghb_appcast_parse(ud->appcast, &description, &build, &version);
	if (build)
		ibuild = g_strtod(build, NULL);
	skip = ghb_settings_get_int(ud->settings, "update_skip_version");
	if (description == NULL || build == NULL || version == NULL 
		|| ibuild <= hb_get_build(NULL) || skip == ibuild)
	{
		if (!stable_update_lock && hb_get_build(NULL) % 100)
			g_thread_create((GThreadFunc)check_stable_update, ud, FALSE, NULL);
		goto done;
	}
	msg = g_strdup_printf("HandBrake %s/%s is now available (you have %s/%d).",
			version, build, hb_get_version(NULL), hb_get_build(NULL));
	label = GHB_WIDGET(ud->builder, "update_message");
	gtk_label_set_text(GTK_LABEL(label), msg);
	html = gtk_html_new_from_string(description, -1);
	g_signal_connect(html, "link_clicked", G_CALLBACK(html_link_cb), ud);
	window = GHB_WIDGET(ud->builder, "update_scroll");
	gtk_container_add(GTK_CONTAINER(window), html);
	// Show it
	dialog = GHB_WIDGET(ud->builder, "update_dialog");
	gtk_widget_set_size_request(html, 420, 240);
	gtk_widget_show(html);
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_hide(dialog);
	gtk_widget_destroy(html);
	if (response == GTK_RESPONSE_OK)
	{
		// Skip
		ghb_settings_set_int(ud->settings, "update_skip_version", ibuild);
		ghb_pref_save(ud->settings, "update_skip_version");
	}
	g_free(msg);

done:
	if (description) g_free(description);
	if (build) g_free(build);
	if (version) g_free(version);
	g_free(ud->appcast);
	ud->appcast_len = 0;
	ud->appcast = NULL;
}

void
ghb_net_close(GIOChannel *ioc)
{
	gint fd;

	g_debug("ghb_net_close");
	if (ioc == NULL) return;
	fd = g_io_channel_unix_get_fd(ioc);
	close(fd);
	g_io_channel_unref(ioc);
}

gboolean
ghb_net_recv_cb(GIOChannel *ioc, GIOCondition cond, gpointer data)
{
	gchar buf[2048];
	gsize len;
	GError *gerror = NULL;
	GIOStatus status;
	
	g_debug("ghb_net_recv_cb");
	signal_user_data_t *ud = (signal_user_data_t*)data;

	status = g_io_channel_read_chars (ioc, buf, 2048, &len, &gerror);
	if ((status == G_IO_STATUS_NORMAL || status == G_IO_STATUS_EOF) &&
		len > 0)
	{
		gint new_len = ud->appcast_len + len;
		ud->appcast = g_realloc(ud->appcast, new_len + 1);
		memcpy(&(ud->appcast[ud->appcast_len]), buf, len);
		ud->appcast_len = new_len;
	}
	if (status == G_IO_STATUS_EOF)
	{
		ud->appcast[ud->appcast_len] = 0;
		ghb_net_close(ioc);
		process_appcast(ud);
		return FALSE;
	}
	return TRUE;
}

GIOChannel*
ghb_net_open(signal_user_data_t *ud, gchar *address, gint port)
{
	GIOChannel *ioc;
	gint fd;

	struct sockaddr_in   sock;
	struct hostent     * host;

	g_debug("ghb_net_open");
	if( !( host = gethostbyname( address ) ) )
	{
		g_warning( "gethostbyname failed (%s)", address );
		return NULL;
	}

	memset( &sock, 0, sizeof( struct sockaddr_in ) );
	sock.sin_family = host->h_addrtype;
	sock.sin_port   = htons( port );
	memcpy( &sock.sin_addr, host->h_addr, host->h_length );

	fd = socket(host->h_addrtype, SOCK_STREAM, 0);
	if( fd < 0 )
	{
		g_debug( "socket failed" );
		return NULL;
	}

	if(connect(fd, (struct sockaddr*)&sock, sizeof(struct sockaddr_in )) < 0 )
	{
		g_debug( "connect failed" );
		return NULL;
	}
	ioc = g_io_channel_unix_new(fd);
	g_io_channel_set_encoding (ioc, NULL, NULL);
	g_io_channel_set_flags(ioc, G_IO_FLAG_NONBLOCK, NULL);
	g_io_add_watch (ioc, G_IO_IN, ghb_net_recv_cb, (gpointer)ud );

	return ioc;
}

gpointer
ghb_check_update(signal_user_data_t *ud)
{
	gchar *query;
	gsize len;
	GIOChannel *ioc;
	GError *gerror = NULL;

	g_debug("ghb_check_update");
	if (hb_get_build(NULL) % 100)
	{
    	query = 
		"GET /appcast_unstable.xml HTTP/1.0\r\nHost: handbrake.fr\r\n\r\n";
	}
	else
	{
		stable_update_lock = TRUE;
    	query = "GET /appcast.xml HTTP/1.0\r\nHost: handbrake.fr\r\n\r\n";
	}
	ioc = ghb_net_open(ud, "handbrake.fr", 80);
	if (ioc == NULL)
		return NULL;

	g_io_channel_write_chars(ioc, query, strlen(query), &len, &gerror);
	g_io_channel_flush(ioc, &gerror);
	// This function is initiated by g_idle_add.  Must return false
	// so that it is not called again
	return NULL;
}

static gpointer
check_stable_update(signal_user_data_t *ud)
{
	gchar *query;
	gsize len;
	GIOChannel *ioc;
	GError *gerror = NULL;

	g_debug("check_stable_update");
	stable_update_lock = TRUE;
   	query = "GET /appcast.xml HTTP/1.0\r\nHost: handbrake.fr\r\n\r\n";
	ioc = ghb_net_open(ud, "handbrake.fr", 80);
	if (ioc == NULL)
		return NULL;

	g_io_channel_write_chars(ioc, query, strlen(query), &len, &gerror);
	g_io_channel_flush(ioc, &gerror);
	// This function is initiated by g_idle_add.  Must return false
	// so that it is not called again
	return NULL;
}

