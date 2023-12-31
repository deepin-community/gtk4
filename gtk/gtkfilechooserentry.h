/* GTK - The GIMP Toolkit
 * gtkfilechooserentry.h: Entry with filename completion
 * Copyright (C) 2003, Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "deprecated/gtkfilechooser.h"

G_BEGIN_DECLS

#define GTK_TYPE_FILE_CHOOSER_ENTRY    (_gtk_file_chooser_entry_get_type ())
#define GTK_FILE_CHOOSER_ENTRY(obj)    (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_FILE_CHOOSER_ENTRY, GtkFileChooserEntry))
#define GTK_IS_FILE_CHOOSER_ENTRY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_FILE_CHOOSER_ENTRY))

typedef struct _GtkFileChooserEntry      GtkFileChooserEntry;

GType              _gtk_file_chooser_entry_get_type           (void) G_GNUC_CONST;
GtkWidget *        _gtk_file_chooser_entry_new                (gboolean             eat_tab,
                                                               gboolean             eat_escape);
void               _gtk_file_chooser_entry_set_action         (GtkFileChooserEntry *chooser_entry,
							       GtkFileChooserAction action);
GtkFileChooserAction _gtk_file_chooser_entry_get_action       (GtkFileChooserEntry *chooser_entry);
void               _gtk_file_chooser_entry_set_base_folder    (GtkFileChooserEntry *chooser_entry,
							       GFile               *folder);
GFile *            _gtk_file_chooser_entry_get_current_folder (GtkFileChooserEntry *chooser_entry);
const char *      _gtk_file_chooser_entry_get_file_part      (GtkFileChooserEntry *chooser_entry);
gboolean           _gtk_file_chooser_entry_get_is_folder      (GtkFileChooserEntry *chooser_entry,
							       GFile               *file);
void               _gtk_file_chooser_entry_select_filename    (GtkFileChooserEntry *chooser_entry);
void               _gtk_file_chooser_entry_set_file_filter    (GtkFileChooserEntry *chooser_entry,
                                                               GtkFileFilter       *filter);
void               gtk_file_chooser_entry_set_text            (GtkFileChooserEntry *entry,
                                                               const char          *text);

G_END_DECLS

