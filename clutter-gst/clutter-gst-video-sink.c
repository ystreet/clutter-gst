/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * clutter-gst-video-sink.c - Gstreamer Video Sink that renders to a
 *                            Clutter Texture.
 *
 * Authored by Jonathan Matthew  <jonathan@kaolin.wh9.net>,
 *             Chris Lord        <chris@openedhand.com>
 *             Damien Lespiau    <damien.lespiau@intel.com>
 *
 * Copyright (C) 2007,2008 OpenedHand
 * Copyright (C) 2009,2010,2011 Intel Corporation
 * Copyright (C) 2011 Red Hat, Inc.
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
 * SECTION:clutter-gst-video-sink
 * @short_description: GStreamer video sink
 *
 * #ClutterGstVideoSink is a GStreamer sink element that sends
 * data to a #ClutterTexture.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-gst-video-sink.h"
#include "clutter-gst-util.h"
#include "clutter-gst-private.h"
#include "clutter-gst-shaders.h"

#ifdef CLUTTER_COGL_HAS_GL
/* include assembly shaders */
#include "I420.h"
#include "YV12.h"
#endif

#include <gst/gst.h>
#include <gst/gstvalue.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/navigation.h>
#include <gst/riff/riff-ids.h>

#ifdef HAVE_HW_DECODER_SUPPORT
#define GST_USE_UNSTABLE_API 1
#include <gst/video/gstsurfacemeta.h>
#endif

#include <glib.h>
#include <string.h>

/* Flags to give to cogl_texture_new(). Since clutter 1.1.10 put NO_ATLAS to
 * be sure the frames don't end up in an atlas */
#if CLUTTER_CHECK_VERSION(1, 1, 10)
#define CLUTTER_GST_TEXTURE_FLAGS \
  (COGL_TEXTURE_NO_SLICING | COGL_TEXTURE_NO_ATLAS)
#else
#define CLUTTER_GST_TEXTURE_FLAGS  COGL_TEXTURE_NO_SLICING
#endif

static gchar *ayuv_to_rgba_shader = \
     FRAGMENT_SHADER_VARS
     "uniform sampler2D tex;"
     "void main () {"
     "  vec4 color = texture2D (tex, vec2(" TEX_COORD "));"
     "  float y = 1.1640625 * (color.g - 0.0625);"
     "  float u = color.b - 0.5;"
     "  float v = color.a - 0.5;"
     "  color.a = color.r;"
     "  color.r = y + 1.59765625 * v;"
     "  color.g = y - 0.390625 * u - 0.8125 * v;"
     "  color.b = y + 2.015625 * u;"
     "  gl_FragColor = color;"
     FRAGMENT_SHADER_END
     "}";

static gchar *yv12_to_rgba_shader = \
     FRAGMENT_SHADER_VARS
     "uniform sampler2D ytex;"
     "uniform sampler2D utex;"
     "uniform sampler2D vtex;"
     "void main () {"
     "  vec2 coord = vec2(" TEX_COORD ");"
     "  float y = 1.1640625 * (texture2D (ytex, coord).g - 0.0625);"
     "  float u = texture2D (utex, coord).g - 0.5;"
     "  float v = texture2D (vtex, coord).g - 0.5;"
     "  vec4 color;"
     "  color.r = y + 1.59765625 * v;"
     "  color.g = y - 0.390625 * u - 0.8125 * v;"
     "  color.b = y + 2.015625 * u;"
     "  color.a = 1.0;"
     "  gl_FragColor = color;"
     FRAGMENT_SHADER_END
     "}";

#define BASE_SINK_CAPS "{ AYUV," \
                       "YV12," \
                       "I420," \
                       "RGBA," \
                       "BGRA," \
                       "RGB," \
                       "BGR }"


#define SINK_CAPS GST_VIDEO_CAPS_MAKE(BASE_SINK_CAPS)

static GstStaticPadTemplate sinktemplate_all
 = GST_STATIC_PAD_TEMPLATE ("sink",
                            GST_PAD_SINK,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS (SINK_CAPS));

GST_DEBUG_CATEGORY_STATIC (clutter_gst_video_sink_debug);
#define GST_CAT_DEFAULT clutter_gst_video_sink_debug


enum
{
  PROP_0,
  PROP_TEXTURE,
  PROP_UPDATE_PRIORITY
};

typedef enum
{
  CLUTTER_GST_NOFORMAT,
  CLUTTER_GST_RGB32,
  CLUTTER_GST_RGB24,
  CLUTTER_GST_AYUV,
  CLUTTER_GST_YV12,
  CLUTTER_GST_I420,
  CLUTTER_GST_SURFACE
} ClutterGstVideoFormat;

/*
 * features: what does the underlaying video card supports ?
 */
typedef enum _ClutterGstFeatures
{
  CLUTTER_GST_FP             = 0x1, /* fragment programs (ARB fp1.0) */
  CLUTTER_GST_GLSL           = 0x2, /* GLSL */
  CLUTTER_GST_MULTI_TEXTURE  = 0x4, /* multi-texturing */
} ClutterGstFeatures;

/*
 * Custom GSource to signal we have a new frame pending
 */

#define CLUTTER_GST_DEFAULT_PRIORITY    (G_PRIORITY_HIGH_IDLE)

typedef struct _ClutterGstSource
{
  GSource              source;

  ClutterGstVideoSink *sink;
  GMutex               buffer_lock;   /* mutex for the buffer */
  GstBuffer           *buffer;
  gboolean             has_new_caps;
  gboolean             stage_lost;
} ClutterGstSource;

/*
 * renderer: abstracts a backend to render a frame.
 */
typedef void (ClutterGstRendererPaint) (ClutterActor *, ClutterGstVideoSink *);
typedef void (ClutterGstRendererPostPaint) (ClutterActor *,
                                            ClutterGstVideoSink *);

typedef struct _ClutterGstRenderer
{
 const char            *name;     /* user friendly name */
 ClutterGstVideoFormat  format;   /* the format handled by this renderer */
 int                    flags;    /* ClutterGstFeatures ORed flags */
 GstStaticCaps          caps;     /* caps handled by the renderer */

 void (*init)       (ClutterGstVideoSink *sink);
 void (*deinit)     (ClutterGstVideoSink *sink);
 void (*upload)     (ClutterGstVideoSink *sink,
                     GstBuffer           *buffer);
} ClutterGstRenderer;

