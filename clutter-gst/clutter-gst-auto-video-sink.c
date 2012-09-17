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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <clutter/clutter.h>

#include "clutter-gst-auto-video-sink.h"
#include "clutter-gst-private.h"

GST_DEBUG_CATEGORY_EXTERN (clutter_gst_auto_video_sink_debug);
#define GST_CAT_DEFAULT clutter_gst_auto_video_sink_debug

static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

enum
{
  PROP_0,
  PROP_TEXTURE,
  PROP_TS_OFFSET
};

#define DEFAULT_TS_OFFSET           0

G_DEFINE_TYPE (ClutterGstAutoVideoSink,
    clutter_gst_auto_video_sink, GST_TYPE_BIN);

#define parent_class clutter_gst_auto_video_sink_parent_class

typedef struct
{
  const gchar *factory_name;
  GstElement *element;
  GstCaps *caps;
} SinkElement;

static GstCaps *
_get_sink_caps (GstElement * sink)
{
  GstPad *sinkpad;
  GstCaps *caps = NULL;

  /* try to activate */
  if (GST_STATE (sink) < GST_STATE_READY &&
      gst_element_set_state (sink, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE)
  {
    goto beach;
  }

  if ((sinkpad = gst_element_get_static_pad (sink, "sink"))) {
    /* Got the sink pad, now let's see which caps will be accepted */
    caps = gst_pad_query_caps (sinkpad, NULL);
  }
  gst_object_unref (sinkpad);

beach:
  return caps;
}

static SinkElement *
_sink_element_create (GstElement * element)
{
  SinkElement *se = NULL;
  GstCaps *caps = NULL;

  /* Check if the sink can be set to READY and recover it's caps */
  if (!(caps = _get_sink_caps (element))) {
    gst_element_set_state (element, GST_STATE_NULL);
    gst_object_unref (element);
    goto beach;
  }

  if ((se = g_new0 (SinkElement, 1))) {
    gst_object_ref_sink (element);
    se->element = element;
    se->caps = caps;
  } else {
    gst_caps_unref (caps);
    gst_object_unref (element);
  }

beach:
  return se;
}

static void
_sink_element_free (gpointer data, gpointer user_data)
{
  SinkElement *se = (SinkElement *) data;

  gst_element_set_state (se->element, GST_STATE_NULL);
  gst_caps_unref (se->caps);
  gst_object_unref (se->element);
  g_free (se);
}

static gboolean
_factory_filter (GstPluginFeature * feature, gpointer data)
{
  const gchar *klass;
  guint rank;

  /* we only care about element factories */
  if (!GST_IS_ELEMENT_FACTORY (feature))
    return FALSE;

  /* video sinks */
  klass = gst_element_factory_get_klass (GST_ELEMENT_FACTORY (feature));
  if (!(strstr (klass, "Sink") && strstr (klass, "Video")))
    return FALSE;

  /* only select elements with autoplugging rank */
  rank = gst_plugin_feature_get_rank (feature);
  if (rank < GST_RANK_MARGINAL)
    return FALSE;

  return TRUE;
}

static gint
_factories_compare_ranks (GstPluginFeature * f1, GstPluginFeature * f2)
{
  gint diff;

  diff = gst_plugin_feature_get_rank (f2) - gst_plugin_feature_get_rank (f1);

  if (diff != 0)
    return diff;

  return strcmp (gst_plugin_feature_get_name (f2),
      gst_plugin_feature_get_name (f1));
}

static GstElement *
_create_element_with_pretty_name (ClutterGstAutoVideoSink * bin,
    GstElementFactory * factory)
{
  GstElement *element;
  gchar *name, *marker;

  marker =
      g_strdup (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)));
  if (g_str_has_suffix (marker, "sink"))
    marker[strlen (marker) - 4] = '\0';
  if (g_str_has_prefix (marker, "gst"))
    g_memmove (marker, marker + 3, strlen (marker + 3) + 1);
  name = g_strdup_printf ("%s-actual-sink-%s", GST_OBJECT_NAME (bin), marker);
  g_free (marker);

  element = gst_element_factory_create (factory, name);
  g_free (name);

  return element;
}

