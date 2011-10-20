/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * clutter-gst-private.h - a private header, put whatever you want here.
 *
 * Copyright (C) 2010 Intel Corporation
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __CLUTTER_GST_PRIVATE_H__
#define __CLUTTER_GST_PRIVATE_H__

#include <glib.h>

G_BEGIN_DECLS

/* GLib has some define for that, but defining it ourselves allows to require a
 * lower version of GLib */
#define CLUTTER_GST_PARAM_STATIC        \
  (G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB)

#define CLUTTER_GST_PARAM_READABLE      \
  (G_PARAM_READABLE | CLUTTER_GST_PARAM_STATIC)

#define CLUTTER_GST_PARAM_WRITABLE      \
  (G_PARAM_READABLE | CLUTTER_GST_PARAM_STATIC)

#define CLUTTER_GST_PARAM_READWRITE     \
  (G_PARAM_READABLE | G_PARAM_WRITABLE | CLUTTER_GST_PARAM_STATIC)

gboolean
_internal_plugin_init (GstPlugin *plugin);

G_END_DECLS

#endif /* __CLUTTER_GST_PRIVATE_H__ */