typedef enum _ClutterGstRendererState
{
  CLUTTER_GST_RENDERER_STOPPED,
  CLUTTER_GST_RENDERER_RUNNING,
  CLUTTER_GST_RENDERER_NEED_GC,
} ClutterGstRendererState;

struct _ClutterGstVideoSinkPrivate
{
  ClutterTexture          *texture;
  CoglMaterial            *material_template;

  ClutterGstVideoFormat    format;
  gboolean                 bgr;
  int                      width;
  int                      height;
  int                      fps_n, fps_d;
  int                      par_n, par_d;

  GMainContext            *clutter_main_context;
  ClutterGstSource        *source;
  int                      priority;

  GSList                  *renderers;
  GstCaps                 *caps;
  ClutterGstRenderer      *renderer;

  GArray                  *signal_handler_ids;

#ifdef HAVE_HW_DECODER_SUPPORT
  GstSurfaceConverter     *converter;
#endif
};

static void
clutter_gst_navigation_interface_init (GstNavigationInterface *iface);

#define clutter_gst_video_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (ClutterGstVideoSink, clutter_gst_video_sink, GST_TYPE_BASE_SINK,
			 G_IMPLEMENT_INTERFACE (GST_TYPE_NAVIGATION,
						clutter_gst_navigation_interface_init));

static void clutter_gst_video_sink_set_texture (ClutterGstVideoSink *sink,
                                                ClutterTexture      *texture);

/*
 * ClutterGstSource implementation
 */

static GSourceFuncs gst_source_funcs;

static ClutterGstSource *
clutter_gst_source_new (ClutterGstVideoSink *sink)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  GSource *source;
  ClutterGstSource *gst_source;

  source = g_source_new (&gst_source_funcs, sizeof (ClutterGstSource));
  gst_source = (ClutterGstSource *) source;

  g_source_set_can_recurse (source, TRUE);
  g_source_set_priority (source, priv->priority);

  gst_source->sink = sink;
  g_mutex_init (&gst_source->buffer_lock);
  gst_source->buffer = NULL;

  return gst_source;
}

static void
clutter_gst_source_finalize (GSource *source)
{
  ClutterGstSource *gst_source = (ClutterGstSource *) source;

  g_mutex_lock (&gst_source->buffer_lock);
  if (gst_source->buffer)
    gst_buffer_unref (gst_source->buffer);
  gst_source->buffer = NULL;
  g_mutex_unlock (&gst_source->buffer_lock);
  g_mutex_clear (&gst_source->buffer_lock);
}

static gboolean
clutter_gst_source_prepare (GSource *source,
                            gint    *timeout)
{
  ClutterGstSource *gst_source = (ClutterGstSource *) source;

  *timeout = -1;

  return gst_source->buffer != NULL;
}

static gboolean
clutter_gst_source_check (GSource *source)
{
  ClutterGstSource *gst_source = (ClutterGstSource *) source;

  return gst_source->buffer != NULL;
}

static ClutterGstRenderer *
clutter_gst_find_renderer_by_format (ClutterGstVideoSink  *sink,
                                     ClutterGstVideoFormat format)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  ClutterGstRenderer *renderer = NULL;
  GSList *element;

  for (element = priv->renderers; element; element = g_slist_next(element))
    {
      ClutterGstRenderer *candidate = (ClutterGstRenderer *)element->data;

      if (candidate->format == format)
        {
          renderer = candidate;
          break;
        }
    }

  return renderer;
}

static void
ensure_texture_pixel_aspect_ratio (ClutterGstVideoSink *sink)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  GParamSpec *pspec;
  GValue par = {0, };

  if (priv->texture == NULL)
    return;

  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (priv->texture),
                                        "pixel-aspect-ratio");
  if (pspec)
    {
      g_value_init (&par, GST_TYPE_FRACTION);
      gst_value_set_fraction (&par, priv->par_n, priv->par_d);
      g_object_set_property (G_OBJECT(priv->texture),
                             "pixel-aspect-ratio", &par);
      g_value_unset (&par);
    }
}

static gboolean
clutter_gst_parse_caps (GstCaps             *caps,
                        ClutterGstVideoSink *sink,
                        gboolean             save)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  GstCaps                    *intersection;
  GstVideoInfo		      vinfo;
  gint               fps_n, fps_d;
  gint               par_n, par_d;
  gint                        width, height;
  ClutterGstVideoFormat       format;
  gboolean                    bgr;
  ClutterGstRenderer         *renderer;

  intersection = gst_caps_intersect (priv->caps, caps);
  if (gst_caps_is_empty (intersection))
    return FALSE;

  gst_caps_unref (intersection);

  if (!gst_video_info_from_caps (&vinfo, caps))
    goto unknown_format;

  width  = vinfo.width;
  height = vinfo.height;

  /* We dont yet use fps or pixel aspect into but handy to have */
  fps_n  = vinfo.fps_n;
  fps_d  = vinfo.fps_d;

  par_n = vinfo.par_n;
  par_d = vinfo.par_d;

  switch (vinfo.finfo->format) {
  case GST_VIDEO_FORMAT_YV12:
    format = CLUTTER_GST_YV12;
    break;
  case GST_VIDEO_FORMAT_I420:
    format = CLUTTER_GST_I420;
    break;
  case GST_VIDEO_FORMAT_AYUV:
    format = CLUTTER_GST_AYUV;
    bgr = FALSE;
    break;
  case GST_VIDEO_FORMAT_RGB:
    format = CLUTTER_GST_RGB24;
    bgr = FALSE;
    break;
  case GST_VIDEO_FORMAT_BGR: 
    format = CLUTTER_GST_RGB24;
    bgr = TRUE;
    break;
  case GST_VIDEO_FORMAT_RGBA:
    format = CLUTTER_GST_RGB32;
    bgr = FALSE;
    break;
  case GST_VIDEO_FORMAT_BGRA:
    format = CLUTTER_GST_RGB32;
    bgr = TRUE;
    break;
  default:
    break;
  }

  /* find a renderer that can display our format */
  renderer = clutter_gst_find_renderer_by_format (sink, format);
  if (G_UNLIKELY (renderer == NULL))
    {
      GST_ERROR_OBJECT (sink, "could not find a suitable renderer");
      return FALSE;
    }

  GST_INFO_OBJECT (sink, "using the %s renderer", renderer->name);

  if (save)
    {
      priv->width  = width;
      priv->height = height;

      /* We dont yet use fps or pixel aspect into but handy to have */
      priv->fps_n  = fps_n;
      priv->fps_d  = fps_d;

      priv->par_n = par_n;
      priv->par_d = par_d;

      /* If we happen to use a ClutterGstVideoTexture, now is to good time
       * to instruct it about the pixel aspect ratio so we can have a
       * correct natural width/height */
      ensure_texture_pixel_aspect_ratio (sink);

      priv->format = format;
      priv->bgr = bgr;

      priv->renderer = renderer;
      GST_INFO_OBJECT (sink, "using the %s renderer", priv->renderer->name);
    }

  return TRUE;

 unknown_format:
  {
    GST_WARNING_OBJECT (sink, "Could not figure format of input caps");
    return FALSE;
  }
}

