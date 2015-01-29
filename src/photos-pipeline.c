/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */


#include "config.h"

#include <glib.h>

#include "photos-pipeline.h"


struct _PhotosPipeline
{
  GObject parent_instance;
  GQueue *history;
  GeglNode *graph;
};

struct _PhotosPipelineClass
{
  GObjectClass parent_class;
};


G_DEFINE_TYPE (PhotosPipeline, photos_pipeline, G_TYPE_OBJECT);


static void
photos_pipeline_dispose (GObject *object)
{
  PhotosPipeline *self = PHOTOS_PIPELINE (object);

  g_clear_object (&self->graph);

  G_OBJECT_CLASS (photos_pipeline_parent_class)->dispose (object);
}


static void
photos_pipeline_finalize (GObject *object)
{
  PhotosPipeline *self = PHOTOS_PIPELINE (object);

  g_queue_free (self->history);

  G_OBJECT_CLASS (photos_pipeline_parent_class)->finalize (object);
}


static void
photos_pipeline_init (PhotosPipeline *self)
{
  GeglNode *input;
  GeglNode *output;

  self->history = g_queue_new ();

  self->graph = gegl_node_new ();
  input = gegl_node_get_input_proxy (self->graph, "input");
  output = gegl_node_get_output_proxy (self->graph, "output");
  gegl_node_link (input, output);
}


static void
photos_pipeline_class_init (PhotosPipelineClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->dispose = photos_pipeline_dispose;
  object_class->finalize = photos_pipeline_finalize;
}


PhotosPipeline *
photos_pipeline_new (void)
{
  return g_object_new (PHOTOS_TYPE_PIPELINE, NULL);
}


void
photos_pipeline_add (PhotosPipeline *self, const gchar *operation)
{
  GeglNode *last;
  GeglNode *node;
  GeglNode *output;

  node = gegl_node_new_child (self->graph, "operation", operation, NULL);
  output = gegl_node_get_output_proxy (self->graph, "output");
  last = gegl_node_get_producer (output, "input", NULL);
  gegl_node_disconnect (output, "input");
  gegl_node_link_many (last, node, output, NULL);
}


GeglNode *
photos_pipeline_get_input (PhotosPipeline *self)
{
  GeglNode *input;

  input = gegl_node_get_input_proxy (self->graph, "input");
  return input;
}


GeglNode *
photos_pipeline_get_output (PhotosPipeline *self)
{
  GeglNode *output;

  output = gegl_node_get_output_proxy (self->graph, "output");
  return output;
}


void
photos_pipeline_redo (PhotosPipeline *self)
{
}


void
photos_pipeline_undo (PhotosPipeline *self)
{
  GeglNode *input;
  GeglNode *last;
  GeglNode *last2;
  GeglNode *output;

  input = gegl_node_get_input_proxy (self->graph, "input");
  output = gegl_node_get_output_proxy (self->graph, "output");
  last = gegl_node_get_producer (output, "input", NULL);
  if (last == input)
    return;

  g_queue_push_head (self->history, last);

  last2 = gegl_node_get_producer (last, "input", NULL);
  gegl_node_disconnect (output, "input");
  gegl_node_disconnect (last, "input");
  gegl_node_link (last2, output);
}
