/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * clutter-gst-player.c - Wrap some convenience functions around playbin2
 *
 * Authored By Damien Lespiau    <damien.lespiau@intel.com>
 *             Lionel Landwerlin <lionel.g.landwerlin@linux.intel.com>
 *
 * Copyright (C) 2011 Intel Corporation
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

/**
 * SECTION:clutter-gst-player
 * @short_description: An interface for controlling playback of media data
 *
 * #ClutterGstPlayer is an interface for controlling playback of media
 *  sources. Contrary to most interfaces, you don't need to implement
 *  #ClutterGstPlayer. It already provides an implementation/logic
 *  leaving you only tweak a few properties to get the desired behavior.
 *
 * #ClutterGstPlayer extends and implements #ClutterMedia to create
 * enhanced player.
 *
 * #ClutterMedia is available since Clutter 0.2
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/video/video.h>
#include <gst/tag/tag.h>
#include <gst/interfaces/streamvolume.h>

#include "clutter-gst-debug.h"
#include "clutter-gst-enum-types.h"
#include "clutter-gst-marshal.h"
#include "clutter-gst-player.h"
#include "clutter-gst-private.h"

#if defined (CLUTTER_WINDOWING_X11) && defined (HAVE_HW_DECODER_SUPPORT)
#define GST_USE_UNSTABLE_API 1
#include <gst/video/videocontext.h>
#include <clutter/x11/clutter-x11.h>
#endif


typedef ClutterGstPlayerIface       ClutterGstPlayerInterface;

G_DEFINE_INTERFACE_WITH_CODE (ClutterGstPlayer, clutter_gst_player, G_TYPE_OBJECT,
                              g_type_interface_add_prerequisite (g_define_type_id,
                                                                 CLUTTER_TYPE_MEDIA))

#define PLAYER_GET_PRIVATE(player)                              \
  (g_object_get_qdata (G_OBJECT (player),                       \
                       clutter_gst_player_private_quark))
#define PLAYER_SET_PRIVATE(player,private)                      \
  (g_object_set_qdata (G_OBJECT (player),                       \
                       clutter_gst_player_private_quark,        \
                       private))

#define PLAYER_GET_CLASS_PRIVATE(player)                     \
  (g_type_get_qdata (G_OBJECT_TYPE (player),                 \
                     clutter_gst_player_class_quark))

/* idle timeouts (in ms) */
#define TICK_TIMEOUT        500
#define BUFFERING_TIMEOUT   250

enum
{
  DOWNLOAD_BUFFERING,

  LAST_SIGNAL
};

enum
{
  PROP_0,

  /* ClutterMedia properties */
  PROP_URI,
  PROP_PLAYING,
  PROP_PROGRESS,
  PROP_SUBTITLE_URI,
  PROP_SUBTITLE_FONT_NAME,
  PROP_AUDIO_VOLUME,
  PROP_CAN_SEEK,
  PROP_BUFFER_FILL,
  PROP_DURATION,

  /* ClutterGstPlayer properties */
  PROP_IDLE,
  PROP_USER_AGENT,
  PROP_SEEK_FLAGS,
  PROP_AUDIO_STREAMS,
  PROP_AUDIO_STREAM,
  PROP_SUBTITLE_TRACKS,
  PROP_SUBTITLE_TRACK,
  PROP_IN_SEEK
};

struct _ClutterGstPlayerIfacePrivate
{
  void       (*set_property)		(GObject        *object,
                                         guint           property_id,
                                         const GValue   *value,
                                         GParamSpec     *pspec);
  void       (*get_property)		(GObject        *object,
                                         guint           property_id,
                                         GValue         *value,
                                         GParamSpec     *pspec);
};

typedef struct _ClutterGstPlayerPrivate ClutterGstPlayerPrivate;

/* Elements don't expose header files */
typedef enum {
  GST_PLAY_FLAG_VIDEO         = (1 << 0),
  GST_PLAY_FLAG_AUDIO         = (1 << 1),
  GST_PLAY_FLAG_TEXT          = (1 << 2),
  GST_PLAY_FLAG_VIS           = (1 << 3),
  GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  GST_PLAY_FLAG_DEINTERLACE   = (1 << 9)
} GstPlayFlags;

struct _ClutterGstPlayerPrivate
{
  GObject parent;

  GstElement *pipeline;
  GstBus *bus;

  gchar *uri;

  guint is_idle : 1;
  guint can_seek : 1;
  guint in_seek : 1;
  guint is_changing_uri : 1;
  guint in_error : 1;
  guint in_eos : 1;
  guint in_download_buffering : 1;
  /* when in progressive download, we use the buffer-fill property to signal
   * that we have enough data to play the stream. This flag allows to send
   * the notify that buffer-fill is 1.0 only once */
  guint virtual_stream_buffer_signalled : 1;

  gdouble stacked_progress;

  gdouble target_progress;
  GstState target_state;

  guint tick_timeout_id;
  guint buffering_timeout_id;

  /* This is a cubic volume, suitable for use in a UI cf. StreamVolume doc */
  gdouble volume;

  gdouble buffer_fill;
  gdouble duration;
  gchar *font_name;
  gchar *user_agent;

  GstSeekFlags seek_flags;    /* flags for the seek in set_progress(); */

  GstElement *download_buffering_element;

  GList *audio_streams;
  GList *subtitle_tracks;
};

static GQuark clutter_gst_player_private_quark = 0;
static GQuark clutter_gst_player_class_quark = 0;

static guint signals[LAST_SIGNAL] = { 0, };

static gboolean player_buffering_timeout (gpointer data);

/* Logic */

#ifdef CLUTTER_GST_ENABLE_DEBUG
static gchar *
get_stream_description (GstTagList *tags,
                        gint        track_num)
{
  gchar *description = NULL;

  if (tags)
    {

      gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_CODE, &description);

      if (description)
        {
          const gchar *language = gst_tag_get_language_name (description);

          if (language)
            {
              g_free (description);
              description = g_strdup (language);
            }
        }

      if (!description)
        gst_tag_list_get_string (tags, GST_TAG_CODEC, &description);
    }

  if (!description)
    description = g_strdup_printf ("Track %d", track_num);

  return description;
}

gchar *
list_to_string (GList *list)
{
  GstTagList *tags;
  gchar *description;
  GString *string;
  GList *l;
  gint n, i;

  if (!list)
    return g_strdup ("<empty list>");

  string = g_string_new (NULL);
  n = g_list_length (list);
  for (i = 0, l = list; i < n - 1; i++, l = g_list_next (l))
    {
      tags = l->data;
      description = get_stream_description (tags, i);
      g_string_append_printf (string, "%s, ", description);
      g_free (description);
    }

  tags = l->data;
  description = get_stream_description (tags, i);
  g_string_append_printf (string, "%s", (gchar *) description);
  g_free (description);

  return g_string_free (string, FALSE);
}
#endif

static const gchar *
gst_state_to_string (GstState state)
{
  switch (state)
    {
    case GST_STATE_VOID_PENDING:
      return "pending";
    case GST_STATE_NULL:
      return "null";
    case GST_STATE_READY:
      return "ready";
    case GST_STATE_PAUSED:
      return "paused";
    case GST_STATE_PLAYING:
      return "playing";
    }

  return "Unknown state";
}

static void
free_tags_list (GList **listp)
{
  GList *l;

  l = *listp;
  while (l)
    {
      if (l->data)
        gst_tag_list_free (l->data);
      l = g_list_delete_link (l, l);
    }

  *listp = NULL;
}

static gboolean
tick_timeout (gpointer data)
{
  GObject *player = data;

  g_object_notify (player, "progress");

  return TRUE;
}

static void
player_set_user_agent (ClutterGstPlayer *player,
                       const gchar      *user_agent)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstElement *source;
  GParamSpec *pspec;

  if (user_agent == NULL)
    return;

  g_object_get (priv->pipeline, "source", &source, NULL);
  if (source == NULL)
    return;

  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (source),
                                        "user-agent");
  if (pspec == NULL)
    return;

  CLUTTER_GST_NOTE (MEDIA, "setting user agent: %s", user_agent);

  g_object_set (source, "user-agent", user_agent, NULL);
}