static gboolean
on_stage_destroyed (ClutterStage *stage,
                    ClutterEvent *event,
                    gpointer      user_data)
{
  ClutterGstSource *gst_source = user_data;
  ClutterGstVideoSinkPrivate *priv = gst_source->sink->priv;

  g_mutex_lock (&gst_source->buffer_lock);

  clutter_actor_hide (CLUTTER_ACTOR (stage));
  clutter_container_remove_actor (CLUTTER_CONTAINER (stage),
      CLUTTER_ACTOR (priv->texture));

  if (gst_source->buffer)
    gst_buffer_unref (gst_source->buffer);

  gst_source->stage_lost = TRUE;
  gst_source->buffer = NULL;
  priv->texture = NULL;

  g_mutex_unlock (&gst_source->buffer_lock);

  return TRUE;
}

static void
on_stage_allocation_changed (ClutterStage          *stage,
                             ClutterActorBox       *box,
                             ClutterAllocationFlags flags,
                             gpointer               user_data)
{
  ClutterGstSource *gst_source = user_data;
  ClutterGstVideoSinkPrivate *priv = gst_source->sink->priv;
  gint width, height;

  if (gst_source->stage_lost)
    return;

  width = (gint) (box->x2 - box->x1);
  height = (gint) (box->y2 - box->y1);

  GST_DEBUG ("Size changed to %i/%i", width, height);
  clutter_actor_set_size (CLUTTER_ACTOR (priv->texture), width, height);
}

static gboolean
clutter_gst_source_dispatch (GSource     *source,
                             GSourceFunc  callback,
                             gpointer     user_data)
{
  ClutterGstSource *gst_source = (ClutterGstSource *) source;
  ClutterGstVideoSinkPrivate *priv = gst_source->sink->priv;
  GstBuffer *buffer;

  g_mutex_lock (&gst_source->buffer_lock);

  if (G_UNLIKELY (gst_source->has_new_caps))
    {
      GstCaps *caps = gst_pad_get_current_caps(
                  GST_BASE_SINK_PAD((GST_BASE_SINK(gst_source->sink))));

      if (priv->renderer)
        priv->renderer->deinit (gst_source->sink);

      clutter_gst_parse_caps (caps, gst_source->sink, TRUE);
      gst_source->has_new_caps = FALSE;

      if (!priv->texture)
        {
          ClutterActor *stage = clutter_stage_get_default ();
          ClutterActor *actor = g_object_new (CLUTTER_TYPE_TEXTURE,
                                              "disable-slicing", TRUE,
                                              NULL);

          clutter_gst_video_sink_set_texture (gst_source->sink,
              CLUTTER_TEXTURE (actor));
          clutter_stage_set_user_resizable (CLUTTER_STAGE (stage), TRUE);
          clutter_container_add_actor (CLUTTER_CONTAINER (stage), actor);
          clutter_stage_set_no_clear_hint (CLUTTER_STAGE (stage), TRUE);

          g_signal_connect (stage, "delete-event",
                            G_CALLBACK (on_stage_destroyed), gst_source);
          g_signal_connect (stage, "allocation-changed",
                            G_CALLBACK (on_stage_allocation_changed), gst_source);

          clutter_gst_parse_caps (caps, gst_source->sink, TRUE);
          clutter_actor_set_size (stage, priv->width, priv->height);
          clutter_actor_show (stage);
        }
      else
        {
          clutter_gst_parse_caps (caps, gst_source->sink, TRUE);
        }

      priv->renderer->init (gst_source->sink);
      gst_source->has_new_caps = FALSE;

      ensure_texture_pixel_aspect_ratio (gst_source->sink);
    }

  buffer = gst_source->buffer;
  gst_source->buffer = NULL;

  g_mutex_unlock (&gst_source->buffer_lock);

  if (buffer)
    {
      priv->renderer->upload (gst_source->sink, buffer);
      gst_buffer_unref (buffer);
    }

  return TRUE;
}

static GSourceFuncs gst_source_funcs = {
  clutter_gst_source_prepare,
  clutter_gst_source_check,
  clutter_gst_source_dispatch,
  clutter_gst_source_finalize
};

static void
clutter_gst_video_sink_set_priority (ClutterGstVideoSink *sink,
                                     int                  priority)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;

  GST_INFO ("GSource priority: %d", priority);
  priv->priority = priority;
  if (priv->source)
    g_source_set_priority ((GSource *) priv->source, priority);
}

/*
 * Small helpers
 */

#ifdef CLUTTER_COGL_HAS_GL
static void
_string_array_to_char_array (char	*dst,
                             const char *src[])
{
  int i, n;

  for (i = 0; src[i]; i++) {
      n = strlen (src[i]);
      memcpy (dst, src[i], n);
      dst += n;
  }
  *dst = '\0';
}
#endif

