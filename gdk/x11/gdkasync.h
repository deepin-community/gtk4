/* GTK - The GIMP Toolkit
 * gdkasync.h: Utility functions using the Xlib asynchronous interfaces
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

#include "gdkdisplay.h"
#include <X11/Xlib.h>

G_BEGIN_DECLS

typedef struct _GdkChildInfoX11 GdkChildInfoX11;

typedef void (*GdkSendXEventCallback) (Window   window,
				       gboolean success,
				       gpointer data);
typedef void (*GdkRoundTripCallback)  (GdkDisplay *display,
				       gpointer data,
				       gulong serial);

struct _GdkChildInfoX11
{
  Window window;
  int x;
  int y;
  int width;
  int height;
  guint is_mapped : 1;
  guint has_wm_state : 1;
  guint window_class : 2;
};

void _gdk_x11_send_client_message_async (GdkDisplay            *display,
					 Window                 window,
					 gboolean               propagate,
					 glong                  event_mask,
					 XClientMessageEvent   *event_send,
					 GdkSendXEventCallback  callback,
					 gpointer               data);

gboolean _gdk_x11_get_window_child_info (GdkDisplay       *display,
					 Window            window,
					 gboolean          get_wm_state,
					 gboolean         *win_has_wm_state,
					 GdkChildInfoX11 **children,
					 guint            *nchildren);

void _gdk_x11_roundtrip_async           (GdkDisplay           *display, 
					 GdkRoundTripCallback callback,
					 gpointer              data);

G_END_DECLS

