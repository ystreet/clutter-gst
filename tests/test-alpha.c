/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * test-alpha.c - Transparent videos.
 *
 * Authored by Damien Lespiau  <damien.lespiau@intel.com>
 *
 * Copyright (C) 2009 Intel Corporation
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

#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <glib/gprintf.h>
#include <clutter-gst/clutter-gst.h>

static gint   opt_framerate = 30;
static gchar *opt_fourcc    = "I420";
static gint   opt_bpp       = 24;
static gint   opt_depth     = 24;

static GOptionEntry options[] =
{
  { "framerate",
    'f', 0,
    G_OPTION_ARG_INT,
    &opt_framerate,
    "Number of frames per second",
    NULL },
  { "fourcc",
    'o', 0,
    G_OPTION_ARG_STRING,
    &opt_fourcc,
    "Fourcc of the wanted YUV format",
    NULL },

  { NULL }
};

static guint32
parse_fourcc (const gchar *fourcc)
{
  if (strlen (fourcc) != 4)
    return 0;

  return GST_STR_FOURCC (fourcc);
}

void
size_change (ClutterTexture *texture,
             gint            width,
             gint            height,
             gpointer        user_data)
{
  ClutterActor *stage;
  gfloat new_x, new_y, new_width, new_height;
  gfloat stage_width, stage_height;

  stage = clutter_actor_get_stage (CLUTTER_ACTOR (texture));
  if (stage == NULL)
    return;

  clutter_actor_get_size (stage, &stage_width, &stage_height);

  new_height = (height * stage_width) / width;
  if (new_height <= stage_height)
    {
      new_width = stage_width;

      new_x = 0;
      new_y = (stage_height - new_height) / 2;
    }
  else
    {
      new_width  = (width * stage_height) / height;
      new_height = stage_height;

      new_x = (stage_width - new_width) / 2;
      new_y = 0;
    }

  clutter_actor_set_position (CLUTTER_ACTOR (texture), new_x, new_y);
  clutter_actor_set_size (CLUTTER_ACTOR (texture), new_width, new_height);
}

int
main (int argc, char *argv[])
{
  GError                *error = NULL;
  gboolean               result;

  const ClutterColor     stage_color     = {128,   0, 192, 255};
  const ClutterColor     rectangle_color = { 96,   0,   0, 255};
  const ClutterGeometry  rectangle_geom  = {110,  70, 100, 100};
  ClutterActor          *stage;
  ClutterActor          *texture;
  ClutterActor          *rectangle;
  ClutterAnimation      *animation;

  GstPipeline           *pipeline;
  GstElement            *src;
  GstElement            *capsfilter;
  GstElement            *sink;
  GstCaps               *caps;

  if (!g_thread_supported ())
    g_thread_init (NULL);

  result = clutter_gst_init_with_args (&argc,
                                       &argv,
                                       " - Test alpha with video textures",
                                       options,
                                       NULL,
                                       &error);

  if (error)
    {
      g_print ("%s\n", error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }


  stage = clutter_stage_get_default ();
  clutter_actor_set_size (stage, 320.0f, 240.0f);
  clutter_stage_set_color (CLUTTER_STAGE (stage), &stage_color);

  rectangle = clutter_rectangle_new_with_color (&rectangle_color);
  clutter_actor_set_geometry (rectangle, &rectangle_geom);

  /* We need to set certain props on the target texture currently for
   * efficient/corrent playback onto the texture (which sucks a bit)
   */
  texture = g_object_new (CLUTTER_TYPE_TEXTURE,
                          "disable-slicing", TRUE,
                          NULL);
  clutter_actor_set_opacity (texture, 0);

  g_signal_connect (CLUTTER_TEXTURE (texture),
                    "size-change",
                    G_CALLBACK (size_change), NULL);

  /* Set up pipeline */
  pipeline = GST_PIPELINE(gst_pipeline_new (NULL));

  src = gst_element_factory_make ("videotestsrc", NULL);
  g_object_set (G_OBJECT (src), "pattern", 1, NULL);
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  sink = gst_element_factory_make ("cluttersink", NULL);
  g_object_set (G_OBJECT (sink), "texture", CLUTTER_TEXTURE (texture), NULL);

  /* make videotestsrc spit the format we want */
  if (g_strcmp0 (opt_fourcc, "RGB ") == 0)
    {
      caps = gst_caps_new_simple ("video/x-raw",
				  "format", G_TYPE_STRING, "RGB",
                                  "framerate", GST_TYPE_FRACTION,
                                               opt_framerate, 1,
                                  NULL);

    }
  else
    {
      caps = gst_caps_new_simple ("video/x-raw",
                                  "format", G_TYPE_STRING,
				  opt_fourcc,
                                  "framerate", GST_TYPE_FRACTION,
                                               opt_framerate, 1,
                                  NULL);
    }

  g_object_set (capsfilter, "caps", caps, NULL);

  g_printf ("%s: [caps] %s\n", __FILE__, gst_caps_to_string (caps));
  gst_bin_add_many (GST_BIN (pipeline), src, capsfilter, sink, NULL);
  result = gst_element_link_many (src, capsfilter, sink, NULL);
  if (result == FALSE)
    g_critical("Could not link elements");
  gst_element_set_state (GST_ELEMENT(pipeline), GST_STATE_PLAYING);

  clutter_container_add (CLUTTER_CONTAINER (stage), rectangle, texture, NULL);
  clutter_actor_show_all (stage);

  animation = clutter_actor_animate (texture, CLUTTER_LINEAR, 6000,
                                     "opacity", 0xff,
                                     NULL);
  clutter_animation_set_loop (animation, TRUE);

  clutter_main();

  return EXIT_SUCCESS;
}