#if defined (HAVE_COGL_1_8) && !defined (HAVE_CLUTTER_OSX)
static gint
get_n_fragment_texture_units (void)
{
  ClutterBackend *backend;
  CoglContext *context;
  CoglDisplay *display;
  CoglRenderer *renderer;
  gint n;

  backend = clutter_get_default_backend ();
  context = clutter_backend_get_cogl_context (backend);
  display = cogl_context_get_display (context);
  renderer = cogl_display_get_renderer (display);

  n = cogl_renderer_get_n_fragment_texture_units (renderer);

  return n;
}
#else
static gint
get_n_fragment_texture_units (void)
{
  gint n_texture_units;

  glGetIntegerv (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &n_texture_units);
  return n_texture_units;
}
#endif

static CoglHandle
_create_cogl_program (const char *source)
{
  CoglHandle shader;
  CoglHandle program;

  /* Create shader through Cogl - necessary as we need to be able to set
   * integer uniform variables for multi-texturing.
   */
  shader = cogl_create_shader (COGL_SHADER_TYPE_FRAGMENT);
  cogl_shader_source (shader, source);
  cogl_shader_compile (shader);

  program = cogl_create_program ();
  cogl_program_attach_shader (program, shader);
  cogl_program_link (program);

  cogl_handle_unref (shader);

  return program;
}

static void
_create_template_material (ClutterGstVideoSink *sink,
                           const char *source,
                           gboolean set_uniforms,
                           int n_layers)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  CoglMaterial *template;
  int i;

  if (priv->material_template)
    cogl_object_unref (priv->material_template);

  template = cogl_material_new ();

  if (source)
    {
      CoglHandle program = _create_cogl_program (source);

      if (set_uniforms)
        {
          unsigned int location;

          cogl_program_use (program);

          location = cogl_program_get_uniform_location (program, "ytex");
          cogl_program_set_uniform_1i (program, location, 0);
          if (n_layers > 1)
            {
              location = cogl_program_get_uniform_location (program, "utex");
              cogl_program_set_uniform_1i (program, location, 1);
            }
          if (n_layers > 2)
            {
              location = cogl_program_get_uniform_location (program, "vtex");
              cogl_program_set_uniform_1i (program, location, 2);
            }

          cogl_program_use (COGL_INVALID_HANDLE);
        }

      cogl_material_set_user_program (template, program);
      cogl_handle_unref (program);
    }

  for (i = 0; i < n_layers; i++)
    cogl_material_set_layer (template, i, COGL_INVALID_HANDLE);

  priv->material_template = template;
}

static void
_create_paint_material (ClutterGstVideoSink *sink,
                        CoglHandle tex0,
                        CoglHandle tex1,
                        CoglHandle tex2)
{
  ClutterGstVideoSinkPrivate *priv= sink->priv;
  CoglMaterial *material = cogl_material_copy (priv->material_template);

  if (tex0 != COGL_INVALID_HANDLE)
    {
      cogl_material_set_layer (material, 0, tex0);
      cogl_handle_unref (tex0);
    }
  if (tex1 != COGL_INVALID_HANDLE)
    {
      cogl_material_set_layer (material, 1, tex1);
      cogl_handle_unref (tex1);
    }
  if (tex2 != COGL_INVALID_HANDLE)
    {
      cogl_material_set_layer (material, 2, tex2);
      cogl_handle_unref (tex2);
    }

  clutter_texture_set_cogl_material (priv->texture, material);
  cogl_object_unref (material);
}

static void
clutter_gst_dummy_deinit (ClutterGstVideoSink *sink)
{
}

static void
clutter_gst_rgb_init (ClutterGstVideoSink *sink)
{
  _create_template_material (sink, NULL, FALSE, 1);
}

/*
 * RGB 24 / BGR 24
 *
 * 3 bytes per pixel, stride % 4 = 0.
 */

static void
clutter_gst_rgb24_upload (ClutterGstVideoSink *sink,
                          GstBuffer           *buffer)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  CoglPixelFormat format;
  CoglHandle tex;
  GstMapInfo info;

  if (priv->bgr)
    format = COGL_PIXEL_FORMAT_BGR_888;
  else
    format = COGL_PIXEL_FORMAT_RGB_888;

  gst_buffer_map (buffer, &info, GST_MAP_READ);

  tex = cogl_texture_new_from_data (priv->width,
                                    priv->height,
                                    CLUTTER_GST_TEXTURE_FLAGS,
                                    format,
                                    format,
                                    GST_ROUND_UP_4 (3 * priv->width),
                                    info.data);

  gst_buffer_unmap (buffer, &info);

  _create_paint_material (sink,
                          tex,
                          COGL_INVALID_HANDLE,
                          COGL_INVALID_HANDLE);
}

static ClutterGstRenderer rgb24_renderer =
{
  "RGB 24",
  CLUTTER_GST_RGB24,
  0,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE("{ RGB, BGR }")),
  clutter_gst_rgb_init,
  clutter_gst_dummy_deinit,
  clutter_gst_rgb24_upload,
};

/*
 * RGBA / BGRA 8888
 */

static void
clutter_gst_rgb32_upload (ClutterGstVideoSink *sink,
                          GstBuffer           *buffer)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  CoglPixelFormat format;
  CoglHandle tex;
  GstMapInfo info;

  gst_buffer_map (buffer, &info, GST_MAP_READ);

  if (priv->bgr)
    format = COGL_PIXEL_FORMAT_BGRA_8888;
  else
    format = COGL_PIXEL_FORMAT_RGBA_8888;

  tex = cogl_texture_new_from_data (priv->width,
                                    priv->height,
                                    CLUTTER_GST_TEXTURE_FLAGS,
                                    format,
                                    format,
                                    GST_ROUND_UP_4 (4 * priv->width),
                                    info.data);

  gst_buffer_unmap (buffer, &info);

  _create_paint_material (sink,
                          tex,
                          COGL_INVALID_HANDLE,
                          COGL_INVALID_HANDLE);
}

static ClutterGstRenderer rgb32_renderer =
{
  "RGB 32",
  CLUTTER_GST_RGB32,
  0,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE("{ RGBA, BGRA }")),
  clutter_gst_rgb_init,
  clutter_gst_dummy_deinit,
  clutter_gst_rgb32_upload,
};

/*
 * YV12
 *
 * 8 bit Y plane followed by 8 bit 2x2 subsampled U and V planes.
 */

