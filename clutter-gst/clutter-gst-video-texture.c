/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * clutter-gst-video-texture.c - ClutterTexture using GStreamer to display a
 *                               video stream.
 *
 * Authored By Matthew Allum     <mallum@openedhand.com>
 *             Damien Lespiau    <damien.lespiau@intel.com>
 *             Lionel Landwerlin <lionel.g.landwerlin@linux.intel.com>
 *
 * Copyright (C) 2006 OpenedHand
 * Copyright (C) 2010, 2011 Intel Corporation
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
 * SECTION:clutter-gst-video-texture
 * @short_description: Actor for playback of video files.
 *
 * #ClutterGstVideoTexture is a #ClutterTexture that plays video files.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>
#include <gio/gio.h>
#include <gst/base/gstbasesink.h>
#include <gst/video/video.h>

#include "clutter-gst-debug.h"
#include "clutter-gst-enum-types.h"
#include "clutter-gst-marshal.h"
#include "clutter-gst-player.h"
#include "clutter-gst-private.h"
#include "clutter-gst-video-texture.h"

struct _ClutterGstVideoTexturePrivate
{
  /* width / height (in pixels) of the frame data before applying the pixel
   * aspect ratio */
  gint buffer_width;
  gint buffer_height;

  /* Pixel aspect ration is par_n / par_d. this is set by the sink */
  guint par_n, par_d;

  /* natural width / height (in pixels) of the texture (after par applied) */
  guint texture_width;
  guint texture_height;

  CoglHandle idle_material;
  CoglColor idle_color_unpre;
};

enum {
  PROP_0 = 32,  /* Avoid overlap with player properties */

  PROP_IDLE_MATERIAL,
  PROP_PAR
};

static void clutter_gst_video_texture_media_init (ClutterMediaIface *iface);
static void clutter_gst_video_texture_player_init (ClutterGstPlayerIface *iface);

G_DEFINE_TYPE_WITH_CODE (ClutterGstVideoTexture,
                         clutter_gst_video_texture,
                         CLUTTER_TYPE_TEXTURE,
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_MEDIA,
                                                clutter_gst_video_texture_media_init)
                         G_IMPLEMENT_INTERFACE (CLUTTER_GST_TYPE_PLAYER,
                                                clutter_gst_video_texture_player_init));

/* Clutter 1.4 has this symbol, we don't want to depend on 1.4 just for that
 * just yet */
static void
_cogl_color_unpremultiply (CoglColor *color)
{
  gfloat alpha;

  alpha = cogl_color_get_alpha (color);

  if (alpha != 0)
    {
      gfloat red, green, blue;

      red = cogl_color_get_red (color);
      green = cogl_color_get_green (color);
      blue = cogl_color_get_blue (color);

      red = red / alpha;
      green = green / alpha;
      blue = blue / alpha;

      cogl_color_set_from_4f (color, red, green, blue, alpha);
    }
}

/* Clutter 1.4 has this symbol, we don't want to depend on 1.4 just for that
 * just yet */
static void
_cogl_color_set_alpha_byte (CoglColor     *color,
                            unsigned char  alpha)
{
  unsigned char red, green, blue;

  red = cogl_color_get_red_byte (color);
  green = cogl_color_get_green_byte (color);
  blue = cogl_color_get_blue_byte (color);

  cogl_color_set_from_4ub (color, red, green, blue, alpha);
}

static void
gen_texcoords_and_draw_cogl_rectangle (ClutterActor *self)
{
  ClutterActorBox box;

  clutter_actor_get_allocation_box (self, &box);

  cogl_rectangle_with_texture_coords (0, 0,
                                      box.x2 - box.x1,
                                      box.y2 - box.y1,
                                      0, 0, 1.0, 1.0);
}

static void
create_black_idle_material (ClutterGstVideoTexture *video_texture)
{
  ClutterGstVideoTexturePrivate *priv = video_texture->priv;

  priv->idle_material = cogl_material_new ();
  cogl_color_set_from_4ub (&priv->idle_color_unpre, 0, 0, 0, 0xff);
  cogl_material_set_color (priv->idle_material, &priv->idle_color_unpre);
}

static void
clutter_gst_video_texture_media_init (ClutterMediaIface *iface)
{
}

static void
clutter_gst_video_texture_player_init (ClutterGstPlayerIface *iface)
{
}