static void
autoload_subtitle (ClutterGstPlayer *player,
                   const gchar      *uri)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  gchar *path, *dot, *subtitle_path;
  GFile *video;
  guint i;

  static const char subtitles_extensions[][4] =
    {
      "sub", "SUB",
      "srt", "SRT",
      "smi", "SMI",
      "ssa", "SSA",
      "ass", "ASS",
      "asc", "ASC"
    };

  /* do not try to look for subtitle files if the video file is not mounted
   * locally */
  if (!g_str_has_prefix (uri, "file://"))
    return;

  /* Retrieve the absolute path of the video file */
  video = g_file_new_for_uri (uri);
  path = g_file_get_path (video);
  g_object_unref (video);
  if (path == NULL)
    return;

  /* Put a '\0' after the dot of the extension */
  dot = strrchr (path, '.');
  if (dot == NULL) {
    g_free (path);
    return;
  }
  *++dot = '\0';

  /* we can't use path as the temporary buffer for the paths of the potential
   * subtitle files as we may not have enough room there */
  subtitle_path = g_malloc (strlen (path) + 1 + 4);
  strcpy (subtitle_path, path);

  /* reuse dot to point to the first byte of the extension of subtitle_path */
  dot = subtitle_path + (dot - path);

  for (i = 0; i < G_N_ELEMENTS (subtitles_extensions); i++)
    {
      GFile *candidate;

      memcpy (dot, subtitles_extensions[i], 4);
      candidate = g_file_new_for_path (subtitle_path);
      if (g_file_query_exists (candidate, NULL))
        {
          gchar *suburi;

          suburi = g_file_get_uri (candidate);

          CLUTTER_GST_NOTE (MEDIA, "found subtitle: %s", suburi);

          g_object_set (priv->pipeline, "suburi", suburi, NULL);
          g_free (suburi);

          g_object_unref (candidate);
          break;
        }

      g_object_unref (candidate);
    }

  g_free (path);
  g_free (subtitle_path);
}

static void
set_subtitle_uri (ClutterGstPlayer *player,
                  const gchar      *uri)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstPlayFlags flags;

  if (!priv->pipeline)
    return;

  CLUTTER_GST_NOTE (MEDIA, "setting subtitle URI: %s", uri);

  g_object_get (priv->pipeline, "flags", &flags, NULL);

  g_object_set (priv->pipeline, "suburi", uri, NULL);

  g_object_set (priv->pipeline, "flags", flags, NULL);
}

static void
player_configure_buffering_timeout (ClutterGstPlayer *player,
                                    guint             ms)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  if (priv->buffering_timeout_id)
    {
      g_source_remove (priv->buffering_timeout_id);
      priv->buffering_timeout_id = 0;
    }

  if (ms)
    {
      priv->buffering_timeout_id =
        g_timeout_add (ms, player_buffering_timeout, player);
    }
}

static void
player_clear_download_buffering (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  if (priv->download_buffering_element)
    {
      g_object_unref (priv->download_buffering_element);
      priv->download_buffering_element = NULL;
    }
  player_configure_buffering_timeout (player, 0);
  priv->in_download_buffering = FALSE;
  priv->virtual_stream_buffer_signalled = 0;
}

static void
set_uri (ClutterGstPlayer *player,
         const gchar      *uri)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GObject *self = G_OBJECT (player);
  GstState state, pending;

  CLUTTER_GST_NOTE (MEDIA, "setting uri %s", uri);

  if (!priv->pipeline)
    return;

  g_free (priv->uri);

  priv->in_eos = FALSE;
  priv->in_error = FALSE;

  if (uri)
    {
      priv->uri = g_strdup (uri);

      /* Ensure the tick timeout is installed.
       *
       * We also have it installed in PAUSED state, because
       * seeks etc may have a delayed effect on the position.
       */
      if (priv->tick_timeout_id == 0)
        {
          priv->tick_timeout_id =
            g_timeout_add (TICK_TIMEOUT, tick_timeout, self);
        }

      /* try to load subtitles based on the uri of the file */
      set_subtitle_uri (player, NULL);

      /* reset the states of download buffering */
      player_clear_download_buffering (player);
    }
  else
    {
      priv->uri = NULL;

      if (priv->tick_timeout_id)
	{
	  g_source_remove (priv->tick_timeout_id);
	  priv->tick_timeout_id = 0;
	}

      if (priv->buffering_timeout_id)
        {
          g_source_remove (priv->buffering_timeout_id);
          priv->buffering_timeout_id = 0;
        }

      if (priv->download_buffering_element)
        {
          g_object_unref (priv->download_buffering_element);
          priv->download_buffering_element = NULL;
        }

    }

  priv->can_seek = FALSE;
  priv->duration = 0.0;
  priv->stacked_progress = 0.0;
  priv->target_progress = 0.0;

  CLUTTER_GST_NOTE (MEDIA, "setting URI: %s", uri);

  if (uri)
    {
      gst_element_get_state (priv->pipeline, &state, &pending, 0);
      if (pending)
        state = pending;

      gst_element_set_state (priv->pipeline, GST_STATE_NULL);

      g_object_set (priv->pipeline, "uri", uri, NULL);
      set_subtitle_uri (player, NULL);
      autoload_subtitle (player, uri);

      gst_element_set_state (priv->pipeline, state);

      priv->is_changing_uri = TRUE;
    }
  else
    {
      priv->is_idle = TRUE;
      set_subtitle_uri (player, NULL);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      g_object_notify (G_OBJECT (player), "idle");
    }

  /*
   * Emit notifications for all these to make sure UI is not showing
   * any properties of the old URI.
   */
  g_object_notify (self, "uri");
  g_object_notify (self, "can-seek");
  g_object_notify (self, "duration");
  g_object_notify (self, "progress");

  free_tags_list (&priv->audio_streams);
  CLUTTER_GST_NOTE (AUDIO_STREAM, "audio-streams changed");
  g_object_notify (self, "audio-streams");

  free_tags_list (&priv->subtitle_tracks);
  CLUTTER_GST_NOTE (SUBTITLES, "subtitle-tracks changed");
  g_object_notify (self, "subtitle-tracks");
}

static void
set_in_seek (ClutterGstPlayer *player,
             gboolean          seeking)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  priv->in_seek = seeking;
  g_object_notify (G_OBJECT (player), "in-seek");
}


static void
set_playing (ClutterGstPlayer *player,
             gboolean          playing)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  if (!priv->pipeline)
    return;

  CLUTTER_GST_NOTE (MEDIA, "set playing: %d", playing);

  priv->in_error = FALSE;
  priv->in_eos = FALSE;

  priv->target_state = playing ? GST_STATE_PLAYING : GST_STATE_PAUSED;

  if (priv->uri)
    {
      set_in_seek (player, FALSE);

      gst_element_set_state (priv->pipeline, priv->target_state);
    }
  else
    {
      if (playing)
       g_warning ("Unable to start playing: no URI is set");
    }

  g_object_notify (G_OBJECT (player), "playing");
  g_object_notify (G_OBJECT (player), "progress");
}

static gboolean
get_playing (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstState state, pending;
  gboolean playing;

  if (!priv->pipeline)
    return FALSE;

  gst_element_get_state (priv->pipeline, &state, &pending, 0);

  if (pending)
    playing = (pending == GST_STATE_PLAYING);
  else
    playing = (state == GST_STATE_PLAYING);

  CLUTTER_GST_NOTE (MEDIA, "get playing: %d", playing);

  return playing;
}

static void
set_progress (ClutterGstPlayer *player,
              gdouble           progress)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstQuery *duration_q;
  gint64 position;

  if (!priv->pipeline)
    return;

  CLUTTER_GST_NOTE (MEDIA, "set progress: %.02f", progress);

  priv->in_eos = FALSE;
  priv->target_progress = progress;

  if (priv->in_download_buffering)
    {
      /* we clear the virtual_stream_buffer_signalled flag as it's likely we
       * need to buffer again */
      priv->virtual_stream_buffer_signalled = 0;
    }

  if (priv->in_seek || priv->is_idle || priv->is_changing_uri)
    {
      /* We can't seek right now, let's save the position where we
         want to seek and do that later. */
      CLUTTER_GST_NOTE (MEDIA,
                        "already seeking/idleing. stacking progress point.");
      priv->stacked_progress = progress;
      return;
    }

  duration_q = gst_query_new_duration (GST_FORMAT_TIME);

  if (gst_element_query (priv->pipeline, duration_q))
    {
      gint64 duration = 0;

      gst_query_parse_duration (duration_q, NULL, &duration);

      position = progress * duration;
    }
  else
    position = 0;

  gst_query_unref (duration_q);

  gst_element_seek (priv->pipeline,
		    1.0,
		    GST_FORMAT_TIME,
		    GST_SEEK_FLAG_FLUSH | priv->seek_flags,
		    GST_SEEK_TYPE_SET,
		    position,
		    GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

  set_in_seek (player, TRUE);

  priv->stacked_progress = 0.0;

  CLUTTER_GST_NOTE (MEDIA, "set progress (seeked): %.02f", progress);
}