static void
clutter_gst_yv12_upload (ClutterGstVideoSink *sink,
                         GstBuffer           *buffer)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  gint y_row_stride  = GST_ROUND_UP_4 (priv->width);
  gint uv_row_stride = GST_ROUND_UP_4 (priv->width / 2);
  CoglHandle y_tex, u_tex, v_tex;
  GstMapInfo info;

  gst_buffer_map (buffer, &info, GST_MAP_READ);

  y_tex = cogl_texture_new_from_data (priv->width,
                                      priv->height,
                                      CLUTTER_GST_TEXTURE_FLAGS,
                                      COGL_PIXEL_FORMAT_G_8,
                                      COGL_PIXEL_FORMAT_G_8,
                                      y_row_stride,
                                      info.data);

  u_tex = cogl_texture_new_from_data (priv->width / 2,
                                      priv->height / 2,
                                      CLUTTER_GST_TEXTURE_FLAGS,
                                      COGL_PIXEL_FORMAT_G_8,
                                      COGL_PIXEL_FORMAT_G_8,
                                      uv_row_stride,
                                      info.data +
                                      (y_row_stride * priv->height));

  v_tex = cogl_texture_new_from_data (priv->width / 2,
                                      priv->height / 2,
                                      CLUTTER_GST_TEXTURE_FLAGS,
                                      COGL_PIXEL_FORMAT_G_8,
                                      COGL_PIXEL_FORMAT_G_8,
                                      uv_row_stride,
                                      info.data
                                      + (y_row_stride * priv->height)
                                      + (uv_row_stride * priv->height / 2));

  gst_buffer_unmap (buffer, &info);

  _create_paint_material (sink, y_tex, u_tex, v_tex);
}

static void
clutter_gst_yv12_glsl_init (ClutterGstVideoSink *sink)
{
  _create_template_material (sink, yv12_to_rgba_shader, TRUE, 3);
}


static ClutterGstRenderer yv12_glsl_renderer =
{
  "YV12 glsl",
  CLUTTER_GST_YV12,
  CLUTTER_GST_GLSL | CLUTTER_GST_MULTI_TEXTURE,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("YV12")),
  clutter_gst_yv12_glsl_init,
  clutter_gst_dummy_deinit,
  clutter_gst_yv12_upload,
};

/*
 * YV12 (fragment program version)
 *
 * 8 bit Y plane followed by 8 bit 2x2 subsampled V and U planes.
 */

#ifdef CLUTTER_COGL_HAS_GL
static void
clutter_gst_yv12_fp_init (ClutterGstVideoSink *sink)
{
  char *shader = g_malloc (YV12_FP_SZ + 1);
  _string_array_to_char_array (shader, YV12_fp);

  _create_template_material (sink, shader, FALSE, 3);

  g_free (shader);
}

static ClutterGstRenderer yv12_fp_renderer =
{
  "YV12 fp",
  CLUTTER_GST_YV12,
  CLUTTER_GST_FP | CLUTTER_GST_MULTI_TEXTURE,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("YV12")),
  clutter_gst_yv12_fp_init,
  clutter_gst_dummy_deinit,
  clutter_gst_yv12_upload,
};
#endif

/*
 * I420
 *
 * 8 bit Y plane followed by 8 bit 2x2 subsampled U and V planes.
 * Basically the same as YV12, but with the 2 chroma planes switched.
 */

static void
clutter_gst_i420_glsl_init (ClutterGstVideoSink *sink)
{
  _create_template_material (sink, yv12_to_rgba_shader, TRUE, 3);
}

static ClutterGstRenderer i420_glsl_renderer =
{
  "I420 glsl",
  CLUTTER_GST_I420,
  CLUTTER_GST_GLSL | CLUTTER_GST_MULTI_TEXTURE,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("I420")),
  clutter_gst_i420_glsl_init,
  clutter_gst_dummy_deinit,
  clutter_gst_yv12_upload,
};

/*
 * I420 (fragment program version)
 *
 * 8 bit Y plane followed by 8 bit 2x2 subsampled U and V planes.
 * Basically the same as YV12, but with the 2 chroma planes switched.
 */

#ifdef CLUTTER_COGL_HAS_GL
static void
clutter_gst_i420_fp_init (ClutterGstVideoSink *sink)
{
  char *shader = g_malloc(I420_FP_SZ + 1);
  _string_array_to_char_array (shader, I420_fp);

  _create_template_material (sink, shader, FALSE, 3);

  g_free (shader);
}

static ClutterGstRenderer i420_fp_renderer =
{
  "I420 fp",
  CLUTTER_GST_I420,
  CLUTTER_GST_FP | CLUTTER_GST_MULTI_TEXTURE,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("I420")),
  clutter_gst_i420_fp_init,
  clutter_gst_dummy_deinit,
  clutter_gst_yv12_upload,
};
#endif

/*
 * AYUV
 *
 * This is a 4:4:4 YUV format with 8 bit samples for each component along
 * with an 8 bit alpha blend value per pixel. Component ordering is A Y U V
 * (as the name suggests).
 */

static void
clutter_gst_ayuv_glsl_init(ClutterGstVideoSink *sink)
{
  _create_template_material (sink, ayuv_to_rgba_shader, TRUE, 1);
}

static void
clutter_gst_ayuv_upload (ClutterGstVideoSink *sink,
                         GstBuffer           *buffer)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  CoglHandle tex;
  GstMapInfo info;

  gst_buffer_map (buffer, &info, GST_MAP_READ);

  tex =
    cogl_texture_new_from_data (priv->width,
                                priv->height,
                                CLUTTER_GST_TEXTURE_FLAGS,
                                COGL_PIXEL_FORMAT_RGBA_8888,
                                COGL_PIXEL_FORMAT_RGBA_8888,
                                GST_ROUND_UP_4 (4 * priv->width),
                                info.data);

  gst_buffer_unmap (buffer, &info);

  _create_paint_material (sink,
                          tex,
                          COGL_INVALID_HANDLE,
                          COGL_INVALID_HANDLE);
}

static ClutterGstRenderer ayuv_glsl_renderer =
{
  "AYUV glsl",
  CLUTTER_GST_AYUV,
  CLUTTER_GST_GLSL,
  GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("AYUV")),
  clutter_gst_ayuv_glsl_init,
  clutter_gst_dummy_deinit,
  clutter_gst_ayuv_upload,
};

