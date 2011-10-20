/*
  * Clutter-GStreamer.
  *
  * GStreamer integration library for Clutter.
  *
  * clutter-gst-plugin.c - GStreamer plugin.
  *
  * Authored by Josep Torra  <support@fluendo.com>
  *
  * Copyright (C) 2011 Fluendo, S.A.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <clutter/clutter.h>

#if defined (CLUTTER_WINDOWING_X11)
#include <X11/Xlib.h>
#endif

#include "clutter-gst-video-sink.h"
#include "clutter-gst-auto-video-sink.h"

GST_DEBUG_CATEGORY (clutter_gst_video_sink_debug);
GST_DEBUG_CATEGORY (clutter_gst_auto_video_sink_debug);

/* entry point to initialize the plug-in
  * initialize the plug-in itself
  * register the element factories and pad templates
  * register the features
  */
static gboolean
plugin_init (GstPlugin *plugin)
{
  gboolean ret;

  GST_DEBUG_CATEGORY_INIT (clutter_gst_auto_video_sink_debug,
                           "autocluttersink",
                           0,
                           "clutter auto video sink");

#if defined (CLUTTER_WINDOWING_X11)
  /* Required by some GStreamer element like VA */
  XInitThreads ();
#endif

  /* We must enshure that clutter is initialized */
  if (clutter_init (NULL, NULL) != CLUTTER_INIT_SUCCESS)
    return FALSE;

  ret = gst_element_register (plugin,
                              "cluttersink",
                              GST_RANK_MARGINAL,
                              CLUTTER_GST_TYPE_VIDEO_SINK);
  if (!ret)
    return FALSE;

  ret = gst_element_register (plugin,
                              "autocluttersink",
                              GST_RANK_NONE,
                              CLUTTER_GST_TYPE_AUTO_VIDEO_SINK);
  if (!ret)
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
                   GST_VERSION_MINOR,
                   clutter,
                   "Elements to render to Clutter textures",
                   plugin_init,
                   VERSION,
                   "LGPL",        /* license */
                   PACKAGE,
                   "http://www.clutter-project.org");