static gdouble
get_progress (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstQuery *position_q, *duration_q;
  gdouble progress;

  if (!priv->pipeline)
    return 0.0;

  /* when hitting an error or after an EOS, playbin2 has some weird values when
   * querying the duration and progress. We default to 0.0 on error and 1.0 on
   * EOS */
  if (priv->in_error)
    {
      CLUTTER_GST_NOTE (MEDIA, "get progress (error): 0.0");
      return 0.0;
    }

  if (priv->in_eos)
    {
      CLUTTER_GST_NOTE (MEDIA, "get progress (eos): 1.0");
      return 1.0;
    }

  /* When seeking, the progress returned by playbin2 is 0.0. We want that to be
   * the last known position instead as returning 0.0 will have some ugly
   * effects, say on a progress bar getting updated from the progress tick. */
  if (priv->in_seek || priv->is_changing_uri)
    {
      CLUTTER_GST_NOTE (MEDIA, "get progress (target): %.02f",
                        priv->target_progress);
      return priv->target_progress;
    }

  position_q = gst_query_new_position (GST_FORMAT_TIME);
  duration_q = gst_query_new_duration (GST_FORMAT_TIME);

  if (gst_element_query (priv->pipeline, position_q) &&
      gst_element_query (priv->pipeline, duration_q))
    {
      gint64 position, duration;

      position = duration = 0;

      gst_query_parse_position (position_q, NULL, &position);
      gst_query_parse_duration (duration_q, NULL, &duration);

      progress = CLAMP ((gdouble) position / (gdouble) duration, 0.0, 1.0);
    }
  else
    progress = 0.0;

  gst_query_unref (position_q);
  gst_query_unref (duration_q);

  CLUTTER_GST_NOTE (MEDIA, "get progress (pipeline): %.02f", progress);

  return progress;
}

static void
set_subtitle_font_name (ClutterGstPlayer *player,
                        const gchar      *font_name)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  if (!priv->pipeline)
    return;

  CLUTTER_GST_NOTE (MEDIA, "setting subtitle font to %s", font_name);

  g_free (priv->font_name);
  priv->font_name = g_strdup (font_name);
  g_object_set (priv->pipeline, "subtitle-font-desc", font_name, NULL);
}

static void
set_audio_volume (ClutterGstPlayer *player,
                  gdouble           volume)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

    if (!priv->pipeline)
      return;

  CLUTTER_GST_NOTE (MEDIA, "set volume: %.02f", volume);

  volume = CLAMP (volume, 0.0, 1.0);
  gst_stream_volume_set_volume (GST_STREAM_VOLUME (priv->pipeline),
				GST_STREAM_VOLUME_FORMAT_CUBIC,
				volume);
  g_object_notify (G_OBJECT (player), "audio-volume");
}

static gdouble
get_audio_volume (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  if (!priv->pipeline)
    return 0.0;

  CLUTTER_GST_NOTE (MEDIA, "get volume: %.02f", priv->volume);

  return priv->volume;
}

static gboolean
player_buffering_timeout (gpointer data)
{
  ClutterGstPlayer *player = (ClutterGstPlayer *) data;
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  gdouble start_d, stop_d, seconds_buffered;
  gint64 start, stop, left;
  GstState current_state;
  GstElement *element;
  GstQuery *query;
  gboolean res;

  element = priv->download_buffering_element;
  if (element == NULL)
    element = priv->pipeline;

  /* queue2 only knows about _PERCENT and _BYTES */
  query = gst_query_new_buffering (GST_FORMAT_PERCENT);
  res = gst_element_query (element, query);

  if (res == FALSE)
    {
      priv->buffering_timeout_id = 0;
      player_clear_download_buffering (player);
      return FALSE;
    }

  /* signal the current range */
  gst_query_parse_buffering_stats (query, NULL, NULL, NULL, &left);
  gst_query_parse_buffering_range (query, NULL, &start, &stop, NULL);

  CLUTTER_GST_NOTE (BUFFERING,
                    "start %" G_GINT64_FORMAT ", stop %" G_GINT64_FORMAT
                    ", buffering left %" G_GINT64_FORMAT, start, stop, left);

  start_d = (gdouble) start / GST_FORMAT_PERCENT_MAX;
  stop_d = (gdouble) stop / GST_FORMAT_PERCENT_MAX;

  g_signal_emit (player, signals[DOWNLOAD_BUFFERING], 0, start_d, stop_d);

  /* handle the "virtual stream buffer" and the associated pipeline state.
   * We pause the pipeline until 2s of content is buffered. With the current
   * implementation of queue2, start is always 0, so even when we seek in
   * the stream the start position of the download-buffering signal is
   * always 0.0. FIXME: look at gst_query_parse_nth_buffering_range () */
  seconds_buffered = priv->duration * (stop_d - start_d);
  priv->buffer_fill = seconds_buffered / 2.0;
  priv->buffer_fill = CLAMP (priv->buffer_fill, 0.0, 1.0);

  if (priv->buffer_fill != 1.0 || !priv->virtual_stream_buffer_signalled)
    {
      CLUTTER_GST_NOTE (BUFFERING, "buffer holds %0.2fs of data, buffer-fill "
                        "is %.02f", seconds_buffered, priv->buffer_fill);

      g_object_notify (G_OBJECT (player), "buffer-fill");

      if (priv->buffer_fill == 1.0)
        priv->virtual_stream_buffer_signalled = 1;
    }

  gst_element_get_state (priv->pipeline, &current_state, NULL, 0);
  if (priv->buffer_fill < 1.0)
    {
      if (current_state != GST_STATE_PAUSED)
        {
          CLUTTER_GST_NOTE (BUFFERING, "pausing the pipeline");
          gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
        }
    }
  else
    {
      if (current_state != priv->target_state)
        {
          CLUTTER_GST_NOTE (BUFFERING, "restoring the pipeline");
          gst_element_set_state (priv->pipeline, priv->target_state);
        }
    }

  /* the file has finished downloading */
  if (left == G_GINT64_CONSTANT (0))
    {
      priv->buffering_timeout_id = 0;

      player_clear_download_buffering (player);
      gst_query_unref (query);
      return FALSE;
    }

  gst_query_unref (query);
  return TRUE;
}

static void
bus_message_error_cb (GstBus           *bus,
                      GstMessage       *message,
                      ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GError *error = NULL;

  gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  gst_message_parse_error (message, &error, NULL);
  g_signal_emit_by_name (player, "error", error);
  g_error_free (error);

  priv->is_idle = TRUE;
  g_object_notify (G_OBJECT (player), "idle");
}

/*
 * This is what's intented in the EOS callback:
 *   - receive EOS from playbin 2
 *   - fire the EOS signal, the user can install a signal handler to loop the
 *     video for instance.
 *   - after having emitted the signal, check the state of the pipeline
 *   - if the pipeline has been set back to playing or pause, don't touch the
 *     idle state. This will avoid drawing a frame (or more) with the idle
 *     material when looping
 */
static void
bus_message_eos_cb (GstBus           *bus,
                    GstMessage       *message,
                    ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstState state, pending;

  priv->in_eos = TRUE;

  gst_element_set_state (priv->pipeline, GST_STATE_READY);

  g_signal_emit_by_name (player, "eos");
  g_object_notify (G_OBJECT (player), "progress");

  gst_element_get_state (priv->pipeline, &state, &pending, 0);
  if (pending)
    state = pending;

  if (!(state == GST_STATE_PLAYING || state == GST_STATE_PAUSED))
    {
      priv->is_idle = TRUE;
      g_object_notify (G_OBJECT (player), "idle");
    }
}