/*
 * HW Surfaces
 */

#ifdef HAVE_HW_DECODER_SUPPORT
static void
clutter_gst_hw_init (ClutterGstVideoSink *sink)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  CoglHandle tex;
  CoglHandle material;

  /* Default texture is 1x1, let's replace it with one big enough. */
  tex = cogl_texture_new_with_size (priv->width, priv->height,
                                    CLUTTER_GST_TEXTURE_FLAGS,
                                    COGL_PIXEL_FORMAT_BGRA_8888);

  material = cogl_material_new ();
  cogl_material_set_layer (material, 0, tex);
  clutter_texture_set_cogl_material (priv->texture, material);

  cogl_object_unref (tex);
  cogl_object_unref (material);
}

static void
clutter_gst_hw_deinit (ClutterGstVideoSink *sink)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;

  if (priv->converter != NULL)
    g_object_unref (priv->converter);
  priv->converter = NULL;
}

static void
clutter_gst_hw_upload (ClutterGstVideoSink *sink,
                       GstBuffer           *buffer)
{
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  GstSurfaceMeta *surface = gst_buffer_get_surface_meta (buffer);

  g_return_if_fail (surface != NULL);

  if (G_UNLIKELY (priv->converter == NULL)) {
    CoglHandle tex;
    GLuint gl_texture;
    GLenum gl_target;
    GValue value = {0};

    tex = clutter_texture_get_cogl_texture (priv->texture);
    cogl_texture_get_gl_texture (tex, &gl_texture, &gl_target);
    g_return_if_fail (gl_target == GL_TEXTURE_2D);

    g_value_init (&value, G_TYPE_UINT);
    g_value_set_uint (&value, gl_texture);

    priv->converter = gst_surface_meta_create_converter (surface, "opengl", &value);
    g_return_if_fail (priv->converter);
  }

  gst_surface_converter_upload (priv->converter, buffer);

  /* The texture is dirty, schedule a redraw */
  clutter_actor_queue_redraw (CLUTTER_ACTOR (priv->texture));
}

static ClutterGstRenderer hw_renderer =
{
  "HW surface",
  CLUTTER_GST_SURFACE,
  0,
  GST_STATIC_CAPS ("x-video/surface, opengl=true"),
  clutter_gst_hw_init,
  clutter_gst_hw_deinit,
  clutter_gst_hw_upload,
};
#endif

static GSList *
clutter_gst_build_renderers_list (void)
{
  GSList             *list = NULL;
  gint                nb_texture_units = 0;
  gint                features = 0, i;
  /* The order of the list of renderers is important. They will be prepended
   * to a GSList and we'll iterate over that list to choose the first matching
   * renderer. Thus if you want to use the fp renderer over the glsl one, the
   * fp renderer has to be put after the glsl one in this array */
  ClutterGstRenderer *renderers[] =
    {
      &rgb24_renderer,
      &rgb32_renderer,
      &yv12_glsl_renderer,
      &i420_glsl_renderer,
#ifdef CLUTTER_COGL_HAS_GL
      &yv12_fp_renderer,
      &i420_fp_renderer,
#endif
      &ayuv_glsl_renderer,
#ifdef HAVE_HW_DECODER_SUPPORT
      &hw_renderer,
#endif
      NULL
    };

  nb_texture_units = get_n_fragment_texture_units();

  if (nb_texture_units >= 3)
    features |= CLUTTER_GST_MULTI_TEXTURE;

#ifdef CLUTTER_COGL_HAS_GL
  if (cogl_features_available (COGL_FEATURE_SHADERS_ARBFP))
    features |= CLUTTER_GST_FP;
#endif

  if (cogl_features_available (COGL_FEATURE_SHADERS_GLSL))
    features |= CLUTTER_GST_GLSL;

  GST_INFO ("GL features: 0x%08x", features);

  for (i = 0; renderers[i]; i++)
    {
      gint needed = renderers[i]->flags;

      if ((needed & features) == needed)
        list = g_slist_prepend (list, renderers[i]);
    }

  return list;
}

static void
append_cap (gpointer data, gpointer user_data)
{
  ClutterGstRenderer *renderer = (ClutterGstRenderer *)data;
  GstCaps *caps = (GstCaps *)user_data;
  GstCaps *writable_caps;

  writable_caps =
    gst_caps_make_writable (gst_static_caps_get (&renderer->caps));
  gst_caps_append (caps, writable_caps);
}

static GstCaps *
clutter_gst_build_caps (GSList *renderers)
{
  GstCaps *caps;

  caps = gst_caps_new_empty ();

  g_slist_foreach (renderers, append_cap, caps);

  return caps;
}


static gboolean
navigation_event (ClutterActor        *actor,
                  ClutterEvent        *event,
                  ClutterGstVideoSink *sink)
{
  if (event->type == CLUTTER_MOTION)
    {
      ClutterMotionEvent *mevent = (ClutterMotionEvent *) event;

      GST_DEBUG ("Received mouse move event to %d,%d", mevent->x, mevent->y);
      gst_navigation_send_mouse_event (GST_NAVIGATION (sink),
                                       "mouse-move", 0, mevent->x, mevent->y);
    }
  else if (event->type == CLUTTER_BUTTON_PRESS ||
           event->type == CLUTTER_BUTTON_RELEASE)
    {
      ClutterButtonEvent *bevent = (ClutterButtonEvent *) event;
      const char *type;

      GST_DEBUG ("Received button %s event at %d,%d",
                 (event->type == CLUTTER_BUTTON_PRESS) ? "press" : "release",
                 bevent->x, bevent->y);
      type = (event->type == CLUTTER_BUTTON_PRESS) ? "mouse-button-press" : "mouse-button-release";
      gst_navigation_send_mouse_event (GST_NAVIGATION (sink),
                                       type, bevent->button, bevent->x, bevent->y);
    }
  else if (event->type == CLUTTER_KEY_PRESS)
    {
      ClutterKeyEvent *kevent = (ClutterKeyEvent *) event;
      GstNavigationCommand command;

      switch (kevent->keyval)
        {
        case CLUTTER_KEY_Up:
          command = GST_NAVIGATION_COMMAND_UP;
          break;
        case CLUTTER_KEY_Down:
          command = GST_NAVIGATION_COMMAND_DOWN;
          break;
        case CLUTTER_KEY_Left:
          command = GST_NAVIGATION_COMMAND_LEFT;
          break;
        case CLUTTER_KEY_Right:
          command = GST_NAVIGATION_COMMAND_RIGHT;
          break;
        case CLUTTER_KEY_Return:
          command = GST_NAVIGATION_COMMAND_ACTIVATE;
          break;
        default:
          command = GST_NAVIGATION_COMMAND_INVALID;
        }

      if (command != GST_NAVIGATION_COMMAND_INVALID)
        {
          gst_navigation_send_command (GST_NAVIGATION (sink), command);

          return TRUE;
        }
    }

  return FALSE;
}