static inline gboolean
_is_clutter_sink (GstElement * element)
{
  GParamSpec *pspec;

  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (element),
      "texture");

  if (pspec == NULL) {
    GST_DEBUG_OBJECT (element, "don't have a texture property");
    return FALSE;
  }

  if (CLUTTER_TYPE_TEXTURE == pspec->value_type ||
      g_type_is_a (pspec->value_type, CLUTTER_TYPE_TEXTURE)) {
    GST_DEBUG_OBJECT (element, "has a clutter texture property");
    return TRUE;
  }

  GST_WARNING_OBJECT (element, "has texture property, but it's of type %s "
      "and we expected it to be of type CLUTTER_TYPE_TEXTURE",
      g_type_name (pspec->value_type));

  return FALSE;
}

static inline void
_sinks_discover (ClutterGstAutoVideoSink * bin)
{
  GstCaps *caps = gst_caps_new_empty ();
  GList *factories, *item;

  factories = gst_registry_feature_filter (gst_registry_get (),
      (GstPluginFeatureFilter) _factory_filter, FALSE, bin);
  factories = g_list_sort (factories, (GCompareFunc) _factories_compare_ranks);

  for (item = factories; item != NULL; item = item->next) {
    GstElementFactory *f = GST_ELEMENT_FACTORY (item->data);
    GstElement *el;
    SinkElement *se;

    if ((el = _create_element_with_pretty_name (bin, f))) {
      GST_DEBUG_OBJECT (bin, "Testing %s",
          gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (f)));

      /* Check for a texture property with CLUTTER_TYPE_TEXTURE type */
      if (!_is_clutter_sink (el)) {
        gst_object_unref (el);
        continue;
      }
      se = _sink_element_create (el);
      if (se) {
        GstCaps *caps_union = gst_caps_merge (caps, gst_caps_ref (se->caps));
        caps = caps_union;
        bin->sinks = g_slist_append (bin->sinks, se);
        GST_DEBUG_OBJECT (bin, "Added %s with caps %" GST_PTR_FORMAT,
            gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (f)), se->caps);
      } else {
        gst_object_unref (el);
      }
    }
  }

  if (!gst_caps_is_empty (caps)) {
    gst_caps_replace (&bin->video_caps, caps);
    GST_DEBUG_OBJECT (bin, "Supported caps %" GST_PTR_FORMAT, bin->video_caps);
  }
  gst_caps_unref (caps);
}

static inline void
_sinks_destroy (ClutterGstAutoVideoSink * bin)
{
  g_slist_foreach (bin->sinks, _sink_element_free, NULL);
  g_slist_free (bin->sinks);
  bin->sinks = NULL;
}

static inline GstElement *
_sinks_find_sink_by_caps (ClutterGstAutoVideoSink * bin, GstCaps * caps)
{
  GstElement *element = NULL;
  GSList *walk = bin->sinks;

  while (walk) {
    SinkElement *se = (SinkElement *) walk->data;
    if (se) {
      GstCaps *intersect = NULL;

      intersect = gst_caps_intersect (caps, se->caps);
      if (!gst_caps_is_empty (intersect)) {
        element = se->element;
        gst_caps_unref (intersect);
        GST_DEBUG_OBJECT (bin, "found sink %" GST_PTR_FORMAT, element);
        goto beach;
      }
      gst_caps_unref (intersect);
    }
    walk = g_slist_next (walk);
  }

beach:
  return element;
}

static void
clutter_gst_auto_video_sink_do_async_start (ClutterGstAutoVideoSink * bin)
{
  GstMessage *message;

  if (!bin->need_async_start) {
    GST_DEBUG_OBJECT (bin, "no async_start needed");
    return;
  }

  bin->async_pending = TRUE;

  GST_INFO_OBJECT (bin, "Sending async_start message");
  message = gst_message_new_async_start (GST_OBJECT_CAST (bin));
  GST_BIN_CLASS (parent_class)->handle_message (GST_BIN_CAST (bin), message);
}

static void
clutter_gst_auto_video_sink_do_async_done (ClutterGstAutoVideoSink * bin)
{
  GstMessage *message;

  if (bin->async_pending) {
    GST_INFO_OBJECT (bin, "Sending async_done message");
    message = gst_message_new_async_done (GST_OBJECT_CAST (bin), FALSE);
    GST_BIN_CLASS (parent_class)->handle_message (GST_BIN_CAST (bin), message);

    bin->async_pending = FALSE;
  }
  bin->need_async_start = FALSE;
}