static void
bus_message_buffering_cb (GstBus           *bus,
                          GstMessage       *message,
                          ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstBufferingMode mode;
  GstState current_state;
  gint buffer_percent;

  gst_message_parse_buffering_stats (message, &mode, NULL, NULL, NULL);

  if (mode != GST_BUFFERING_DOWNLOAD)
    priv->in_download_buffering = FALSE;

  switch (mode)
    {
    case GST_BUFFERING_STREAM:
      gst_message_parse_buffering (message, &buffer_percent);
      priv->buffer_fill = CLAMP ((gdouble) buffer_percent / 100.0, 0.0, 1.0);

      CLUTTER_GST_NOTE (BUFFERING, "buffer-fill: %.02f", priv->buffer_fill);

      /* The playbin2 documentation says that we need to pause the pipeline
       * when there's not enough data yet. We try to limit the calls to
       * gst_element_set_state() */
      gst_element_get_state (priv->pipeline, &current_state, NULL, 0);

      if (priv->buffer_fill < 1.0)
        {
          if (current_state != GST_STATE_PAUSED)
            {
              CLUTTER_GST_NOTE (BUFFERING, "pausing the pipeline");
              gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
            }
        }
      else
        {
          if (current_state != priv->target_state)
            {
              CLUTTER_GST_NOTE (BUFFERING, "restoring the pipeline");
              gst_element_set_state (priv->pipeline, priv->target_state);
            }
        }

      g_object_notify (G_OBJECT (player), "buffer-fill");
      break;

    case GST_BUFFERING_DOWNLOAD:
      /* we rate limit the messages from GStreamer for a usage in a UI (we
       * don't want *that* many updates). This is done by installing an idle
       * handler querying the buffer range and sending a signal from there */

      if (priv->in_download_buffering)
        break;

      /* install the querying idle handler the first time we receive a download
       * buffering message */
      player_configure_buffering_timeout (player, BUFFERING_TIMEOUT);

      /* pause the stream. the idle timeout will set the target state when
       * having received enough data. We'll use buffer_fill as a "virtual
       * stream buffer" to signal the application we're buffering until we
       * can play back from the downloaded stream. */
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      priv->buffer_fill = 0.0;
      g_object_notify (G_OBJECT (player), "buffer-fill");

      priv->download_buffering_element = g_object_ref (message->src);
      priv->in_download_buffering = TRUE;
      priv->virtual_stream_buffer_signalled = 0;
      break;

    case GST_BUFFERING_TIMESHIFT:
    case GST_BUFFERING_LIVE:
    default:
      g_warning ("Buffering mode %d not handled", mode);
      break;
    }
}

static void
on_source_changed (GstElement       *pipeline,
                   GParamSpec       *pspec,
                   ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  player_set_user_agent (player, priv->user_agent);
}

static void
query_duration (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  gboolean success;
  GstFormat format = GST_FORMAT_TIME;
  gint64 duration;
  gdouble new_duration, difference;

  success = gst_element_query_duration (priv->pipeline,
                                        &format,
                                        &duration);
  if (G_UNLIKELY (success != TRUE))
    return;

  new_duration = (gdouble) duration / GST_SECOND;

  /* while we store the new duration if it sligthly changes, the duration
   * signal is sent only if the new duration is at least one second different
   * from the old one (as the duration signal is mainly used to update the
   * time displayed in a UI */
  difference = ABS (priv->duration - new_duration);
  if (difference > 1e-3)
    {
      CLUTTER_GST_NOTE (MEDIA, "duration: %.02f", new_duration);
      priv->duration = new_duration;

      if (difference > 1.0)
        g_object_notify (G_OBJECT (player), "duration");
    }
}

static void
bus_message_duration_cb (GstBus           *bus,
                         GstMessage       *message,
                         ClutterGstPlayer *player)
{
  gint64 duration;

  /* GstElements send a duration message on the bus with GST_CLOCK_TIME_NONE
   * as duration to signal a new duration */
  gst_message_parse_duration (message, NULL, &duration);
  if (G_UNLIKELY ((GstClockTime) duration != GST_CLOCK_TIME_NONE))
    return;

  query_duration (player);
}

static void
bus_message_state_change_cb (GstBus           *bus,
                             GstMessage       *message,
                             ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  GstState old_state, new_state;
  gpointer src;

  src = GST_MESSAGE_SRC (message);
  if (src != priv->pipeline)
    return;

  gst_message_parse_state_changed (message, &old_state, &new_state, NULL);

  CLUTTER_GST_NOTE (MEDIA, "state change:  %s -> %s",
                    gst_state_to_string (old_state),
                    gst_state_to_string (new_state));

  if (old_state == new_state)
    return;

  if (old_state == GST_STATE_READY &&
      new_state == GST_STATE_PAUSED)
    {
      GstQuery *query;

      /* Determine whether we can seek */
      query = gst_query_new_seeking (GST_FORMAT_TIME);

      if (gst_element_query (priv->pipeline, query))
        {
          gboolean can_seek = FALSE;

          gst_query_parse_seeking (query, NULL, &can_seek,
                                   NULL,
                                   NULL);

          priv->can_seek = (can_seek == TRUE) ? TRUE : FALSE;
        }
      else
        {
	  /* could not query for ability to seek by querying the
           * pipeline; let's crudely try by using the URI
	   */
	  if (priv->uri && g_str_has_prefix (priv->uri, "http://"))
            priv->can_seek = FALSE;
          else
            priv->can_seek = TRUE;
	}

      gst_query_unref (query);

      CLUTTER_GST_NOTE (MEDIA, "can-seek: %d", priv->can_seek);

      g_object_notify (G_OBJECT (player), "can-seek");

      query_duration (player);
    }

  /* is_idle controls the drawing with the idle material */
  if (new_state == GST_STATE_NULL)
    {
      priv->is_idle = TRUE;
      g_object_notify (G_OBJECT (player), "idle");
    }
  else if (new_state == GST_STATE_PLAYING)
    {
      priv->is_idle = FALSE;
      priv->is_changing_uri = FALSE;
      g_object_notify (G_OBJECT (player), "idle");
    }

  if (!priv->is_idle)
    {
      if (priv->stacked_progress)
        {
          set_progress (player, priv->stacked_progress);
        }
    }
}

static void
bus_message_async_done_cb (GstBus           *bus,
                           GstMessage       *message,
                           ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  if (priv->in_seek)
    {
      g_object_notify (G_OBJECT (player), "progress");

      set_in_seek (player, FALSE);

      if (priv->stacked_progress)
        {
          set_progress (player, priv->stacked_progress);
        }
    }
}

static gboolean
on_volume_changed_main_context (gpointer data)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (data);
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  gdouble volume;

  volume =
    gst_stream_volume_get_volume (GST_STREAM_VOLUME (priv->pipeline),
                                  GST_STREAM_VOLUME_FORMAT_CUBIC);
  priv->volume = volume;

  g_object_notify (G_OBJECT (player), "audio-volume");

  g_object_unref (player);

  return FALSE;
}

/* playbin2 proxies the volume property change notification directly from
 * the element having the "volume" property. This means this callback is
 * called from the thread that runs the element, potentially different from
 * the main thread */
static void
on_volume_changed (GstElement       *pipeline,
		   GParamSpec       *pspec,
		   ClutterGstPlayer *player)
{
  g_idle_add (on_volume_changed_main_context, g_object_ref (player));
}

static GList *
get_tags (GstElement  *pipeline,
          const gchar *property_name,
          const gchar *action_signal)
{
  GList *ret = NULL;
  gint i, n;

  g_object_get (G_OBJECT (pipeline), property_name, &n, NULL);
  if (n == 0)
    return NULL;

  for (i = 0; i < n; i++)
    {
      GstTagList *tags = NULL;

      g_signal_emit_by_name (G_OBJECT (pipeline), action_signal, i, &tags);

      ret = g_list_prepend (ret, tags);
    }

  return g_list_reverse (ret);
}

static gboolean
on_audio_changed_main_context (gpointer data)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (data);
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  free_tags_list (&priv->audio_streams);
  priv->audio_streams = get_tags (priv->pipeline, "n-audio", "get-audio-tags");

  CLUTTER_GST_NOTE (AUDIO_STREAM, "audio-streams changed");

  g_object_notify (G_OBJECT (player), "audio-streams");

  g_object_unref (player);

  return FALSE;
}

/* same explanation as for notify::volume's usage of g_idle_add() */
static void
on_audio_changed (GstElement       *pipeline,
                  ClutterGstPlayer *player)
{
  g_idle_add (on_audio_changed_main_context, g_object_ref (player));
}

static void
on_audio_tags_changed (GstElement       *pipeline,
                       gint              stream,
                       ClutterGstPlayer *player)
{
  gint current_stream;

  g_object_get (G_OBJECT (pipeline), "current-audio", &current_stream, NULL);

  if (current_stream != stream)
    return;

  g_idle_add (on_audio_changed_main_context, g_object_ref (player));
}

static gboolean
on_current_audio_changed_main_context (gpointer data)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (data);

  CLUTTER_GST_NOTE (AUDIO_STREAM, "audio stream changed");
  g_object_notify (G_OBJECT (player), "audio-stream");

  g_object_unref (player);

  return FALSE;
}

static void
on_current_audio_changed (GstElement       *pipeline,
                          GParamSpec       *pspec,
                          ClutterGstPlayer *player)
{
  g_idle_add (on_current_audio_changed_main_context, g_object_ref (player));
}

static gboolean
on_text_changed_main_context (gpointer data)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (data);
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);

  free_tags_list (&priv->subtitle_tracks);
  priv->subtitle_tracks = get_tags (priv->pipeline, "n-text", "get-text-tags");

  CLUTTER_GST_NOTE (AUDIO_STREAM, "subtitle-tracks changed");

  g_object_notify (G_OBJECT (player), "subtitle-tracks");

  g_object_unref (player);

  return FALSE;
}