static void
clutter_gst_video_sink_init (ClutterGstVideoSink      *sink)
{
  ClutterGstVideoSinkPrivate *priv;

  sink->priv = priv =
    G_TYPE_INSTANCE_GET_PRIVATE (sink, CLUTTER_GST_TYPE_VIDEO_SINK,
                                 ClutterGstVideoSinkPrivate);

  /* We are saving the GMainContext of the caller thread (which has to be
   * the clutter thread)  */
  priv->clutter_main_context = g_main_context_default ();

  priv->renderers = clutter_gst_build_renderers_list ();
  priv->caps = clutter_gst_build_caps (priv->renderers);

  priv->signal_handler_ids = g_array_new (FALSE, TRUE, sizeof (gulong));
  priv->priority = CLUTTER_GST_DEFAULT_PRIORITY;
}

static GstFlowReturn
clutter_gst_video_sink_render (GstBaseSink *bsink,
                               GstBuffer   *buffer)
{
  ClutterGstVideoSink *sink = CLUTTER_GST_VIDEO_SINK (bsink);
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  ClutterGstSource *gst_source = priv->source;


  g_mutex_lock (&gst_source->buffer_lock);

  if (gst_source->stage_lost)
    {
      GST_ELEMENT_ERROR (bsink, RESOURCE, CLOSE,
          ("The window has been closed."),
          ("The window has been closed."));
      g_mutex_unlock (&gst_source->buffer_lock);
      return GST_FLOW_ERROR;
    }

  if (gst_source->buffer)
    gst_buffer_unref (gst_source->buffer);
  gst_source->buffer = gst_buffer_ref (buffer);

  g_mutex_unlock (&gst_source->buffer_lock);

  g_main_context_wakeup (priv->clutter_main_context);

  return GST_FLOW_OK;
}

static GstCaps *
clutter_gst_video_sink_get_caps (GstBaseSink *bsink, GstCaps *filter)
{
  ClutterGstVideoSink *sink;

  sink = CLUTTER_GST_VIDEO_SINK (bsink);
  return gst_caps_ref (sink->priv->caps);
}

static gboolean
clutter_gst_video_sink_set_caps (GstBaseSink *bsink,
                                 GstCaps     *caps)
{
  ClutterGstVideoSink        *sink;
  ClutterGstVideoSinkPrivate *priv;

  sink = CLUTTER_GST_VIDEO_SINK(bsink);
  priv = sink->priv;

  if (!clutter_gst_parse_caps (caps, sink, FALSE))
    return FALSE;

  g_mutex_lock (&priv->source->buffer_lock);
  priv->source->has_new_caps = TRUE;
  g_mutex_unlock (&priv->source->buffer_lock);

  return TRUE;
}


