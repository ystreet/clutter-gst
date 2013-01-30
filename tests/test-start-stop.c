/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * test-start-stop.c - Test switching between 2 media files.
 *
 * Authored by Shuang He <shuang.he@intel.com>
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

#include <clutter/clutter.h>
#include <clutter-gst/clutter-gst.h>

char *video_files[] = {NULL, NULL};

void
size_change (ClutterTexture *texture,
             gint            width,
             gint            height,
             gpointer        user_data)
{
  ClutterActor *stage = (ClutterActor *)user_data;
  gfloat new_x, new_y, new_width, new_height;
  gfloat stage_width, stage_height;

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

void
on_error (ClutterMedia *media)
{
  g_print ("error\n");
  clutter_main_quit ();
}

gboolean
test (gpointer data)
{
  static int count = 1;
  static ClutterMedia *media = NULL;
  static char *uri[2] = {NULL, NULL};
  const char *playing_uri = NULL;

  /* Check until we get video playing */
  if (!clutter_media_get_playing (CLUTTER_MEDIA (data)))
    return TRUE;

  if (CLUTTER_MEDIA (data) != media)
    {
      media = CLUTTER_MEDIA (data);
      count = 1;
      g_free(uri[0]);
      uri[0] = NULL;
      g_free(uri[1]);
      uri[1] = NULL;
    }

  clutter_media_set_filename (media, video_files[count & 1]);
  g_print ("playing %s\n", video_files[count & 1]);

  if (uri[count & 1] == NULL)
    {
      uri[count & 1] = g_strdup (clutter_media_get_uri (media));
      g_assert (uri[count & 1] != NULL);
    }

  /* See if it's still playing */
  g_assert (clutter_media_get_playing (media));

  /* See if it's already change to play correct file */
  playing_uri = clutter_media_get_uri (media);
  g_assert_cmpstr (playing_uri, ==, uri[count & 1]);

  if (count ++ > 10)
    {
      clutter_media_set_playing (media, FALSE);
      clutter_main_quit ();
      return FALSE;
    }

  return TRUE;
}


int
main (int argc, char *argv[])
{
  ClutterInitError error;
  ClutterColor     stage_color = { 0x00, 0x00, 0x00, 0x00 };
  ClutterActor    *stage = NULL;
  ClutterActor    *video = NULL;

  if (argc < 3)
    {
      g_print ("%s video1 video2\n", argv[0]);
      exit (1);
    }

  video_files[0] = argv[1];
  video_files[1] = argv[2];

  error = clutter_gst_init (&argc, &argv);
  g_assert (error == CLUTTER_INIT_SUCCESS);

  stage = clutter_stage_new ();
  clutter_stage_set_color (CLUTTER_STAGE (stage), &stage_color);

  video = clutter_gst_video_texture_new ();
  g_assert (CLUTTER_GST_IS_VIDEO_TEXTURE(video));

  g_signal_connect (CLUTTER_TEXTURE(video),
                    "size-change",
                    G_CALLBACK(size_change),
                    stage);
  g_signal_connect (CLUTTER_TEXTURE(video),
                    "error",
                    G_CALLBACK(on_error),
                    stage);
  g_timeout_add (5000, test, video);
  clutter_media_set_filename (CLUTTER_MEDIA(video), video_files[0]);
  clutter_media_set_audio_volume (CLUTTER_MEDIA(video), 0.5);
  clutter_media_set_playing (CLUTTER_MEDIA(video), TRUE);

  clutter_container_add (CLUTTER_CONTAINER(stage), video, NULL);
  clutter_actor_show_all (stage);
  clutter_main ();

  return EXIT_SUCCESS;
}
