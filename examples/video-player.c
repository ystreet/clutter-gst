/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * video-player.c - A simple video player with an OSD.
 *
 * Copyright (C) 2007,2008 OpenedHand
 * Copyright (C) 2013 Collabora
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

#include <clutter/clutter.h>
#include <clutter-gst/clutter-gst.h>

#define SEEK_H 14
#define SEEK_W 440

#define GST_PLAY_FLAG_VIS 1 << 3

typedef struct _VideoApp
{
  ClutterActor *stage;

  ClutterActor *vtexture;

  ClutterActor *control;
  ClutterActor *control_bg;
  ClutterActor *control_label;

  ClutterActor *control_play, *control_pause;

  ClutterActor *control_seek1, *control_seek2, *control_seekbar;

  gboolean      controls_showing, paused, mouse_in_window;

  guint         controls_timeout;
} VideoApp;

static void show_controls (VideoApp *app, gboolean vis);

static gboolean opt_fullscreen = FALSE;
static gboolean opt_loop = FALSE;

static GOptionEntry options[] =
{
  { "fullscreen",
    'f', 0,
    G_OPTION_ARG_NONE,
    &opt_fullscreen,
    "Start the player in fullscreen",
    NULL },

  { "loop",
    'l', 0,
    G_OPTION_ARG_NONE,
    &opt_loop,
    "Start the video again once reached the EOS",
    NULL },

  { NULL }
};

static gboolean
controls_timeout_cb (gpointer data)
{
  VideoApp *app = data;

  app->controls_timeout = 0;
  show_controls (app, FALSE);

  return FALSE;
}

static ClutterTimeline *
_actor_animate (ClutterActor         *actor,
                ClutterAnimationMode  mode,
                guint                 duration,
                const gchar          *first_property,
                ...)
{
  va_list args;

  clutter_actor_save_easing_state (actor);
  clutter_actor_set_easing_mode (actor, mode);
  clutter_actor_set_easing_duration (actor, duration);

  va_start (args, first_property);
  g_object_set_valist (G_OBJECT (actor), first_property, args);
  va_end (args);

  clutter_actor_restore_easing_state (actor);

  return CLUTTER_TIMELINE (clutter_actor_get_transition (actor,
                                                         first_property));
}

static void
show_controls (VideoApp *app, gboolean vis)
{
  if (app->control == NULL)
    return;

  if (vis == TRUE && app->controls_showing == TRUE)
    {
      if (app->controls_timeout == 0)
        {
          app->controls_timeout =
            g_timeout_add_seconds (5, controls_timeout_cb, app);
        }

      return;
    }

  if (vis == TRUE && app->controls_showing == FALSE)
    {
      app->controls_showing = TRUE;

      clutter_stage_show_cursor (CLUTTER_STAGE (app->stage));
      _actor_animate (app->control, CLUTTER_EASE_OUT_QUINT, 250,
                      "opacity", 224,
                      NULL);

      return;
    }

  if (vis == FALSE && app->controls_showing == TRUE)
    {
      app->controls_showing = FALSE;

      if (app->mouse_in_window)
        clutter_stage_hide_cursor (CLUTTER_STAGE (app->stage));
      _actor_animate (app->control, CLUTTER_EASE_OUT_QUINT, 250,
                      "opacity", 0,
                      NULL);
      return;
    }
}

void
toggle_pause_state (VideoApp *app)
{
  if (app->vtexture == NULL)
    return;

  if (app->paused)
    {
      clutter_media_set_playing (CLUTTER_MEDIA(app->vtexture),
                                 TRUE);
      app->paused = FALSE;
      clutter_actor_hide (app->control_play);
      clutter_actor_show (app->control_pause);
    }
  else
    {
      clutter_media_set_playing (CLUTTER_MEDIA(app->vtexture),
                                 FALSE);
      app->paused = TRUE;
      clutter_actor_hide (app->control_pause);
      clutter_actor_show (app->control_play);
    }
}

static void
reset_animation (ClutterTimeline  *animation,
                 VideoApp         *app)
{
  if (app->vtexture)
    clutter_actor_set_rotation_angle (app->vtexture, CLUTTER_Y_AXIS, 0.0);
}