/* same explanation as for notify::volume's usage of g_idle_add() */
static void
on_text_changed (GstElement       *pipeline,
                  ClutterGstPlayer *player)
{
  g_idle_add (on_text_changed_main_context, g_object_ref (player));
}

static void
on_text_tags_changed (GstElement       *pipeline,
                       gint              stream,
                       ClutterGstPlayer *player)
{
  g_idle_add (on_text_changed_main_context, g_object_ref (player));
}

static gboolean
on_current_text_changed_main_context (gpointer data)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (data);

  CLUTTER_GST_NOTE (AUDIO_STREAM, "text stream changed");
  g_object_notify (G_OBJECT (player), "subtitle-track");

  g_object_unref (player);

  return FALSE;
}

static void
on_current_text_changed (GstElement       *pipeline,
                          GParamSpec       *pspec,
                          ClutterGstPlayer *player)
{
  g_idle_add (on_current_text_changed_main_context, g_object_ref (player));
}

/* GObject's magic/madness */

static void
clutter_gst_player_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (object);
  ClutterGstPlayerIfacePrivate *iface_priv;

  switch (property_id)
    {
    case PROP_URI:
      set_uri (player, g_value_get_string (value));
      break;

    case PROP_PLAYING:
      set_playing (player, g_value_get_boolean (value));
      break;

    case PROP_PROGRESS:
      set_progress (player, g_value_get_double (value));
      break;

    case PROP_SUBTITLE_URI:
      set_subtitle_uri (player, g_value_get_string (value));
      break;

    case PROP_SUBTITLE_FONT_NAME:
      set_subtitle_font_name (player, g_value_get_string (value));
      break;

    case PROP_AUDIO_VOLUME:
      set_audio_volume (player, g_value_get_double (value));
      break;

    case PROP_USER_AGENT:
      clutter_gst_player_set_user_agent (player,
                                         g_value_get_string (value));
      break;

    case PROP_SEEK_FLAGS:
      clutter_gst_player_set_seek_flags (player,
                                         g_value_get_flags (value));
      break;

    case PROP_AUDIO_STREAM:
      clutter_gst_player_set_audio_stream (player,
                                           g_value_get_int (value));
      break;

    case PROP_SUBTITLE_TRACK:
      clutter_gst_player_set_subtitle_track (player,
                                             g_value_get_int (value));
      break;

    default:
      iface_priv = PLAYER_GET_CLASS_PRIVATE (object);
      iface_priv->set_property (object, property_id, value, pspec);
    }
}


static void
clutter_gst_player_get_property (GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (object);
  ClutterGstPlayerPrivate *priv = PLAYER_GET_PRIVATE (player);
  ClutterGstPlayerIfacePrivate *iface_priv;
  gchar *str;

  switch (property_id)
    {
    case PROP_URI:
      g_value_set_string (value, priv->uri);
      break;

    case PROP_PLAYING:
      g_value_set_boolean (value, get_playing (player));
      break;

    case PROP_PROGRESS:
      g_value_set_double (value, get_progress (player));
      break;

    case PROP_SUBTITLE_URI:
      g_object_get (priv->pipeline, "suburi", &str, NULL);
      g_value_take_string (value, str);
      break;

    case PROP_SUBTITLE_FONT_NAME:
      g_value_set_string (value, priv->font_name);
      break;

    case PROP_AUDIO_VOLUME:
      g_value_set_double (value, get_audio_volume (player));
      break;

    case PROP_CAN_SEEK:
      g_value_set_boolean (value, priv->can_seek);
      break;

    case PROP_BUFFER_FILL:
      g_value_set_double (value, priv->buffer_fill);
      break;

    case PROP_DURATION:
      g_value_set_double (value, priv->duration);
      break;

    case PROP_IDLE:
      g_value_set_boolean (value, priv->is_idle);
      break;

    case PROP_USER_AGENT:
      {
        gchar *user_agent;

        user_agent = clutter_gst_player_get_user_agent (player);
        g_value_take_string (value, user_agent);
      }
      break;

    case PROP_SEEK_FLAGS:
      {
        ClutterGstSeekFlags seek_flags;

        seek_flags = clutter_gst_player_get_seek_flags (player);
        g_value_set_flags (value, seek_flags);
      }
      break;

    case PROP_AUDIO_STREAMS:
      g_value_set_pointer (value, priv->audio_streams);
      break;

    case PROP_AUDIO_STREAM:
      {
        gint index_;

        index_ = clutter_gst_player_get_audio_stream (player);
        g_value_set_int (value, index_);
      }
      break;

    case PROP_SUBTITLE_TRACKS:
      g_value_set_pointer (value, priv->subtitle_tracks);
      break;

    case PROP_SUBTITLE_TRACK:
      {
        gint index_;

        index_ = clutter_gst_player_get_subtitle_track (player);
        g_value_set_int (value, index_);
      }
      break;

    case PROP_IN_SEEK:
      g_value_set_boolean (value, priv->in_seek);
      break;

    default:
      iface_priv = PLAYER_GET_CLASS_PRIVATE (object);
      iface_priv->get_property (object, property_id, value, pspec);
    }
}

/**
 * clutter_gst_player_class_init:
 * @object_class: a #GObjectClass
 *
 * Adds the #ClutterGstPlayer properties to a class and surchages the
 * set/get_property of #GObjectClass. You should call this
 * function at the end of the class_init method of the class
 * implementing #ClutterGstPlayer.
 *
 * Since: 1.4
 */
void
clutter_gst_player_class_init (GObjectClass *object_class)
{
  ClutterGstPlayerIfacePrivate *priv;

  priv = g_new0 (ClutterGstPlayerIfacePrivate, 1);
  g_type_set_qdata (G_OBJECT_CLASS_TYPE (object_class),
                    clutter_gst_player_class_quark,
                    priv);

  /* Save object's methods we want to override */
  priv->set_property = object_class->set_property;
  priv->get_property = object_class->get_property;

  /* Replace by our methods */
  object_class->set_property = clutter_gst_player_set_property;
  object_class->get_property = clutter_gst_player_get_property;

  /* Override ClutterMedia's properties */
  g_object_class_override_property (object_class,
                                    PROP_URI, "uri");
  g_object_class_override_property (object_class,
                                    PROP_PLAYING, "playing");
  g_object_class_override_property (object_class,
                                    PROP_PROGRESS, "progress");
  g_object_class_override_property (object_class,
                                    PROP_SUBTITLE_URI, "subtitle-uri");
  g_object_class_override_property (object_class,
                                    PROP_SUBTITLE_FONT_NAME,
                                    "subtitle-font-name");
  g_object_class_override_property (object_class,
                                    PROP_AUDIO_VOLUME, "audio-volume");
  g_object_class_override_property (object_class,
                                    PROP_CAN_SEEK, "can-seek");
  g_object_class_override_property (object_class,
                                    PROP_DURATION, "duration");
  g_object_class_override_property (object_class,
                                    PROP_BUFFER_FILL, "buffer-fill");

  /* Override ClutterGstPlayer's properties */
  g_object_class_override_property (object_class,
                                    PROP_IDLE, "idle");
  g_object_class_override_property (object_class,
                                    PROP_USER_AGENT, "user-agent");
  g_object_class_override_property (object_class,
                                    PROP_SEEK_FLAGS, "seek-flags");

  g_object_class_override_property (object_class,
                                    PROP_AUDIO_STREAMS, "audio-streams");
  g_object_class_override_property (object_class,
                                    PROP_AUDIO_STREAM, "audio-stream");

  g_object_class_override_property (object_class,
                                    PROP_SUBTITLE_TRACKS, "subtitle-tracks");
  g_object_class_override_property (object_class,
                                    PROP_SUBTITLE_TRACK, "subtitle-track");
  g_object_class_override_property (object_class,
                                    PROP_IN_SEEK, "in-seek");
}

static GstElement *
get_pipeline (void)
{
  GstElement *pipeline, *audio_sink;

  pipeline = gst_element_factory_make ("playbin2", "pipeline");
  if (!pipeline)
    {
      g_critical ("Unable to create playbin2 element");
      return NULL;
    }

  audio_sink = gst_element_factory_make ("gconfaudiosink", "audio-sink");
  if (!audio_sink)
    {
      audio_sink = gst_element_factory_make ("autoaudiosink", "audio-sink");
      if (!audio_sink)
	{
	  audio_sink = gst_element_factory_make ("alsasink", "audio-sink");
	  g_warning ("Could not create a GST audio_sink. "
		     "Audio unavailable.");

          /* do we even need to bother? */
	  if (!audio_sink)
	    audio_sink = gst_element_factory_make ("fakesink", "audio-sink");
	}
    }

  g_object_set (G_OBJECT (pipeline),
                "audio-sink", audio_sink,
                "subtitle-font-desc", "Sans 16",
                NULL);

  return pipeline;
}