static void
clutter_gst_video_texture_size_change (ClutterTexture *texture,
                                       gint            width,
                                       gint            height)
{
  ClutterGstVideoTexture *video_texture = CLUTTER_GST_VIDEO_TEXTURE (texture);
  ClutterGstVideoTexturePrivate *priv = video_texture->priv;
  gboolean changed;

  /* we are being told the actual (as in number of pixels in the buffers)
   * frame size. Store the values to be used in preferred_width/height() */
  changed = (priv->buffer_width != width) || (priv->buffer_height != height);
  priv->buffer_width = width;
  priv->buffer_height = height;

  if (changed)
    {
      /* reset the computed texture dimensions if the underlying frames have
       * changed size */
      CLUTTER_GST_NOTE (ASPECT_RATIO, "frame size has been updated to %dx%d",
                        width, height);

      priv->texture_width = priv->texture_height = 0;

      /* queue a relayout to ask containers/layout manager to ask for
       * the preferred size again */
      clutter_actor_queue_relayout (CLUTTER_ACTOR (texture));
    }
}

/*
 * Clutter actor implementation
 */

static void
clutter_gst_video_texture_get_natural_size (ClutterGstVideoTexture *texture,
                                            gfloat                 *width,
                                            gfloat                 *height)
{
  ClutterGstVideoTexturePrivate *priv = texture->priv;
  guint dar_n, dar_d;
  gboolean ret;

  /* we cache texture_width and texture_height */

  if (G_UNLIKELY (priv->buffer_width == 0 || priv->buffer_height == 0))
    {
      /* we don't know the size of the frames yet default to 0,0 */
      priv->texture_width = 0;
      priv->texture_height = 0;
    }
  else if (G_UNLIKELY (priv->texture_width == 0 || priv->texture_height == 0))
    {
      CLUTTER_GST_NOTE (ASPECT_RATIO, "frame is %dx%d with par %d/%d",
                        priv->buffer_width, priv->buffer_height,
                        priv->par_n, priv->par_d);

      ret = gst_video_calculate_display_ratio (&dar_n, &dar_d,
                                               priv->buffer_width,
                                               priv->buffer_height,
                                               priv->par_n, priv->par_d,
                                               1, 1);
      if (ret == FALSE)
        dar_n = dar_d = 1;

      if (priv->buffer_height % dar_d == 0)
        {
          priv->texture_width = gst_util_uint64_scale (priv->buffer_height,
                                                       dar_n, dar_d);
          priv->texture_height = priv->buffer_height;
        }
      else if (priv->buffer_width % dar_n == 0)
        {
          priv->texture_width = priv->buffer_width;
          priv->texture_height = gst_util_uint64_scale (priv->buffer_width,
                                                        dar_d, dar_n);

        }
      else
        {
          priv->texture_width = gst_util_uint64_scale (priv->buffer_height,
                                                       dar_n, dar_d);
          priv->texture_height = priv->buffer_height;
        }

      CLUTTER_GST_NOTE (ASPECT_RATIO,
                        "final size is %dx%d (calculated par is %d/%d)",
                        priv->texture_width, priv->texture_height,
                        dar_n, dar_d);
    }

  if (width)
    *width = (gfloat)priv->texture_width;

  if (height)
    *height = (gfloat)priv->texture_height;
}

static void
clutter_gst_video_texture_get_preferred_width (ClutterActor *self,
                                               gfloat        for_height,
                                               gfloat       *min_width_p,
                                               gfloat       *natural_width_p)
{
  ClutterGstVideoTexture *texture = CLUTTER_GST_VIDEO_TEXTURE (self);
  ClutterGstVideoTexturePrivate *priv = texture->priv;
  gboolean sync_size, keep_aspect_ratio;
  gfloat natural_width, natural_height;

  /* Min request is always 0 since we can scale down or clip */
  if (min_width_p)
    *min_width_p = 0;

  sync_size = clutter_texture_get_sync_size (CLUTTER_TEXTURE (self));
  keep_aspect_ratio =
    clutter_texture_get_keep_aspect_ratio (CLUTTER_TEXTURE (self));

  clutter_gst_video_texture_get_natural_size (texture,
                                              &natural_width,
                                              &natural_height);

  if (sync_size)
    {
      if (natural_width_p)
        {
          if (!keep_aspect_ratio ||
              for_height < 0 ||
              priv->buffer_height <= 0)
            {
              *natural_width_p = natural_width;
            }
          else
            {
              /* Set the natural width so as to preserve the aspect ratio */
              gfloat ratio =  natural_width /  natural_height;

              *natural_width_p = ratio * for_height;
            }
        }
    }
  else
    {
      if (natural_width_p)
        *natural_width_p = 0;
    }
}