static gboolean
input_cb (ClutterStage *stage,
          ClutterEvent *event,
          gpointer      user_data)
{
  VideoApp *app = (VideoApp*)user_data;
  gboolean handled = FALSE;

  switch (event->type)
    {
    case CLUTTER_MOTION:
      show_controls (app, TRUE);
      handled = TRUE;
      break;

    case CLUTTER_BUTTON_PRESS:
      if (app->controls_showing)
        {
          ClutterActor       *actor;
          ClutterButtonEvent *bev = (ClutterButtonEvent *) event;

          actor = clutter_stage_get_actor_at_pos (stage, CLUTTER_PICK_ALL,
                                                  bev->x,
                                                  bev->y);

          if (actor == app->control_pause || actor == app->control_play)
            {
              toggle_pause_state (app);
            }
          else if (actor == app->control_seek1 ||
                   actor == app->control_seek2 ||
                   actor == app->control_seekbar)
            {
              gfloat x, y, dist;
              gdouble progress;

              clutter_actor_get_transformed_position (app->control_seekbar,
                                                      &x, &y);

              dist = bev->x - x;

              dist = CLAMP (dist, 0, SEEK_W);

              progress = (gdouble) dist / SEEK_W;

              clutter_media_set_progress (CLUTTER_MEDIA (app->vtexture),
                                          progress);
            }
        }
      handled = TRUE;
      break;

    case CLUTTER_KEY_PRESS:
      {
        ClutterTimeline *animation = NULL;

        switch (clutter_event_get_key_symbol (event))
          {
          case CLUTTER_d:
            if (app->vtexture)
              {
                clutter_actor_remove_child (app->stage, app->vtexture);
                app->vtexture = NULL;
              }
            if (app->control)
              {
                clutter_actor_remove_child (app->stage, app->control);
                app->control = NULL;
              }
            break;
          case CLUTTER_q:
          case CLUTTER_Escape:
            clutter_actor_destroy (app->stage);
            break;

          case CLUTTER_e:
            if (app->vtexture == NULL)
              break;

            clutter_actor_set_pivot_point (app->vtexture, 0.5, 0);

            animation = _actor_animate (app->vtexture, CLUTTER_LINEAR, 500,
                                        "rotation-angle-y", 360.0, NULL);

            g_signal_connect_after (animation, "completed",
                                    G_CALLBACK (reset_animation),
                                    app);
            handled = TRUE;
            break;

          default:
            toggle_pause_state (app);
            handled = TRUE;
            break;
          }
      }

    case CLUTTER_ENTER:
      app->mouse_in_window = TRUE;
      g_object_set (app->stage, "cursor-visible", app->controls_showing, NULL);
      break;

    case CLUTTER_LEAVE:
      app->mouse_in_window = FALSE;
      clutter_stage_show_cursor (CLUTTER_STAGE (app->stage));
      break;

    default:
      break;
    }

  return handled;
}

static void
size_change (ClutterTexture *texture,
             gint            base_width,
             gint            base_height,
             VideoApp       *app)
{
  ClutterActor *stage = app->stage;
  gfloat new_x, new_y, new_width, new_height;
  gfloat stage_width, stage_height;
  gfloat frame_width, frame_height;

  clutter_actor_get_size (stage, &stage_width, &stage_height);

  /* base_width and base_height are the actual dimensions of the buffers before
   * taking the pixel aspect ratio into account. We need to get the actual
   * size of the texture to display */
  clutter_actor_get_size (CLUTTER_ACTOR (texture), &frame_width, &frame_height);

  new_height = (frame_height * stage_width) / frame_width;
  if (new_height <= stage_height)
    {
      new_width = stage_width;

      new_x = 0;
      new_y = (stage_height - new_height) / 2;
    }
  else
    {
      new_width  = (frame_width * stage_height) / frame_height;
      new_height = stage_height;

      new_x = (stage_width - new_width) / 2;
      new_y = 0;
    }

  clutter_actor_set_position (CLUTTER_ACTOR (texture), new_x, new_y);
  clutter_actor_set_size (CLUTTER_ACTOR (texture), new_width, new_height);
}