/* ClutterGstPlayerIface implementation */

static GstElement *
clutter_gst_player_get_pipeline_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  return priv->pipeline;
}

static gchar *
clutter_gst_player_get_user_agent_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;
  GstElement *source;
  GParamSpec *pspec;
  gchar *user_agent;

  priv = PLAYER_GET_PRIVATE (player);

  /* If the user has set a custom user agent, we just return it even if it is
   * not used by the current source element of the pipeline */
  if (priv->user_agent)
    return g_strdup (priv->user_agent);

  /* If not, we try to retrieve the user agent used by the current source */
  g_object_get (priv->pipeline, "source", &source, NULL);
  if (source == NULL)
    return NULL;

  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (source),
                                        "user-agent");
  if (pspec == NULL)
    return NULL;

  g_object_get (source, "user-agent", &user_agent, NULL);

  return user_agent;
}

static void
clutter_gst_player_set_user_agent_impl (ClutterGstPlayer *player,
                                        const gchar      *user_agent)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  g_free (priv->user_agent);
  if (user_agent)
    priv->user_agent = g_strdup (user_agent);
  else
    priv->user_agent = NULL;

  player_set_user_agent (player, user_agent);
}

static ClutterGstSeekFlags
clutter_gst_player_get_seek_flags_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  if (priv->seek_flags == GST_SEEK_FLAG_ACCURATE)
    return CLUTTER_GST_SEEK_FLAG_ACCURATE;
  else
    return CLUTTER_GST_SEEK_FLAG_NONE;
}

static void
clutter_gst_player_set_seek_flags_impl (ClutterGstPlayer    *player,
                                        ClutterGstSeekFlags  flags)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  if (flags == CLUTTER_GST_SEEK_FLAG_NONE)
    priv->seek_flags = GST_SEEK_FLAG_KEY_UNIT;
  else if (flags & CLUTTER_GST_SEEK_FLAG_ACCURATE)
    priv->seek_flags = GST_SEEK_FLAG_ACCURATE;
}

static ClutterGstBufferingMode
clutter_gst_player_get_buffering_mode_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;
  GstPlayFlags flags;

  priv = PLAYER_GET_PRIVATE (player);

  g_object_get (G_OBJECT (priv->pipeline), "flags", &flags, NULL);

  if (flags & GST_PLAY_FLAG_DOWNLOAD)
    return CLUTTER_GST_BUFFERING_MODE_DOWNLOAD;

  return CLUTTER_GST_BUFFERING_MODE_STREAM;
}

static void
clutter_gst_player_set_buffering_mode_impl (ClutterGstPlayer        *player,
                                            ClutterGstBufferingMode  mode)
{
  ClutterGstPlayerPrivate *priv;
  GstPlayFlags flags;

  priv = PLAYER_GET_PRIVATE (player);

  g_object_get (G_OBJECT (priv->pipeline), "flags", &flags, NULL);

  switch (mode)
    {
    case CLUTTER_GST_BUFFERING_MODE_STREAM:
      flags &= ~GST_PLAY_FLAG_DOWNLOAD;
      break;

    case CLUTTER_GST_BUFFERING_MODE_DOWNLOAD:
      flags |= GST_PLAY_FLAG_DOWNLOAD;
      break;

    default:
      g_warning ("Unexpected buffering mode %d", mode);
      break;
    }

  g_object_set (G_OBJECT (priv->pipeline), "flags", flags, NULL);
}

static GList *
clutter_gst_player_get_audio_streams_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  if (CLUTTER_GST_DEBUG_ENABLED (AUDIO_STREAM))
    {
      gchar *streams;

      streams = list_to_string (priv->audio_streams);
      CLUTTER_GST_NOTE (AUDIO_STREAM, "audio streams: %s", streams);
      g_free (streams);
    }

  return priv->audio_streams;
}

static gint
clutter_gst_player_get_audio_stream_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;
  gint index_ = -1;

  priv = PLAYER_GET_PRIVATE (player);

  g_object_get (G_OBJECT (priv->pipeline),
                "current-audio", &index_,
                NULL);

  CLUTTER_GST_NOTE (AUDIO_STREAM, "audio stream is #%d", index_);

  return index_;
}

static void
clutter_gst_player_set_audio_stream_impl (ClutterGstPlayer *player,
                                          gint              index_)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  g_return_if_fail (index_ >= 0 &&
                    index_ < (gint) g_list_length (priv->audio_streams));

  CLUTTER_GST_NOTE (AUDIO_STREAM, "set audio audio stream to #%d", index_);

  g_object_set (G_OBJECT (priv->pipeline),
                "current-audio", index_,
                NULL);
}

static GList *
clutter_gst_player_get_subtitle_tracks_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  if (CLUTTER_GST_DEBUG_ENABLED (SUBTITLES))
    {
      gchar *tracks;

      tracks = list_to_string (priv->subtitle_tracks);
      CLUTTER_GST_NOTE (SUBTITLES, "subtitle tracks: %s", tracks);
      g_free (tracks);
    }

  return priv->subtitle_tracks;
}

static gint
clutter_gst_player_get_subtitle_track_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;
  gint index_ = -1;

  priv = PLAYER_GET_PRIVATE (player);

  g_object_get (G_OBJECT (priv->pipeline),
                "current-text", &index_,
                NULL);

  CLUTTER_GST_NOTE (SUBTITLES, "text track is #%d", index_);

  return index_;
}

static void
clutter_gst_player_set_subtitle_track_impl (ClutterGstPlayer *player,
                                            gint              index_)
{
  ClutterGstPlayerPrivate *priv;
  GstPlayFlags flags;

  priv = PLAYER_GET_PRIVATE (player);

  g_return_if_fail (index_ >= -1 &&
                    index_ < (gint) g_list_length (priv->subtitle_tracks));

  CLUTTER_GST_NOTE (SUBTITLES, "set subtitle track to #%d", index_);

  g_object_get (priv->pipeline, "flags", &flags, NULL);
  flags &= ~GST_PLAY_FLAG_TEXT;
  g_object_set (priv->pipeline, "flags", flags, NULL);

  if (index_ >= 0)
    {
      g_object_set (G_OBJECT (priv->pipeline),
                    "current-text", index_,
                    NULL);

      flags |= GST_PLAY_FLAG_TEXT;
      g_object_set (priv->pipeline, "flags", flags, NULL);
    }
}

static gboolean
clutter_gst_player_get_idle_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  return priv->is_idle;
}

static gboolean
clutter_gst_player_get_in_seek_impl (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  priv = PLAYER_GET_PRIVATE (player);

  return priv->in_seek;
}


/**/

#if defined (CLUTTER_WINDOWING_X11) && defined (HAVE_HW_DECODER_SUPPORT)
static GstBusSyncReply
on_sync_message (GstBus * bus, GstMessage * message, gpointer user_data)
{
  Display *display = user_data;
  GstVideoContext *context;
  const gchar **types;

  if (gst_video_context_message_parse_prepare (message, &types, &context)) {
    gint i;

    for (i = 0; types[i]; i++) {

      if (!strcmp(types[i], "x11-display")) {
        gst_video_context_set_context_pointer (context, "x11-display", display);
      }
      else if (!strcmp(types[i], "x11-display-name")) {
        gst_video_context_set_context_string (context, "x11-display-name",
            DisplayString (display));
      } else {
        continue;
      }

      gst_message_unref (message);
      return GST_BUS_DROP;
    }
  }

  return GST_BUS_PASS;
}
#endif

/**
 * clutter_gst_player_init:
 * @player: a #ClutterGstPlayer
 *
 * Initialize a #ClutterGstPlayer instance. You should call this
 * function at the beginning of the init method of the class
 * implementing #ClutterGstPlayer.
 *
 * When you're finished with the ClutterGstPlayer mixin features (usually in
 * the dispose or finalize vfuncs), call clutter_gst_player_deinit() to
 * desallocate the resources created by clutter_gst_player_init().
 *
 * Return value: TRUE if the initialization was successfull, FALSE otherwise.
 *
 * Since: 1.4
 */