static gboolean
clutter_gst_auto_video_sink_reconfigure (ClutterGstAutoVideoSink * bin,
    GstCaps * caps)
{
  GstElement *sink;
  GstPad *sink_pad_target = NULL;
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (bin, "reconfigure the bin");

  sink = _sinks_find_sink_by_caps (bin, caps);

  if (sink && sink == bin->child) {
    GST_DEBUG_OBJECT (bin, "we already using that sink, done");
    ret = TRUE;
    goto beach;
  }

  if (bin->child) {
    /* Deactivate current child */
    GST_DEBUG_OBJECT (bin, "going to remove %" GST_PTR_FORMAT, bin->child);
    gst_ghost_pad_set_target (GST_GHOST_PAD (bin->sink_pad), NULL);
    gst_element_set_state (bin->child, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (bin), bin->child);
    bin->child = NULL;
  }

  /* This might have failed */
  if (!sink) {
    GST_ELEMENT_ERROR (bin, LIBRARY, INIT,
        ("No usable video rendering element found."),
        ("Failed detecting a video sink for the requested" " caps."));
    goto beach;
  }

  /* Now we are ready to add the sink to bin */
  bin->child = gst_object_ref (sink);
  g_object_set (G_OBJECT (bin->child), "texture", bin->texture,
      "ts-offset", bin->ts_offset, NULL);

  GST_DEBUG_OBJECT (bin, "going to add %" GST_PTR_FORMAT, bin->child);
  /* Add our child */
  gst_bin_add (GST_BIN (bin), bin->child);
  /* Bring all elements to the bin's state */
  gst_element_sync_state_with_parent (bin->child);
  /* Get the child's sink pad */
  sink_pad_target = gst_element_get_static_pad (bin->child, "sink");

  /* Ghost the sink pad to the appropriate element */
  GST_DEBUG_OBJECT (sink_pad_target, "ghosting pad as bin sink pad");
  gst_ghost_pad_set_target (GST_GHOST_PAD (bin->sink_pad), sink_pad_target);
  gst_object_unref (sink_pad_target);
  ret = TRUE;
beach:
  return ret;
}

static GstPadProbeReturn
clutter_gst_auto_video_sink_sink_pad_blocked_cb (GstPad * pad,
    GstPadProbeInfo * info, gpointer user_data)
{
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK_CAST (user_data);
  GstCaps *caps = NULL;

  /* This only occurs when our bin is first initialised || stream changes */
  if (G_UNLIKELY (!bin->setup)) {

    caps = gst_pad_peer_query_caps (bin->sink_pad, NULL);

    if (G_UNLIKELY (!caps)) {
      GST_WARNING_OBJECT (bin, "no incoming caps defined, can't setup");
      goto beach;
    }

    if (G_UNLIKELY (gst_caps_is_empty (caps))) {
      GST_WARNING_OBJECT (bin, "caps empty, can't setup");
      goto beach;
    }

    GST_DEBUG_OBJECT (bin, "incoming caps %" GST_PTR_FORMAT, caps);

    if (!clutter_gst_auto_video_sink_reconfigure (bin, caps))
      goto beach;

    /* We won't be doing this again unless stream changes */
    bin->setup = TRUE;
  }

  /* Note that we finished our ASYNC state change but our children will have
   * posted their own messages on our bus. */
  clutter_gst_auto_video_sink_do_async_done (bin);

  GST_DEBUG_OBJECT (bin, "unblock the pad");

beach:
  if (caps) {
    gst_caps_unref (caps);
  }
  bin->sink_block_id = 0;
  return GST_PAD_PROBE_REMOVE;
}

static GstCaps *
clutter_gst_auto_video_sink_get_caps (ClutterGstAutoVideoSink * bin)
{
  GstCaps *ret;

  if (bin->video_caps) {
    ret = gst_caps_ref (bin->video_caps);
  } else {
    ret = gst_static_pad_template_get_caps (&sink_template_factory);
  }

  return ret;
}

static gboolean
clutter_gst_auto_video_sink_accept_caps (ClutterGstAutoVideoSink * bin,
    GstCaps * caps)
{
  gboolean ret = FALSE;
  GstCaps *allowed_caps = clutter_gst_auto_video_sink_get_caps (bin);

  if (allowed_caps) {
    GstCaps *result = NULL;

    result = gst_caps_intersect (caps, allowed_caps);

    if (!gst_caps_is_empty (result))
      ret = TRUE;

    caps = result;
  }

  gst_caps_unref (allowed_caps);

  return ret;
}