static void
position_controls (VideoApp     *app,
                   ClutterActor *controls)
{
  gfloat x, y, stage_width, stage_height, bg_width, bg_height;

  clutter_actor_get_size (app->stage, &stage_width, &stage_height);
  clutter_actor_get_size (app->control, &bg_width, &bg_height);

  x = (int)((stage_width - bg_width ) / 2);
  y = stage_height - bg_height - 28;

  clutter_actor_set_position (controls, x, y);
}

static void
on_stage_allocation_changed (ClutterActor           *stage,
                             ClutterActorBox        *box,
                             ClutterAllocationFlags  flags,
                             VideoApp               *app)
{
  position_controls (app, app->control);
  show_controls (app, TRUE);
}

static void
tick (GObject      *object,
      GParamSpec   *pspec,
      VideoApp     *app)
{
  ClutterMedia *video_texture = CLUTTER_MEDIA (object);
  gdouble progress = clutter_media_get_progress (video_texture);

  clutter_actor_set_size (app->control_seekbar,
                          progress * SEEK_W,
                          SEEK_H);
}

static void
on_video_texture_eos (ClutterMedia *media,
                      VideoApp     *app)
{
  if (opt_loop)
    {
      clutter_media_set_progress (media, 0.0);
      clutter_media_set_playing (media, TRUE);
    }
}

static ClutterActor *
_new_rectangle_with_color (ClutterColor *color)
{
  ClutterActor *actor = clutter_actor_new ();
  clutter_actor_set_background_color (actor, color);

  return actor;
}