gboolean
clutter_gst_player_init (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), FALSE);

  priv = PLAYER_GET_PRIVATE (player);
  if (priv)
    return TRUE;

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  iface->get_pipeline = clutter_gst_player_get_pipeline_impl;
  iface->get_user_agent = clutter_gst_player_get_user_agent_impl;
  iface->set_user_agent = clutter_gst_player_set_user_agent_impl;
  iface->get_seek_flags = clutter_gst_player_get_seek_flags_impl;
  iface->set_seek_flags = clutter_gst_player_set_seek_flags_impl;
  iface->get_buffering_mode = clutter_gst_player_get_buffering_mode_impl;
  iface->set_buffering_mode = clutter_gst_player_set_buffering_mode_impl;
  iface->get_audio_streams = clutter_gst_player_get_audio_streams_impl;
  iface->get_audio_stream = clutter_gst_player_get_audio_stream_impl;
  iface->set_audio_stream = clutter_gst_player_set_audio_stream_impl;
  iface->get_subtitle_tracks = clutter_gst_player_get_subtitle_tracks_impl;
  iface->get_subtitle_track = clutter_gst_player_get_subtitle_track_impl;
  iface->set_subtitle_track = clutter_gst_player_set_subtitle_track_impl;
  iface->get_idle = clutter_gst_player_get_idle_impl;
  iface->get_in_seek = clutter_gst_player_get_in_seek_impl;

  priv = g_slice_new0 (ClutterGstPlayerPrivate);
  PLAYER_SET_PRIVATE (player, priv);

  priv->is_idle = TRUE;
  priv->in_seek = FALSE;
  priv->is_changing_uri = FALSE;
  priv->in_download_buffering = FALSE;

  priv->pipeline = get_pipeline ();
  if (!priv->pipeline)
    {
      g_critical ("Unable to create pipeline");
      return FALSE;
    }

  g_signal_connect (priv->pipeline, "notify::source",
                    G_CALLBACK (on_source_changed), player);

  /* We default to not playing until someone calls set_playing(TRUE) */
  priv->target_state = GST_STATE_PAUSED;

  /* Default to a fast seek, ie. same effect than set_seek_flags (NONE); */
  priv->seek_flags = GST_SEEK_FLAG_KEY_UNIT;

  priv->bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));

  gst_bus_add_signal_watch (priv->bus);

  g_signal_connect_object (priv->bus, "message::error",
			   G_CALLBACK (bus_message_error_cb),
			   player, 0);
  g_signal_connect_object (priv->bus, "message::eos",
			   G_CALLBACK (bus_message_eos_cb),
			   player, 0);
  g_signal_connect_object (priv->bus, "message::buffering",
			   G_CALLBACK (bus_message_buffering_cb),
			   player, 0);
  g_signal_connect_object (priv->bus, "message::duration",
			   G_CALLBACK (bus_message_duration_cb),
			   player, 0);
  g_signal_connect_object (priv->bus, "message::state-changed",
			   G_CALLBACK (bus_message_state_change_cb),
			   player, 0);
  g_signal_connect_object (priv->bus, "message::async-done",
                           G_CALLBACK (bus_message_async_done_cb),
                           player, 0);

  g_signal_connect (priv->pipeline, "notify::volume",
		    G_CALLBACK (on_volume_changed),
                    player);

  g_signal_connect (priv->pipeline, "audio-changed",
                    G_CALLBACK (on_audio_changed),
                    player);
  g_signal_connect (priv->pipeline, "audio-tags-changed",
                    G_CALLBACK (on_audio_tags_changed),
                    player);
  g_signal_connect (priv->pipeline, "notify::current-audio",
                    G_CALLBACK (on_current_audio_changed),
                    player);

  g_signal_connect (priv->pipeline, "text-changed",
                    G_CALLBACK (on_text_changed),
                    player);
  g_signal_connect (priv->pipeline, "text-tags-changed",
                    G_CALLBACK (on_text_tags_changed),
                    player);
  g_signal_connect (priv->pipeline, "notify::current-text",
                    G_CALLBACK (on_current_text_changed),
                    player);

#if defined(CLUTTER_WINDOWING_X11) && defined (HAVE_HW_DECODER_SUPPORT)
  gst_bus_set_sync_handler (priv->bus, on_sync_message,
      clutter_x11_get_default_display ());
#endif

  gst_object_unref (GST_OBJECT (priv->bus));

  return TRUE;
}

/**
 * clutter_gst_player_deinit:
 * @player: a #ClutterGstPlayer
 *
 * Frees the resources created by clutter_gst_player_init(). After
 * clutter_gst_player_deinit() has been called, no other player method can be
 * called on the instance.
 *
 * Since: 1.4
 */
void
clutter_gst_player_deinit (ClutterGstPlayer *player)
{
  ClutterGstPlayerPrivate *priv;

  g_return_if_fail (CLUTTER_GST_IS_PLAYER (player));

  priv = PLAYER_GET_PRIVATE (player);

  if (priv == NULL)
    return;

  PLAYER_SET_PRIVATE (player, NULL);

  if (priv->tick_timeout_id)
    {
      g_source_remove (priv->tick_timeout_id);
      priv->tick_timeout_id = 0;
    }

  if (priv->buffering_timeout_id)
    {
      g_source_remove (priv->buffering_timeout_id);
      priv->buffering_timeout_id = 0;
    }

  if (priv->download_buffering_element)
    {
      g_object_unref (priv->download_buffering_element);
      priv->download_buffering_element = NULL;
    }

  gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  if (priv->bus)
    {
      gst_bus_remove_signal_watch (priv->bus);
      priv->bus = NULL;
    }

  if (priv->pipeline)
    {
      gst_object_unref (GST_OBJECT (priv->pipeline));
      priv->pipeline = NULL;
    }

  g_free (priv->uri);
  g_free (priv->font_name);
  g_free (priv->user_agent);
  free_tags_list (&priv->audio_streams);
  free_tags_list (&priv->subtitle_tracks);

  g_slice_free (ClutterGstPlayerPrivate, priv);
}

static void
clutter_gst_player_default_init (ClutterGstPlayerIface *iface)
{
  GParamSpec *pspec;

  /**
   * ClutterGstPlayer:idle:
   *
   * Whether the #ClutterGstPlayer is in idle mode.
   *
   * Since: 1.4
   */
  pspec = g_param_spec_boolean ("idle",
                                "Idle",
                                "Idle state of the player's pipeline",
                                TRUE,
                                CLUTTER_GST_PARAM_READABLE);
  g_object_interface_install_property (iface, pspec);

  /**
   * ClutterGstPlayer:user-agent:
   *
   * The User Agent used by #ClutterGstPlayer with network protocols.
   *
   * Since: 1.4
   */
  pspec = g_param_spec_string ("user-agent",
                               "User Agent",
                               "User Agent used with network protocols",
                               NULL,
                               CLUTTER_GST_PARAM_READWRITE);
  g_object_interface_install_property (iface, pspec);

  /**
   * ClutterGstPlayer:seek-flags:
   *
   * Flags to use when seeking.
   *
   * Since: 1.4
   */
  pspec = g_param_spec_flags ("seek-flags",
                              "Seek Flags",
                              "Flags to use when seeking",
                              CLUTTER_GST_TYPE_SEEK_FLAGS,
                              CLUTTER_GST_SEEK_FLAG_NONE,
                              CLUTTER_GST_PARAM_READWRITE);
  g_object_interface_install_property (iface, pspec);

  /**
   * ClutterGstPlayer:audio-streams:
   *
   * List of audio streams available on the current media.
   *
   * Since: 1.4
   */
  pspec = g_param_spec_pointer ("audio-streams",
                                "Audio Streams",
                                "List of the audio streams of the media",
                                CLUTTER_GST_PARAM_READABLE);
  g_object_interface_install_property (iface, pspec);

  /**
   * ClutterGstPlayer:audio-stream:
   *
   * Index of the current audio stream.
   *
   * Since: 1.4
   */
  pspec = g_param_spec_int ("audio-stream",
                            "Audio Stream",
                            "Index of the current audio stream",
                            -1, G_MAXINT, -1,
                            CLUTTER_GST_PARAM_READWRITE);
  g_object_interface_install_property (iface, pspec);

  pspec = g_param_spec_pointer ("subtitle-tracks",
                                "Subtitles Tracks",
                                "List of the subtitles tracks of the media",
                                CLUTTER_GST_PARAM_READABLE);
  g_object_interface_install_property (iface, pspec);

  pspec = g_param_spec_int ("subtitle-track",
                            "Subtitles Track",
                            "Index of the current subtitles track",
                            -1, G_MAXINT, -1,
                            CLUTTER_GST_PARAM_READWRITE);
  g_object_interface_install_property (iface, pspec);


  /**
   * ClutterGstPlayer:in-seek:
   *
   * Whether or not the stream is being seeked.
   *
   * Since: 1.6
   */
  pspec = g_param_spec_boolean ("in-seek",
                                "In seek mode",
                                "If currently seeking",
                                FALSE,
                                CLUTTER_GST_PARAM_READABLE);
  g_object_interface_install_property (iface, pspec);


  /* Signals */

  /**
   * ClutterGstPlayer::download-buffering:
   * @player: the #ClutterGstPlayer instance that received the signal
   * @start: start position of the buffering
   * @stop: start position of the buffering
   *
   * The ::download-buffering signal is emitted each time their an
   * update about the buffering of the current media.
   *
   * Since: 1.4
   */
  signals[DOWNLOAD_BUFFERING] =
    g_signal_new ("download-buffering",
                  CLUTTER_GST_TYPE_PLAYER,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterGstPlayerIface,
                                   download_buffering),
                  NULL, NULL,
                  _clutter_gst_marshal_VOID__DOUBLE_DOUBLE,
                  G_TYPE_NONE, 2, G_TYPE_DOUBLE, G_TYPE_DOUBLE);

  /* Setup a quark for per instance private data */
  if (!clutter_gst_player_private_quark)
    {
      clutter_gst_player_private_quark =
        g_quark_from_static_string ("clutter-gst-player-private-quark");
      clutter_gst_player_class_quark =
        g_quark_from_static_string ("clutter-gst-player-class-quark");
    }
}