static void
clutter_gst_video_sink_dispose (GObject *object)
{
  ClutterGstVideoSink *self;
  ClutterGstVideoSinkPrivate *priv;

  self = CLUTTER_GST_VIDEO_SINK (object);
  priv = self->priv;

  if (priv->renderer)
    {
      priv->renderer->deinit (self);
      priv->renderer = NULL;
    }

  if (priv->texture)
    clutter_gst_video_sink_set_texture (self, NULL);

  if (priv->caps)
    {
      gst_caps_unref (priv->caps);
      priv->caps = NULL;
    }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clutter_gst_video_sink_finalize (GObject *object)
{
  ClutterGstVideoSink *self;
  ClutterGstVideoSinkPrivate *priv;

  self = CLUTTER_GST_VIDEO_SINK (object);
  priv = self->priv;

  g_slist_free (priv->renderers);

  g_array_free (priv->signal_handler_ids, TRUE);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clutter_gst_video_sink_set_texture (ClutterGstVideoSink *sink,
                                    ClutterTexture      *texture)
{
  const char const *events[] = {
    "key-press-event",
    "key-release-event",
    "button-press-event",
    "button-release-event",
    "motion-event"
  };
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  guint i;

  if (priv->texture)
    {
      for (i = 0; i < priv->signal_handler_ids->len; i++)
        {
          gulong id = g_array_index (priv->signal_handler_ids, gulong, i);
          g_signal_handler_disconnect (priv->texture, id);
        }
      g_array_set_size (priv->signal_handler_ids, 0);
    }

  priv->texture = texture;
  if (priv->texture == NULL)
    return;

  clutter_actor_set_reactive (CLUTTER_ACTOR (priv->texture), TRUE);
  g_object_add_weak_pointer (G_OBJECT (priv->texture), (gpointer *) &(priv->texture));

  for (i = 0; i < G_N_ELEMENTS (events); i++)
    {
      gulong id;
      id = g_signal_connect (priv->texture, events[i],
                             G_CALLBACK (navigation_event), sink);
      g_array_append_val (priv->signal_handler_ids, id);
    }
}

static void
clutter_gst_video_sink_set_property (GObject *object,
                                     guint prop_id,
                                     const GValue *value,
                                     GParamSpec *pspec)
{
  ClutterGstVideoSink *sink = CLUTTER_GST_VIDEO_SINK (object);

  switch (prop_id)
    {
    case PROP_TEXTURE:
      clutter_gst_video_sink_set_texture (sink, g_value_get_object (value));
      break;
    case PROP_UPDATE_PRIORITY:
      clutter_gst_video_sink_set_priority (sink, g_value_get_int (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
clutter_gst_video_sink_get_property (GObject *object,
                                     guint prop_id,
                                     GValue *value,
                                     GParamSpec *pspec)
{
  ClutterGstVideoSink *sink = CLUTTER_GST_VIDEO_SINK (object);
  ClutterGstVideoSinkPrivate *priv = sink->priv;

  switch (prop_id)
    {
    case PROP_TEXTURE:
      g_value_set_object (value, priv->texture);
      break;
    case PROP_UPDATE_PRIORITY:
      g_value_set_int (value, priv->priority);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
clutter_gst_video_sink_start (GstBaseSink *base_sink)
{
  ClutterGstVideoSink        *sink = CLUTTER_GST_VIDEO_SINK (base_sink);
  ClutterGstVideoSinkPrivate *priv = sink->priv;

  priv->source = clutter_gst_source_new (sink);
  g_source_attach ((GSource *) priv->source, priv->clutter_main_context);

  return TRUE;
}

static gboolean
clutter_gst_video_sink_stop (GstBaseSink *base_sink)
{
  ClutterGstVideoSink        *sink = CLUTTER_GST_VIDEO_SINK (base_sink);
  ClutterGstVideoSinkPrivate *priv = sink->priv;

  if (priv->source)
    {
      GSource *source = (GSource *) priv->source;

      g_source_destroy (source);
      g_source_unref (source);
      priv->source = NULL;
    }

  return TRUE;
}

static void
clutter_gst_video_sink_class_init (ClutterGstVideoSinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbase_sink_class = GST_BASE_SINK_CLASS (klass);
  GParamSpec *pspec;

  GST_DEBUG_CATEGORY_INIT (clutter_gst_video_sink_debug,
			   "cluttersink",
			   0,
			   "clutter video sink");

  g_type_class_add_private (klass, sizeof (ClutterGstVideoSinkPrivate));

  gobject_class->set_property = clutter_gst_video_sink_set_property;
  gobject_class->get_property = clutter_gst_video_sink_get_property;

  gobject_class->dispose = clutter_gst_video_sink_dispose;
  gobject_class->finalize = clutter_gst_video_sink_finalize;

  gst_element_class_add_pad_template (gstelement_class,
                      gst_static_pad_template_get (&sinktemplate_all));

  gst_element_class_set_details_simple (gstelement_class,
      "Clutter video sink",
      "Sink/Video",
      "Sends video data from a GStreamer pipeline to a Clutter texture",
      "Jonathan Matthew <jonathan@kaolin.wh9.net>, "
      "Matthew Allum <mallum@o-hand.com, "
      "Chris Lord <chris@o-hand.com>");

  gstbase_sink_class->render = clutter_gst_video_sink_render;
  gstbase_sink_class->preroll = clutter_gst_video_sink_render;
  gstbase_sink_class->start = clutter_gst_video_sink_start;
  gstbase_sink_class->stop = clutter_gst_video_sink_stop;
  gstbase_sink_class->set_caps = clutter_gst_video_sink_set_caps;
  gstbase_sink_class->get_caps = clutter_gst_video_sink_get_caps;

  /**
   * ClutterGstVideoSink:texture:
   *
   * This is the texture the video is decoded into. It can be any
   * #ClutterTexture, however Cluter-Gst has a handy subclass,
   * #ClutterGstVideoTexture, that implements the #ClutterMedia
   * interface.
   */
  pspec = g_param_spec_object ("texture",
                               "Texture",
                               "Texture the video will be decoded into",
                               CLUTTER_TYPE_TEXTURE,
                               CLUTTER_GST_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_TEXTURE, pspec);

  /**
   * ClutterGstVideoSink:update-priority:
   *
   * Clutter-Gst installs a #GSource to signal that a new frame is ready to
   * the Clutter thread. This property allows to tweak the priority of the
   * source (Lower value is higher priority).
   *
   * Since 1.0
   */
  pspec = g_param_spec_int ("update-priority",
                            "Update Priority",
                            "Priority of video updates in the Clutter thread",
                            -G_MAXINT, G_MAXINT,
                            CLUTTER_GST_DEFAULT_PRIORITY,
                            CLUTTER_GST_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_UPDATE_PRIORITY, pspec);
}

static void
clutter_gst_navigation_send_event (GstNavigation *navigation,
                                   GstStructure  *structure)
{
  ClutterGstVideoSink *sink = CLUTTER_GST_VIDEO_SINK (navigation);
  ClutterGstVideoSinkPrivate *priv = sink->priv;
  GstEvent *event;
  GstPad *pad = NULL;
  gdouble x, y;
  gfloat x_out, y_out;

  /* Converting pointer coordinates to the non scaled geometry
   * if the structure contains pointer coordinates */
  if (gst_structure_get_double (structure, "pointer_x", &x) &&
      gst_structure_get_double (structure, "pointer_y", &y))
    {
      if (clutter_actor_transform_stage_point (CLUTTER_ACTOR (priv->texture), x, y, &x_out, &y_out) == FALSE)
        {
          g_warning ("Failed to convert non-scaled coordinates for video-sink");
          return;
        }

      x = x_out * priv->width / clutter_actor_get_width (CLUTTER_ACTOR (priv->texture));
      y = y_out * priv->height / clutter_actor_get_height (CLUTTER_ACTOR (priv->texture));

      gst_structure_set (structure,
                         "pointer_x", G_TYPE_DOUBLE, (gdouble) x,
                         "pointer_y", G_TYPE_DOUBLE, (gdouble) y,
                         NULL);
    }

  event = gst_event_new_navigation (structure);

  pad = gst_pad_get_peer (GST_VIDEO_SINK_PAD (sink));

  if (GST_IS_PAD (pad) && GST_IS_EVENT (event))
    {
      gst_pad_send_event (pad, event);

      gst_object_unref (pad);
    }
}

static void
clutter_gst_navigation_interface_init (GstNavigationInterface *iface)
{
  iface->send_event = clutter_gst_navigation_send_event;
}

gboolean
_internal_plugin_init (GstPlugin *plugin)
{
  gboolean ret = gst_element_register (plugin,
                                             "cluttersink",
                                       GST_RANK_PRIMARY,
                                       CLUTTER_GST_TYPE_VIDEO_SINK);

  GST_DEBUG_CATEGORY_INIT (clutter_gst_video_sink_debug,
			   "cluttersink",
			   0,
			   "clutter video sink");

  return ret;
}