int
main (int argc, char *argv[])
{
  VideoApp            *app = NULL;
  GstElement          *pipe;
  GstElement          *playsink;
  GstElement          *goomsource;
  GstIterator         *iter;
  ClutterActor        *stage;
  ClutterColor         stage_color = { 0x00, 0x00, 0x00, 0x00 };
  ClutterColor         control_color1 = { 73, 74, 77, 0xee };
  ClutterColor         control_color2 = { 0xcc, 0xcc, 0xcc, 0xff };
  GError              *error = NULL;
  GValue               value = { 0, };
  char                *sink_name;
  int                  playsink_flags;

  clutter_gst_init_with_args (&argc,
                              &argv,
                              " - A simple video player",
                              options,
                              NULL,
                              &error);

  if (error)
    {
      g_print ("%s\n", error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }

  if (argc < 2)
    {
      g_print ("Usage: %s [OPTIONS] <video file>\n", argv[0]);
      return EXIT_FAILURE;
    }

  stage = clutter_stage_new ();
  clutter_actor_set_background_color (stage, &stage_color);
  clutter_actor_set_size (stage, 768, 576);
  clutter_stage_set_minimum_size (CLUTTER_STAGE (stage), 640, 480);
  if (opt_fullscreen)
    clutter_stage_set_fullscreen (CLUTTER_STAGE (stage), TRUE);

  app = g_new0(VideoApp, 1);
  app->stage = stage;
  app->vtexture = clutter_gst_video_texture_new ();

  if (app->vtexture == NULL)
    g_error("failed to create vtexture");

  /* By default ClutterGst seeks to the nearest key frame (faster). However
   * it has the weird effect that when you click on the progress bar, the fill
   * goes to the key frame position that can be quite far from where you
   * clicked. Using the ACCURATE flag tells playbin2 to seek to the actual
   * frame */
  clutter_gst_video_texture_set_seek_flags (
                                    CLUTTER_GST_VIDEO_TEXTURE (app->vtexture),
                                    CLUTTER_GST_SEEK_FLAG_ACCURATE);

  g_signal_connect (app->vtexture,
                    "eos",
                    G_CALLBACK (on_video_texture_eos),
                    app);

  g_signal_connect (stage,
                    "allocation-changed",
                    G_CALLBACK (on_stage_allocation_changed),
                    app);

  g_signal_connect (stage,
                    "destroy",
                    G_CALLBACK (clutter_main_quit),
                    NULL);

  /* Handle it ourselves so can scale up for fullscreen better */
  g_signal_connect_after (CLUTTER_TEXTURE (app->vtexture),
                          "size-change",
                          G_CALLBACK (size_change), app);

  /* Load up out video texture */
  clutter_media_set_filename (CLUTTER_MEDIA (app->vtexture), argv[1]);

  /* Set up things so that a visualisation is played if there's no video */
  pipe = clutter_gst_video_texture_get_pipeline (CLUTTER_GST_VIDEO_TEXTURE (app->vtexture));
  if (!pipe)
    g_error ("Unable to get gstreamer pipeline!\n");

  iter = gst_bin_iterate_sinks (GST_BIN (pipe));
  if (!iter)
    g_error ("Unable to iterate over sinks!\n");
  while (gst_iterator_next (iter, &value) == GST_ITERATOR_OK) {
    playsink = g_value_get_object (&value);
    sink_name = gst_element_get_name (playsink);
    if (g_strcmp0 (sink_name, "playsink") != 0) {
      g_free (sink_name);
      break;
    }
    g_free (sink_name);
  }
  gst_iterator_free (iter);

  goomsource = gst_element_factory_make ("goom", "source");
  if (!goomsource)
    g_error ("Unable to create goom visualiser!\n");

  g_object_get (playsink, "flags", &playsink_flags, NULL);
  playsink_flags |= GST_PLAY_FLAG_VIS;
  g_object_set (playsink,
                "vis-plugin", goomsource,
                "flags", playsink_flags,
                NULL);

  /* Create the control UI */
  app->control = clutter_actor_new ();

  app->control_bg =
    clutter_texture_new_from_file ("vid-panel.png", NULL);
  app->control_play =
    clutter_texture_new_from_file ("media-actions-start.png", NULL);
  app->control_pause =
    clutter_texture_new_from_file ("media-actions-pause.png", NULL);

  g_assert (app->control_bg && app->control_play && app->control_pause);

  app->control_seek1   = _new_rectangle_with_color (&control_color1);
  app->control_seek2   = _new_rectangle_with_color (&control_color2);
  app->control_seekbar = _new_rectangle_with_color (&control_color1);
  clutter_actor_set_opacity (app->control_seekbar, 0x99);

  app->control_label =
    clutter_text_new_full ("Sans Bold 14",
                           g_path_get_basename (argv[1]),
                           &control_color1);

  clutter_actor_hide (app->control_play);

  clutter_actor_add_child (app->control, app->control_bg);
  clutter_actor_add_child (app->control, app->control_play);
  clutter_actor_add_child (app->control, app->control_pause);
  clutter_actor_add_child (app->control, app->control_seek1);
  clutter_actor_add_child (app->control, app->control_seek2);
  clutter_actor_add_child (app->control, app->control_seekbar);
  clutter_actor_add_child (app->control, app->control_label);

  clutter_actor_set_opacity (app->control, 0xee);

  clutter_actor_set_position (app->control_play, 22, 31);
  clutter_actor_set_position (app->control_pause, 18, 31);

  clutter_actor_set_size (app->control_seek1, SEEK_W+4, SEEK_H+4);
  clutter_actor_set_position (app->control_seek1, 80, 57);
  clutter_actor_set_size (app->control_seek2, SEEK_W, SEEK_H);
  clutter_actor_set_position (app->control_seek2, 82, 59);
  clutter_actor_set_size (app->control_seekbar, 0, SEEK_H);
  clutter_actor_set_position (app->control_seekbar, 82, 59);

  clutter_actor_set_position (app->control_label, 82, 29);

  /* Add control UI to stage */
  clutter_actor_add_child (stage, app->vtexture);
  clutter_actor_add_child (stage, app->control);

  position_controls (app, app->control);

  clutter_stage_hide_cursor (CLUTTER_STAGE (stage));
  _actor_animate (app->control, CLUTTER_EASE_OUT_QUINT, 1000,
                  "opacity", 0,
                  NULL);

  /* Hook up other events */
  g_signal_connect (stage, "event", G_CALLBACK (input_cb), app);

  g_signal_connect (app->vtexture,
                    "notify::progress", G_CALLBACK (tick),
                    app);

  clutter_media_set_playing (CLUTTER_MEDIA (app->vtexture), TRUE);

  clutter_actor_show (stage);

  clutter_main ();

  return EXIT_SUCCESS;
}