static void
clutter_gst_video_texture_get_preferred_height (ClutterActor *self,
                                                gfloat        for_width,
                                                gfloat       *min_height_p,
                                                gfloat       *natural_height_p)
{
  ClutterGstVideoTexture *texture = CLUTTER_GST_VIDEO_TEXTURE (self);
  ClutterGstVideoTexturePrivate *priv = texture->priv;
  gboolean sync_size, keep_aspect_ratio;
  gfloat natural_width, natural_height;

  /* Min request is always 0 since we can scale down or clip */
  if (min_height_p)
    *min_height_p = 0;

  sync_size = clutter_texture_get_sync_size (CLUTTER_TEXTURE (self));
  keep_aspect_ratio =
    clutter_texture_get_keep_aspect_ratio (CLUTTER_TEXTURE (self));

  clutter_gst_video_texture_get_natural_size (texture,
                                              &natural_width,
                                              &natural_height);

  if (sync_size)
    {
      if (natural_height_p)
        {
          if (!keep_aspect_ratio ||
              for_width < 0 ||
              priv->buffer_width <= 0)
            {
              *natural_height_p = natural_height;
            }
          else
            {
              /* Set the natural height so as to preserve the aspect ratio */
              gfloat ratio = natural_height / natural_width;

              *natural_height_p = ratio * for_width;
            }
        }
    }
  else
    {
      if (natural_height_p)
        *natural_height_p = 0;
    }
}

/*
 * ClutterTexture unconditionnaly sets the material color to:
 *    (opacity,opacity,opacity,opacity)
 * so we can't set a black material to the texture. Let's override paint()
 * for now.
 */
static void
clutter_gst_video_texture_paint (ClutterActor *actor)
{
  ClutterGstVideoTexture *video_texture = (ClutterGstVideoTexture *) actor;
  ClutterGstVideoTexturePrivate *priv = video_texture->priv;
  ClutterActorClass *actor_class;
  gboolean is_idle;

  is_idle = clutter_gst_player_get_idle (CLUTTER_GST_PLAYER (video_texture));
  if (G_UNLIKELY (is_idle))
    {
      CoglColor *color;
      gfloat alpha;

      /* blend the alpha of the idle material with the actor's opacity */
      color = cogl_color_copy (&priv->idle_color_unpre);
      alpha = clutter_actor_get_paint_opacity (actor) *
              cogl_color_get_alpha_byte (color) / 0xff;
      _cogl_color_set_alpha_byte (color, alpha);
      cogl_color_premultiply (color);
      cogl_material_set_color (priv->idle_material, color);

      cogl_set_source (priv->idle_material);

      /* draw */
      gen_texcoords_and_draw_cogl_rectangle (actor);
    }
  else
    {
      /* when not idle, just chain up to ClutterTexture::paint() */
      actor_class =
        CLUTTER_ACTOR_CLASS (clutter_gst_video_texture_parent_class);
      actor_class->paint (actor);
    }

}

/*
 * GObject implementation
 */

static void
clutter_gst_video_texture_dispose (GObject *object)
{
  ClutterGstVideoTexture *self = CLUTTER_GST_VIDEO_TEXTURE (object);

  clutter_gst_player_deinit (CLUTTER_GST_PLAYER (self));

  G_OBJECT_CLASS (clutter_gst_video_texture_parent_class)->dispose (object);
}

static void
clutter_gst_video_texture_finalize (GObject *object)
{
  ClutterGstVideoTexture        *self;
  ClutterGstVideoTexturePrivate *priv;

  self = CLUTTER_GST_VIDEO_TEXTURE (object);
  priv = self->priv;

  if (priv->idle_material != COGL_INVALID_HANDLE)
    cogl_handle_unref (priv->idle_material);

  G_OBJECT_CLASS (clutter_gst_video_texture_parent_class)->finalize (object);
}

