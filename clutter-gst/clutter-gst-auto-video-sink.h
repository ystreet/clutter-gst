/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * clutter-gst-auto-video-sink.c - GStreamer Auto Clutter Video Sink bin.
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

#ifndef __CLUTTER_GST_AUTO_VIDEO_SINK_H__
#define __CLUTTER_GST_AUTO_VIDEO_SINK_H__

#include <gst/gst.h>
#include <clutter/clutter.h>

G_BEGIN_DECLS

#define CLUTTER_GST_TYPE_AUTO_VIDEO_SINK (clutter_gst_auto_video_sink_get_type())

#define CLUTTER_GST_AUTO_VIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
                              CLUTTER_GST_TYPE_AUTO_VIDEO_SINK, \
                              ClutterGstAutoVideoSink))

#define CLUTTER_GST_AUTO_VIDEO_SINK_CAST(obj) \
  ((ClutterGstAutoVideoSink *)(obj))

#define CLUTTER_GST_AUTO_VIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
                           CLUTTER_GST_TYPE_AUTO_VIDEO_SINK, \
                           ClutterGstAutoVideoSinkClass))

#define CLUTTER_GST_AUTO_VIDEO_SINK_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
                              CLUTTER_GST_TYPE_AUTO_VIDEO_SINK, \
                              ClutterGstAutoVideoSinkClass))

#define CLUTTER_GST_IS_AUTO_VIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), \
                              CLUTTER_GST_TYPE_AUTO_VIDEO_SINK))

#define CLUTTER_GST_IS_AUTO_VIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), \
                           CLUTTER_GST_TYPE_AUTO_VIDEO_SINK))

#define CLUTTER_GST_AUTO_VIDEO_SINK_LOCK(obj) G_STMT_START {            \
    GST_LOG_OBJECT (obj,                                                \
                    "locking from thread %p",                           \
                    g_thread_self ());                                  \
    g_mutex_lock (&CLUTTER_GST_AUTO_VIDEO_SINK(obj)->lock);              \
    GST_LOG_OBJECT (obj,                                                \
                    "locked from thread %p",                            \
                    g_thread_self ());                                  \
} G_STMT_END

#define CLUTTER_GST_AUTO_VIDEO_SINK_UNLOCK(obj) G_STMT_START {          \
    GST_LOG_OBJECT (obj,                                                \
                    "unlocking from thread %p",                         \
                    g_thread_self ());                                  \
    g_mutex_unlock (&CLUTTER_GST_AUTO_VIDEO_SINK(obj)->lock);            \
} G_STMT_END

typedef struct _ClutterGstAutoVideoSink ClutterGstAutoVideoSink;
typedef struct _ClutterGstAutoVideoSinkClass ClutterGstAutoVideoSinkClass;

struct _ClutterGstAutoVideoSink
{
  GstBin parent;

  GstPad *sink_pad;
  GstPad *sink_block_pad;
  guint sink_block_id;

  GstElement *child;

  GstCaps *video_caps;
  GSList *sinks;

  gboolean need_async_start;
  gboolean async_pending;
  gboolean setup;

  ClutterTexture *texture;

  GMutex lock;
};

struct _ClutterGstAutoVideoSinkClass
{
  GstBinClass parent_class;
};

GType clutter_gst_auto_video_sink_get_type (void);

G_END_DECLS

#endif /* __CLUTTER_GST_AUTO_VIDEO_SINK_H__ */