static gboolean
clutter_gst_auto_video_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  gboolean res;
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;

      gst_query_parse_accept_caps (query, &caps);
      res = clutter_gst_auto_video_sink_accept_caps (bin, caps);
      gst_query_set_accept_caps_result (query, res);
      /* return TRUE, we have answered the query */
      res = TRUE;
    }
      break;
    case GST_QUERY_CAPS:
    {
      GstCaps *caps, *filter;

      gst_query_parse_caps (query, &filter);
      caps = clutter_gst_auto_video_sink_get_caps (bin);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      res = TRUE;
    }
      break;
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }
  return res;
}


static GstStateChangeReturn
clutter_gst_auto_video_sink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS, bret;
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      _sinks_discover (bin);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      CLUTTER_GST_AUTO_VIDEO_SINK_LOCK (bin);
      bin->need_async_start = TRUE;
      /* Here we set our callback to intercept data flow on the first buffer */
      GST_DEBUG_OBJECT (bin, "try to block input pad to setup internal "
          "pipeline");

      if (bin->sink_block_id == 0)
        bin->sink_block_id =
            gst_pad_add_probe (bin->sink_block_pad,
            GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
            clutter_gst_auto_video_sink_sink_pad_blocked_cb, bin, NULL);
      ret = GST_STATE_CHANGE_ASYNC;
      clutter_gst_auto_video_sink_do_async_start (bin);
      CLUTTER_GST_AUTO_VIDEO_SINK_UNLOCK (bin);
      break;
    default:
      break;
  }

  /* do the state change of the children */
  bret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  /* now look at the result of our children and adjust the return value */
  switch (bret) {
    case GST_STATE_CHANGE_FAILURE:
      /* failure, we stop */
      goto activate_failed;
    case GST_STATE_CHANGE_NO_PREROLL:
      /* some child returned NO_PREROLL. This is strange but we never know. We
       * commit our async state change (if any) and return the NO_PREROLL */
      clutter_gst_auto_video_sink_do_async_done (bin);
      ret = bret;
      break;
    case GST_STATE_CHANGE_ASYNC:
      /* some child was async, return this */
      ret = bret;
      break;
    default:
      /* return our previously configured return value */
      break;
  }

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      bin->need_async_start = TRUE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      CLUTTER_GST_AUTO_VIDEO_SINK_LOCK (bin);

      /* Unblock pad */
      if (bin->sink_block_id != 0) {
        gst_pad_remove_probe (bin->sink_block_pad, bin->sink_block_id);
        bin->sink_block_id = 0;
      }
      /* Unset ghost pad target */
      GST_DEBUG_OBJECT (bin, "setting ghost pad target to NULL");
      gst_ghost_pad_set_target (GST_GHOST_PAD (bin->sink_pad), NULL);

      /* Destroy our child */
      if (bin->child) {
        GST_DEBUG_OBJECT (bin->child, "removing child sink");
        gst_element_set_state (bin->child, GST_STATE_NULL);
        gst_bin_remove (GST_BIN (bin), bin->child);
        bin->child = NULL;
      }

      bin->setup = FALSE;
      CLUTTER_GST_AUTO_VIDEO_SINK_UNLOCK (bin);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      _sinks_destroy (bin);
      clutter_gst_auto_video_sink_do_async_done (bin);
      break;
    default:
      break;
  }

  return ret;
  /* ERRORS */
activate_failed:
  {
    GST_DEBUG_OBJECT (bin,
        "element failed to change states -- activation problem?");
    return GST_STATE_CHANGE_FAILURE;
  }
}

static void
clutter_gst_auto_video_sink_dispose (GObject * object)
{
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK (object);

  GST_DEBUG_OBJECT (bin, "Disposing");

  if (bin->child) {
    gst_element_set_state (bin->child, GST_STATE_NULL);
    gst_object_unref (bin->child);
    bin->child = NULL;
  }

  if (bin->sink_block_pad) {
    gst_object_unref (bin->sink_block_pad);
    bin->sink_block_pad = NULL;
  }

  bin->texture = NULL;

  G_OBJECT_CLASS (parent_class)->dispose ((GObject *) object);
}