/* ClutterGstIface */

/**
 * clutter_gst_player_get_pipeline:
 * @player: a #ClutterGstPlayer
 *
 * Retrieves the #GstPipeline used by the @player, for direct use with
 * GStreamer API.
 *
 * Return value: (transfer none): the #GstPipeline element used by the player
 *
 * Since: 1.4
 */
GstElement *
clutter_gst_player_get_pipeline (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), NULL);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_pipeline (player);
}

/**
 * clutter_gst_player_get_user_agent:
 * @player: a #ClutterGstPlayer
 *
 * Retrieves the user agent used when streaming.
 *
 * Return value: the user agent used. The returned string has to be freed with
 * g_free()
 *
 * Since: 1.4
 */
gchar *
clutter_gst_player_get_user_agent (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), NULL);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_user_agent (player);
}

/**
 * clutter_gst_player_set_user_agent:
 * @player: a #ClutterGstPlayer
 * @user_agent: the user agent
 *
 * Sets the user agent to use when streaming.
 *
 * When streaming content, you might want to set a custom user agent, eg. to
 * promote your software, make it appear in statistics or because the server
 * requires a special user agent you want to impersonate.
 *
 * Since: 1.4
 */
void
clutter_gst_player_set_user_agent (ClutterGstPlayer *player,
                                   const gchar      *user_agent)
{
  ClutterGstPlayerIface *iface;

  g_return_if_fail (CLUTTER_GST_IS_PLAYER (player));

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  iface->set_user_agent (player, user_agent);
}

/**
 * clutter_gst_player_get_seek_flags:
 * @player: a #ClutterGstPlayer
 *
 * Get the current value of the seek-flags property.
 *
 * Return value: a combination of #ClutterGstSeekFlags
 *
 * Since: 1.4
 */
ClutterGstSeekFlags
clutter_gst_player_get_seek_flags (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player),
                        CLUTTER_GST_SEEK_FLAG_NONE);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_seek_flags (player);
}

/**
 * clutter_gst_player_set_seek_flags:
 * @player: a #ClutterGstPlayer
 * @flags: a combination of #ClutterGstSeekFlags
 *
 * Seeking can be done with several trade-offs. Clutter-gst defaults
 * to %CLUTTER_GST_SEEK_FLAG_NONE.
 *
 * Since: 1.4
 */
void
clutter_gst_player_set_seek_flags (ClutterGstPlayer    *player,
                                   ClutterGstSeekFlags  flags)
{
  ClutterGstPlayerIface *iface;

  g_return_if_fail (CLUTTER_GST_IS_PLAYER (player));

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  iface->set_seek_flags (player, flags);
}

/**
 * clutter_gst_player_get_buffering_mode:
 * @player: a #ClutterGstPlayer
 *
 * Return value: a #ClutterGstBufferingMode
 *
 * Since: 1.4
 */
ClutterGstBufferingMode
clutter_gst_player_get_buffering_mode (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player),
                        CLUTTER_GST_BUFFERING_MODE_STREAM);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_buffering_mode (player);
}

/**
 * clutter_gst_player_set_buffering_mode:
 * @player: a #ClutterGstPlayer
 * @mode: a #ClutterGstBufferingMode
 *
 * Since: 1.4
 */
void
clutter_gst_player_set_buffering_mode (ClutterGstPlayer        *player,
                                       ClutterGstBufferingMode  mode)
{
  ClutterGstPlayerIface *iface;

  g_return_if_fail (CLUTTER_GST_IS_PLAYER (player));

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  iface->set_buffering_mode (player, mode);
}

/**
 * clutter_gst_player_get_audio_streams:
 * @player: a #ClutterGstPlayer
 *
 * Get the list of audio streams of the current media.
 *
 * Return value: (transfer none) (element-type utf8): a list of
 * strings describing the available audio streams
 *
 * Since: 1.4
 */
GList *
clutter_gst_player_get_audio_streams (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), NULL);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_audio_streams (player);
}

/**
 * clutter_gst_player_get_audio_stream:
 * @player: a #ClutterGstPlayer
 *
 * Get the current audio stream. The number returned in the index of the
 * audio stream playing in the list returned by
 * clutter_gst_player_get_audio_streams().
 *
 * Return value: the index of the current audio stream, -1 if the media has no
 * audio stream
 *
 * Since: 1.4
 */
gint
clutter_gst_player_get_audio_stream (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), -1);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_audio_stream (player);
}

/**
 * clutter_gst_player_set_audio_stream:
 * @player: a #ClutterGstPlayer
 * @index_: the index of the audio stream
 *
 * Set the audio stream to play. @index_ is the index of the stream
 * in the list returned by clutter_gst_player_get_audio_streams().
 *
 * Since: 1.4
 */
void
clutter_gst_player_set_audio_stream (ClutterGstPlayer *player,
                                     gint              index_)
{
  ClutterGstPlayerIface *iface;

  g_return_if_fail (CLUTTER_GST_IS_PLAYER (player));

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  iface->set_audio_stream (player, index_);
}

/**
 * clutter_gst_player_get_subtitle_tracks:
 * @player: a #ClutterGstPlayer
 *
 * Get the list of subtitles tracks of the current media.
 *
 * Return value: (transfer none) (element-type utf8): a list of
 * strings describing the available subtitles tracks
 *
 * Since: 1.4
 */
GList *
clutter_gst_player_get_subtitle_tracks (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), NULL);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_subtitle_tracks (player);
}

/**
 * clutter_gst_player_get_subtitle_track:
 * @player: a #ClutterGstPlayer
 *
 * Get the current subtitles track. The number returned is the index of the
 * subtiles track in the list returned by
 * clutter_gst_player_get_subtitle_tracks().
 *
 * Return value: the index of the current subtitlest track, -1 if the media has
 * no subtitles track or if the subtitles have been turned off
 *
 * Since: 1.4
 */
gint
clutter_gst_player_get_subtitle_track (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), -1);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_subtitle_track (player);
}

/**
 * clutter_gst_player_set_subtitle_track:
 * @player: a #ClutterGstPlayer
 * @index_: the index of the subtitles track
 *
 * Set the subtitles track to play. @index_ is the index of the stream
 * in the list returned by clutter_gst_player_get_subtitle_tracks().
 *
 * If @index_ is -1, the subtitles are turned off.
 *
 * Since: 1.4
 */
void
clutter_gst_player_set_subtitle_track (ClutterGstPlayer *player,
                                       gint              index_)
{
  ClutterGstPlayerIface *iface;

  g_return_if_fail (CLUTTER_GST_IS_PLAYER (player));

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  iface->set_subtitle_track (player, index_);
}

/**
 * clutter_gst_player_get_idle:
 * @player: a #ClutterGstPlayer
 *
 * Get the idle state of the pipeline.
 *
 * Return value: TRUE if the pipline is in idle mode, FALSE otherwise.
 *
 * Since: 1.4
 */
gboolean
clutter_gst_player_get_idle (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), TRUE);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_idle (player);
}

/**
 * clutter_gst_player_get_in_seek:
 * @player: a #ClutterGstPlayer
 *
 * Whether the player is seeking.
 *
 * Return value: TRUE if the player is seeking, FALSE otherwise.
 *
 * Since: 1.6
 */
gboolean
clutter_gst_player_get_in_seek (ClutterGstPlayer *player)
{
  ClutterGstPlayerIface *iface;

  g_return_val_if_fail (CLUTTER_GST_IS_PLAYER (player), FALSE);

  iface = CLUTTER_GST_PLAYER_GET_INTERFACE (player);

  return iface->get_in_seek (player);
}