static void
clutter_gst_video_texture_set_property (GObject      *object,
				        guint         property_id,
				        const GValue *value,
				        GParamSpec   *pspec)
{
  ClutterGstVideoTexture *video_texture = CLUTTER_GST_VIDEO_TEXTURE (object);
  ClutterGstVideoTexturePrivate *priv = video_texture->priv;

  switch (property_id)
    {
    case PROP_IDLE_MATERIAL:
      clutter_gst_video_texture_set_idle_material (video_texture,
                                                   g_value_get_boxed (value));
      break;
    case PROP_PAR:
      priv->par_n = gst_value_get_fraction_numerator (value);
      priv->par_d = gst_value_get_fraction_denominator (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
clutter_gst_video_texture_get_property (GObject    *object,
				        guint       property_id,
				        GValue     *value,
				        GParamSpec *pspec)
{
  ClutterGstVideoTexture *video_texture;
  ClutterGstVideoTexturePrivate *priv;

  video_texture = CLUTTER_GST_VIDEO_TEXTURE (object);
  priv = video_texture->priv;

  switch (property_id)
    {
    case PROP_IDLE_MATERIAL:
      g_value_set_boxed (value, priv->idle_material);
      break;
    case PROP_PAR:
      gst_value_set_fraction (value, priv->par_n, priv->par_d);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
clutter_gst_video_texture_class_init (ClutterGstVideoTextureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ClutterTextureClass *texture_class = CLUTTER_TEXTURE_CLASS (klass);
  GParamSpec *pspec;

  g_type_class_add_private (klass, sizeof (ClutterGstVideoTexturePrivate));

  object_class->dispose      = clutter_gst_video_texture_dispose;
  object_class->finalize     = clutter_gst_video_texture_finalize;
  object_class->set_property = clutter_gst_video_texture_set_property;
  object_class->get_property = clutter_gst_video_texture_get_property;

  actor_class->paint = clutter_gst_video_texture_paint;
  actor_class->get_preferred_width =
    clutter_gst_video_texture_get_preferred_width;
  actor_class->get_preferred_height =
    clutter_gst_video_texture_get_preferred_height;

  texture_class->size_change = clutter_gst_video_texture_size_change;

  pspec = g_param_spec_boxed ("idle-material",
                              "Idle material",
                              "Material to use for drawing when not playing",
                              COGL_TYPE_HANDLE,
                              CLUTTER_GST_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_IDLE_MATERIAL, pspec);

  pspec = gst_param_spec_fraction ("pixel-aspect-ratio",
                                   "Pixel Aspect Ratio",
                                   "Pixel aspect ratio of incoming frames",
                                   1, 100, 100, 1, 1, 1,
                                   CLUTTER_GST_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_PAR, pspec);


  clutter_gst_player_class_init (object_class);
}

static void
idle_cb (ClutterGstVideoTexture *video_texture,
         GParamSpec             *pspec,
         gpointer                data)
{
  /* restore the idle material so we don't just display the last frame */
  clutter_actor_queue_redraw (CLUTTER_ACTOR (video_texture));
}

static void
on_autocluttersink_element_added (GstBin                 *bin,
				  GstElement             *element,
				  ClutterGstVideoTexture *data)
{
  if (GST_IS_BASE_SINK (element))
    g_object_set (G_OBJECT (element), "qos", TRUE, NULL);
}

static gboolean
setup_pipeline (ClutterGstVideoTexture *video_texture)
{
  GstElement *pipeline, *video_sink;

  pipeline =
    clutter_gst_player_get_pipeline (CLUTTER_GST_PLAYER (video_texture));
  if (!pipeline)
    {
      g_critical ("Unable to get playbin2 element");
      return FALSE;
    }

  video_sink = gst_element_factory_make ("cluttersink", NULL);

  g_object_set (G_OBJECT (video_sink),
                "texture", CLUTTER_TEXTURE (video_texture),
		NULL);
  g_object_set (G_OBJECT (pipeline),
                "video-sink", video_sink,
                "subtitle-font-desc", "Sans 16",
                NULL);

  return TRUE;
}

static void
clutter_gst_video_texture_init (ClutterGstVideoTexture *video_texture)
{
  ClutterGstVideoTexturePrivate *priv;

  video_texture->priv = priv =
    G_TYPE_INSTANCE_GET_PRIVATE (video_texture,
                                 CLUTTER_GST_TYPE_VIDEO_TEXTURE,
                                 ClutterGstVideoTexturePrivate);

  if (!clutter_gst_player_init (CLUTTER_GST_PLAYER (video_texture)))
    {
      g_warning ("Failed to initiate suitable playback pipeline.");
      return;
    }

  if (!setup_pipeline (video_texture))
    {
      g_warning ("Failed to initiate suitable sinks for pipeline.");
      return;
    }

  create_black_idle_material (video_texture);

  priv->par_n = priv->par_d = 1;

  g_signal_connect (video_texture, "notify::idle",
                    G_CALLBACK (idle_cb),
                    NULL);
}

/*
 * Private symbols
 */

/**
 * clutter_gst_video_texture_new:
 *
 * Creates a video texture.
 *
 * <note>This function has to be called from Clutter's main thread. While
 * GStreamer will spawn threads to do its work, we want all the GL calls to
 * happen in the same thread. Clutter-gst knows which thread it is by
 * assuming this constructor is called from the Clutter thread.</note>
 *
 * Return value: the newly created video texture actor
 */
ClutterActor*
clutter_gst_video_texture_new (void)
{
  return g_object_new (CLUTTER_GST_TYPE_VIDEO_TEXTURE,
                       "disable-slicing", TRUE,
                       NULL);
}

/**
 * clutter_gst_video_texture_get_pipeline:
 * @texture: a #ClutterGstVideoTexture
 *
 * Retrieves the #GstPipeline used by the @texture, for direct use with
 * GStreamer API.
 *
 * Return value: (transfer none): the pipeline element used by the video texture
 */
GstElement *
clutter_gst_video_texture_get_pipeline (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_pipeline (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_get_idle_material:
 * @texture: a #ClutterGstVideoTexture
 *
 * Retrieves the material used to draw when no media is being played.
 *
 * Return value: (transfer none): the #CoglHandle of the idle material
 *
 * Since: 1.2
 */
CoglHandle
clutter_gst_video_texture_get_idle_material (ClutterGstVideoTexture *texture)
{
  g_return_val_if_fail (CLUTTER_GST_IS_VIDEO_TEXTURE (texture),
                        COGL_INVALID_HANDLE);

  return texture->priv->idle_material;
}
/**
 * clutter_gst_video_texture_set_idle_material:
 * @texture: a #ClutterGstVideoTexture
 * @material: the handle of a Cogl material
 *
 * Sets a material to use to draw when no media is being played. The
 * #ClutterGstVideoTexture holds a reference of the @material.
 *
 * The default idle material will paint the #ClutterGstVideoTexture in black.
 * If %COGL_INVALID_HANDLE is given as @material to this function, this
 * default idle material will be used.
 *
 * Since: 1.2
 */
void
clutter_gst_video_texture_set_idle_material (ClutterGstVideoTexture *texture,
                                             CoglHandle              material)
{
  ClutterGstVideoTexturePrivate *priv;

  g_return_if_fail (CLUTTER_GST_IS_VIDEO_TEXTURE (texture));

  priv = texture->priv;
  /* priv->idle_material always has a valid material */
  cogl_handle_unref (priv->idle_material);

  if (material != COGL_INVALID_HANDLE)
    {
      priv->idle_material = cogl_handle_ref (material);
      cogl_material_get_color (material, &priv->idle_color_unpre);
      _cogl_color_unpremultiply (&priv->idle_color_unpre);
    }
  else
    {
      create_black_idle_material (texture);
    }

  g_object_notify (G_OBJECT (texture), "idle-material");
}

/**
 * clutter_gst_video_texture_get_user_agent:
 * @texture: a #ClutterGstVideoTexture
 *
 * Retrieves the user agent used when streaming.
 *
 * Return value: the user agent used. The returned string has to be freed with
 * g_free()
 *
 * Since: 1.2
 */
gchar *
clutter_gst_video_texture_get_user_agent (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_user_agent (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_set_user_agent:
 * @texture: a #ClutterGstVideoTexture
 * @user_agent: the user agent
 *
 * Sets the user agent to use when streaming.
 *
 * When streaming content, you might want to set a custom user agent, eg. to
 * promote your software, make it appear in statistics or because the server
 * requires a special user agent you want to impersonate.
 *
 * Since: 1.2
 */
void
clutter_gst_video_texture_set_user_agent (ClutterGstVideoTexture *texture,
                                          const gchar *           user_agent)
{
  clutter_gst_player_set_user_agent (CLUTTER_GST_PLAYER (texture),
                                     user_agent);
}

/**
 * clutter_gst_video_texture_get_seek_flags:
 * @texture: a #ClutterGstVideoTexture
 *
 * Get the current value of the seek-flags property.
 *
 * Return value: a combination of #ClutterGstSeekFlags
 *
 * Since: 1.4
 */
ClutterGstSeekFlags
clutter_gst_video_texture_get_seek_flags (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_seek_flags (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_set_seek_flags:
 * @texture: a #ClutterGstVideoTexture
 * @flags: a combination of #ClutterGstSeekFlags
 *
 * Seeking can be done with several trade-offs. Clutter-gst defaults
 * to %CLUTTER_GST_SEEK_FLAG_NONE.
 *
 * Since: 1.4
 */
void
clutter_gst_video_texture_set_seek_flags (ClutterGstVideoTexture *texture,
                                          ClutterGstSeekFlags     flags)
{
  clutter_gst_player_set_seek_flags (CLUTTER_GST_PLAYER (texture), flags);
}

/**
 * clutter_gst_video_texture_get_buffering_mode:
 * @texture: a #ClutterGstVideoTexture
 *
 * Return value: a #ClutterGstBufferingMode
 *
 * Since: 1.4
 */
ClutterGstBufferingMode
clutter_gst_video_texture_get_buffering_mode (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_buffering_mode (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_set_buffering_mode:
 * @texture: a #ClutterGstVideoTexture
 * @mode: a #ClutterGstBufferingMode
 *
 * Since: 1.4
 */
void
clutter_gst_video_texture_set_buffering_mode (ClutterGstVideoTexture *texture,
                                              ClutterGstBufferingMode mode)
{
  clutter_gst_player_set_buffering_mode (CLUTTER_GST_PLAYER (texture), mode);
}

/**
 * clutter_gst_video_texture_get_audio_streams:
 * @texture: a #ClutterGstVideoTexture
 *
 * Get the list of audio streams of the current media.
 *
 * Return value: (transfer none) (element-type Gst.TagList): a list of
 * #GstTagList describing the available audio streams
 *
 * Since: 1.4
 */
GList *
clutter_gst_video_texture_get_audio_streams (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_audio_streams (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_get_audio_stream:
 * @texture: a #ClutterGstVideoTexture
 *
 * Get the current audio stream. The number returned in the index of the
 * audio stream playing in the list returned by
 * clutter_gst_video_texture_get_audio_streams().
 *
 * Return value: the index of the current audio stream, -1 if the media has no
 * audio stream
 *
 * Since: 1.4
 */
gint
clutter_gst_video_texture_get_audio_stream (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_audio_stream (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_set_audio_stream:
 * @texture: a #ClutterGstVideoTexture
 * @index_: the index of the audio stream
 *
 * Set the audio stream to play. @index_ is the index of the stream
 * in the list returned by clutter_gst_video_texture_get_audio_streams().
 *
 * Since: 1.4
 */
void
clutter_gst_video_texture_set_audio_stream (ClutterGstVideoTexture *texture,
                                            gint                    index_)
{
  clutter_gst_player_set_audio_stream (CLUTTER_GST_PLAYER (texture), index_);
}

/**
 * clutter_gst_video_texture_get_subtitle_tracks:
 * @texture: a #ClutterGstVideoTexture
 *
 * Get the list of subtitles tracks of the current media.
 *
 * Return value: (transfer none) (element-type Gst.TagList): a list
 * of #GstTagList describing the available subtitles tracks
 *
 * Since: 1.4
 */
GList *
clutter_gst_video_texture_get_subtitle_tracks (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_subtitle_tracks (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_get_subtitle_track:
 * @texture: a #ClutterGstVideoTexture
 *
 * Get the current subtitles track. The number returned is the index of the
 * subitles track in the list returned by
 * clutter_gst_video_texture_get_subtitle_tracks().
 *
 * Return value: the index of the current subtitlest track, -1 if the media has
 * no subtitles track or if the subtitles have been turned off
 *
 * Since: 1.4
 */
gint
clutter_gst_video_texture_get_subtitle_track (ClutterGstVideoTexture *texture)
{
  return clutter_gst_player_get_subtitle_track (CLUTTER_GST_PLAYER (texture));
}

/**
 * clutter_gst_video_texture_set_subtitle_track:
 * @texture: a #ClutterGstVideoTexture
 * @index_: the index of the subtitles track
 *
 * Set the subtitles track to play. @index_ is the index of the stream
 * in the list returned by clutter_gst_video_texture_get_subtitle_tracks().
 *
 * If @index_ is -1, the subtitles are turned off.
 *
 * Since: 1.4
 */
void
clutter_gst_video_texture_set_subtitle_track (ClutterGstVideoTexture *texture,
                                              gint                    index_)
{
  clutter_gst_player_set_subtitle_track (CLUTTER_GST_PLAYER (texture), index_);
}