static void
clutter_gst_auto_video_sink_finalize (GObject * object)
{
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK (object);

  GST_DEBUG_OBJECT (bin, "Destroying");

  _sinks_destroy (bin);

  g_mutex_clear (&bin->lock);

  GST_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

static void
clutter_gst_auto_video_sink_set_texture (ClutterGstAutoVideoSink * bin,
    ClutterTexture * texture)
{
  bin->texture = texture;
  if (bin->setup) {
    g_object_set (G_OBJECT (bin->child), "texture", texture, NULL);
  }
}

static void
clutter_gst_auto_video_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK (object);

  switch (prop_id) {
    case PROP_TEXTURE:
      clutter_gst_auto_video_sink_set_texture (bin, g_value_get_object (value));
      break;
    case PROP_TS_OFFSET:
      bin->ts_offset = g_value_get_int64 (value);
      if (bin->child) {
        g_object_set_property (G_OBJECT (bin->child), pspec->name,
            value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clutter_gst_auto_video_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  ClutterGstAutoVideoSink *bin = CLUTTER_GST_AUTO_VIDEO_SINK (object);

  switch (prop_id) {
    case PROP_TEXTURE:
      g_value_set_object (value, bin->texture);
      break;
    case PROP_TS_OFFSET:
      g_value_set_int64 (value, bin->ts_offset);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clutter_gst_auto_video_sink_class_init (ClutterGstAutoVideoSinkClass * klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GParamSpec *pspec;

  oclass->dispose = clutter_gst_auto_video_sink_dispose;
  oclass->finalize = clutter_gst_auto_video_sink_finalize;
  oclass->set_property = clutter_gst_auto_video_sink_set_property;
  oclass->get_property = clutter_gst_auto_video_sink_get_property;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template_factory));

  gst_element_class_set_details_simple (gstelement_class,
      "Auto Clutter Sink",
      "Sink/Video",
      "Autoplug clutter capable video sinks",
      "Josep Torra <support@fluendo.com>");

  /**
    * ClutterGstAutoVideoSink:texture:
    *
    * This is the texture the video is decoded into. It can be any
    * #ClutterTexture, however Cluter-Gst has a handy subclass,
    * #ClutterGstVideoTexture, that implements the #ClutterMedia
    * interface.
    */
  pspec = g_param_spec_object ("texture",
      "Texture",
      "Texture the video will be decoded into",
      CLUTTER_TYPE_TEXTURE, CLUTTER_GST_PARAM_READWRITE);

  g_object_class_install_property (oclass, PROP_TEXTURE, pspec);

  g_object_class_install_property (oclass, PROP_TS_OFFSET,
      g_param_spec_int64 ("ts-offset", "TS Offset",
          "Timestamp offset in nanoseconds", G_MININT64, G_MAXINT64,
          DEFAULT_TS_OFFSET, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (clutter_gst_auto_video_sink_change_state);
}

static void
clutter_gst_auto_video_sink_init (ClutterGstAutoVideoSink * bin)
{
  GstPad *proxypad;
  GstPadTemplate *template;
  GValue val = { 0, };

  bin->setup = FALSE;
  bin->texture = NULL;
  bin->ts_offset = DEFAULT_TS_OFFSET;

  /* Create a ghost pad with no target at first */
  template = gst_static_pad_template_get (&sink_template_factory);
  bin->sink_pad = gst_ghost_pad_new_no_target_from_template ("sink", template);
  gst_object_unref (template);

  gst_pad_set_active (bin->sink_pad, TRUE);

  proxypad = NULL;

  if (bin->sink_pad) {
    GstIterator *it = gst_pad_iterate_internal_links (bin->sink_pad);
    if (G_UNLIKELY (!it ||
            gst_iterator_next (it,
                &val) !=
            GST_ITERATOR_OK || g_value_get_object (&val) == NULL)) {
      GST_ERROR_OBJECT (bin,
          "failed to get internally linked pad from sinkpad");
    }
    if (it)
      gst_iterator_free (it);
    proxypad = GST_PAD_CAST (g_value_get_object (&val));
  }

  bin->sink_block_pad = proxypad;

  gst_pad_set_query_function (bin->sink_pad,
      GST_DEBUG_FUNCPTR (clutter_gst_auto_video_sink_query));
  gst_element_add_pad (GST_ELEMENT (bin), bin->sink_pad);
  /* Setup the element */
  GST_OBJECT_FLAG_SET (GST_OBJECT (bin), GST_ELEMENT_FLAG_SINK);
  g_mutex_init (&bin->lock);
}
