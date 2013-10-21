/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2012, 2013 Red Hat, Inc.
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

/* Based on code from:
 *   + Documents
 */


#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <tracker-sparql.h>

#include "photos-base-item.h"
#include "photos-collection-icon-watcher.h"
#include "photos-delete-item-job.h"
#include "photos-print-operation.h"
#include "photos-query.h"
#include "photos-selection-controller.h"
#include "photos-single-item-job.h"
#include "photos-utils.h"


struct _PhotosBaseItemPrivate
{
  GdkPixbuf *icon;
  GdkPixbuf *pristine_icon;
  GeglNode *graph;
  GeglNode *node;
  GeglRectangle bbox;
  GMutex mutex_download;
  GMutex mutex;
  GQuark equipment;
  GQuark flash;
  PhotosCollectionIconWatcher *watcher;
  PhotosSelectionController *sel_cntrlr;
  TrackerSparqlCursor *cursor;
  gboolean collection;
  gboolean failed_thumbnailing;
  gboolean favorite;
  gboolean thumbnailed;
  const gchar *thumb_path;
  gchar *author;
  gchar *default_app_name;
  gchar *id;
  gchar *identifier;
  gchar *mime_type;
  gchar *name;
  gchar *rdf_type;
  gchar *resource_urn;
  gchar *type_description;
  gchar *uri;
  gdouble exposure_time;
  gdouble fnumber;
  gdouble focal_length;
  gdouble iso_speed;
  gint64 date_created;
  gint64 height;
  gint64 mtime;
  gint64 width;
};

enum
{
  PROP_0,
  PROP_CURSOR,
  PROP_FAILED_THUMBNAILING,
  PROP_ICON,
  PROP_ID,
};

enum
{
  INFO_UPDATED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


G_DEFINE_TYPE_WITH_PRIVATE (PhotosBaseItem, photos_base_item, G_TYPE_OBJECT);


static GThreadPool *create_thumbnail_pool;


static void photos_base_item_populate_from_cursor (PhotosBaseItem *self, TrackerSparqlCursor *cursor);


static GIcon *
photos_base_item_create_symbolic_emblem (const gchar *name)
{
  GIcon *pix;
  gint size;

  size = photos_utils_get_icon_size ();
  pix = photos_utils_create_symbolic_icon (name, size);
  if (pix == NULL)
    pix = g_themed_icon_new (name);

  return pix;
}


static void
photos_base_item_set_icon (PhotosBaseItem *self, GdkPixbuf *icon)
{
  PhotosBaseItemPrivate *priv = self->priv;

  if (priv->icon == icon)
    return;

  g_clear_object (&priv->icon);
  if (icon != NULL)
    priv->icon = g_object_ref (icon);

  g_object_notify (G_OBJECT (self), "icon");
}


static void
photos_base_item_check_effects_and_update_info (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GIcon *pix;
  GList *emblem_icons = NULL;
  GdkPixbuf *icon;

  icon = g_object_ref (priv->icon);
  priv->pristine_icon = g_object_ref (icon);

  if (priv->favorite)
    {
      pix = photos_base_item_create_symbolic_emblem ("emblem-favorite");
      emblem_icons = g_list_prepend (emblem_icons, pix);
    }

  if (g_list_length (emblem_icons) > 0)
    {
      GIcon *emblemed_icon;
      GList *l;
      GtkIconInfo *icon_info;
      GtkIconTheme *theme;
      gint height;
      gint size;
      gint width;

      emblem_icons = g_list_reverse (emblem_icons);
      emblemed_icon = g_emblemed_icon_new (G_ICON (priv->icon), NULL);
      for (l = emblem_icons; l != NULL; l = g_list_next (l))
        {
          GEmblem *emblem;
          GIcon *emblem_icon = G_ICON (l->data);

          emblem = g_emblem_new (emblem_icon);
          g_emblemed_icon_add_emblem (G_EMBLEMED_ICON (emblemed_icon), emblem);
          g_object_unref (emblem);
        }

      theme = gtk_icon_theme_get_default ();

      width = gdk_pixbuf_get_width (priv->icon);
      height = gdk_pixbuf_get_height (priv->icon);
      size = (width > height) ? width : height;

      icon_info = gtk_icon_theme_lookup_by_gicon (theme, emblemed_icon, size, GTK_ICON_LOOKUP_FORCE_SIZE);

      if (icon_info != NULL)
        {
          GError *error = NULL;
          GdkPixbuf *tmp;

          tmp = gtk_icon_info_load_icon (icon_info, &error);
          if (error != NULL)
            {
              g_warning ("Unable to render the emblem: %s", error->message);
              g_error_free (error);
            }
          else
            {
              g_object_unref (icon);
              icon = tmp;
            }

          g_object_unref (icon_info);
        }
    }

  g_object_unref (priv->icon);

  if (priv->thumbnailed)
    {
      GtkBorder *slice;
      GdkPixbuf *framed_icon;

      slice = photos_utils_get_thumbnail_frame_border ();
      framed_icon = photos_utils_embed_image_in_frame (icon,
                                                       PACKAGE_ICONS_DIR "/thumbnail-frame.png",
                                                       slice,
                                                       slice);
      photos_base_item_set_icon (self, framed_icon);
      g_clear_object (&framed_icon);
      gtk_border_free (slice);
    }
  else
    photos_base_item_set_icon (self, icon);

  g_signal_emit (self, signals[INFO_UPDATED], 0);

  g_object_unref (icon);
  g_list_free_full (emblem_icons, g_object_unref);
}


static void
photos_base_item_create_thumbnail_in_thread_func (gpointer data, gpointer user_data)
{
  GTask *task = G_TASK (data);
  PhotosBaseItem *self;
  GCancellable *cancellable;
  GError *error;
  gboolean op_res;

  self = PHOTOS_BASE_ITEM (g_task_get_source_object (task));
  cancellable = g_task_get_cancellable (task);

  error = NULL;
  op_res = PHOTOS_BASE_ITEM_GET_CLASS (self)->create_thumbnail (self, cancellable, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

  g_task_return_boolean (task, op_res);

 out:
  g_object_unref (task);
}


static void
photos_base_item_create_thumbnail_async (PhotosBaseItem *self,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
  GTask *task;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_check_cancellable (task, TRUE);
  g_task_set_source_tag (task, photos_base_item_create_thumbnail_async);

  g_thread_pool_push (create_thumbnail_pool, g_object_ref (task), NULL);
  g_object_unref (task);
}


static gboolean
photos_base_item_create_thumbnail_finish (PhotosBaseItem *self, GAsyncResult *res, GError **error)
{
  GTask *task = G_TASK (res);

  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);
  g_return_val_if_fail (g_task_get_source_tag (task) == photos_base_item_create_thumbnail_async, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (task, error);
}


static void
photos_base_item_default_set_favorite (PhotosBaseItem *self, gboolean favorite)
{
  photos_utils_set_favorite (self->priv->id, favorite);
}


static void
photos_base_item_default_open (PhotosBaseItem *self, GdkScreen *screen, guint32 timestamp)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GError *error;

  error = NULL;
  gtk_show_uri (screen, priv->uri, timestamp, &error);
  if (error != NULL)
    {
      g_warning ("Unable to show URI %s: %s", priv->uri, error->message);
      g_error_free (error);
    }
}


static void
photos_base_item_default_update_type_description (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  gchar *description = NULL;

  if (priv->collection)
    description = g_strdup (_("Album"));
  else if (priv->mime_type != NULL)
    description = g_content_type_get_description (priv->mime_type);

  priv->type_description = description;
}


static void
photos_base_item_download_in_thread_func (GTask *task,
                                          gpointer source_object,
                                          gpointer task_data,
                                          GCancellable *cancellable)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (source_object);
  PhotosBaseItemPrivate *priv = self->priv;
  GError *error;
  gchar *path;

  g_mutex_lock (&priv->mutex_download);

  error = NULL;
  path = photos_base_item_download (self, cancellable, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

  g_task_return_pointer (task, path, g_free);

 out:
  g_mutex_unlock (&priv->mutex_download);
}


static void
photos_base_item_icon_updated (PhotosBaseItem *self, GIcon *icon)
{
  PhotosBaseItemPrivate *priv = self->priv;

  if (icon == NULL)
    return;

  photos_base_item_set_icon (self, icon);
  photos_base_item_check_effects_and_update_info (self);
}


static void
photos_base_item_refresh_collection_icon (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;

  if (priv->watcher == NULL)
    {
      priv->watcher = photos_collection_icon_watcher_new (self);
      g_signal_connect_swapped (priv->watcher, "icon-updated", G_CALLBACK (photos_base_item_icon_updated), self);
    }
  else
    photos_collection_icon_watcher_refresh (priv->watcher);
}


static void
photos_base_item_refresh_executed (TrackerSparqlCursor *cursor, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (user_data);

  if (cursor == NULL)
    goto out;

  photos_base_item_populate_from_cursor (self, cursor);

 out:
  g_object_unref (self);
}


static void
photos_base_item_refresh_thumb_path_pixbuf (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (user_data);
  PhotosBaseItemPrivate *priv = self->priv;
  GdkPixbuf *icon = NULL;
  GError *error = NULL;
  GInputStream *stream = G_INPUT_STREAM (source_object);

  icon = gdk_pixbuf_new_from_stream_finish (res, &error);
  if (error != NULL)
    {
      priv->failed_thumbnailing = TRUE;
      g_error_free (error);
      goto out;
    }

  photos_base_item_set_icon (self, icon);
  priv->thumbnailed = TRUE;
  photos_base_item_check_effects_and_update_info (self);

 out:
  g_clear_object (&icon);
  g_input_stream_close_async (stream, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
  g_object_unref (self);
}


static void
photos_base_item_refresh_thumb_path_read (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (user_data);
  PhotosBaseItemPrivate *priv = self->priv;
  GError *error = NULL;
  GFile *file = G_FILE (source_object);
  GFileInputStream *stream;
  gint size;

  stream = g_file_read_finish (file, res, &error);
  if (error != NULL)
    {
      priv->failed_thumbnailing = TRUE;
      g_error_free (error);
      goto out;
    }

  size = photos_utils_get_icon_size ();
  gdk_pixbuf_new_from_stream_at_scale_async (G_INPUT_STREAM (stream),
                                             size,
                                             size,
                                             TRUE,
                                             NULL,
                                             photos_base_item_refresh_thumb_path_pixbuf,
                                             g_object_ref (self));
  g_object_unref (stream);

 out:
  g_object_unref (self);
}


static void
photos_base_item_refresh_thumb_path (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GFile *thumb_file;

  thumb_file = g_file_new_for_path (priv->thumb_path);
  g_file_read_async (thumb_file,
                     G_PRIORITY_DEFAULT,
                     NULL,
                     photos_base_item_refresh_thumb_path_read,
                     g_object_ref (self));
  g_object_unref (thumb_file);
}


static void
photos_base_item_thumbnail_path_info (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (user_data);
  PhotosBaseItemPrivate *priv = self->priv;
  GError *error = NULL;
  GFile *file = G_FILE (source_object);
  GFileInfo *info;

  info = g_file_query_info_finish (file, res, &error);
  if (error != NULL)
    {
      g_warning ("Unable to query info for file at %s: %s", priv->uri, error->message);
      priv->failed_thumbnailing = TRUE;
      g_error_free (error);
      goto out;
    }

  priv->thumb_path = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
  if (priv->thumb_path != NULL)
    photos_base_item_refresh_thumb_path (self);
  else
    priv->failed_thumbnailing = TRUE;

 out:
  g_object_unref (self);
}


static void
photos_base_item_create_thumbnail_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (source_object);
  PhotosBaseItemPrivate *priv = self->priv;
  GError *error;
  GFile *file = G_FILE (user_data);

  error = NULL;
  photos_base_item_create_thumbnail_finish (self, res, &error);
  if (error != NULL)
    {
      priv->failed_thumbnailing = TRUE;
      g_warning ("Unable to create thumbnail: %s", error->message);
      g_error_free (error);
      goto out;
    }

  g_file_query_info_async (file,
                           G_FILE_ATTRIBUTE_THUMBNAIL_PATH,
                           G_FILE_QUERY_INFO_NONE,
                           G_PRIORITY_DEFAULT,
                           NULL,
                           photos_base_item_thumbnail_path_info,
                           g_object_ref (self));

 out:
  g_object_unref (file);
}


static void
photos_base_item_file_query_info (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (user_data);
  PhotosBaseItemPrivate *priv = self->priv;
  GError *error = NULL;
  GFile *file = G_FILE (source_object);
  GFileInfo *info;

  info = g_file_query_info_finish (file, res, &error);
  if (error != NULL)
    {
      g_warning ("Unable to query info for file at %s: %s", priv->uri, error->message);
      priv->failed_thumbnailing = TRUE;
      g_error_free (error);
      goto out;
    }

  priv->thumb_path = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
  if (priv->thumb_path != NULL)
    photos_base_item_refresh_thumb_path (self);
  else
    {
      priv->thumbnailed = FALSE;
      photos_base_item_create_thumbnail_async (self,
                                               NULL,
                                               photos_base_item_create_thumbnail_cb,
                                               g_object_ref (file));
    }

 out:
  g_object_unref (self);
}


static GeglNode *
photos_base_item_load (PhotosBaseItem *self, GCancellable *cancellable, GError **error)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GeglNode *ret_val = NULL;
  gchar *path = NULL;

  if (priv->graph == NULL)
    {
      path = photos_base_item_download (self, cancellable, error);
      if (path == NULL)
        goto out;

      priv->graph = gegl_node_new ();
      priv->node = gegl_node_new_child (priv->graph,
                                        "operation", "gegl:load",
                                        "path", path,
                                        NULL);
    }

  gegl_node_process (priv->node);
  priv->bbox = gegl_node_get_bounding_box (priv->node);
  ret_val = g_object_ref (priv->node);

 out:
  g_free (path);
  return ret_val;
}


static void
photos_base_item_load_in_thread_func (GSimpleAsyncResult *simple, GObject *object, GCancellable *cancellable)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (object);
  PhotosBaseItemPrivate *priv = self->priv;
  GeglNode *node;
  GError *error = NULL;

  g_mutex_lock (&priv->mutex);

  node = photos_base_item_load (self, cancellable, &error);
  if (error != NULL)
    {
      g_simple_async_result_take_error (simple, error);
      g_simple_async_result_set_op_res_gpointer (simple, NULL, NULL);
    }
  else
    g_simple_async_result_set_op_res_gpointer (simple, (gpointer) node, g_object_unref);

  g_mutex_unlock (&priv->mutex);
}


static void
photos_base_item_update_icon_from_type (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GIcon *icon = NULL;
  GtkIconInfo *info;
  GtkIconTheme *theme;
  gint icon_size;

  if (priv->mime_type != NULL)
    icon = g_content_type_get_icon (priv->mime_type);

  if (icon == NULL)
    icon = photos_utils_icon_from_rdf_type (priv->rdf_type);

  theme = gtk_icon_theme_get_default ();
  icon_size = photos_utils_get_icon_size ();
  info = gtk_icon_theme_lookup_by_gicon (theme,
                                         icon,
                                         icon_size,
                                         GTK_ICON_LOOKUP_FORCE_SIZE | GTK_ICON_LOOKUP_GENERIC_FALLBACK);
  if (info != NULL)
    {
      GdkPixbuf *pixbuf_icon;

      pixbuf_icon = gtk_icon_info_load_icon (info, NULL);
      /* TODO: use a GError */
      photos_base_item_set_icon (self, pixbuf_icon);
      g_clear_object (&pixbuf_icon);
    }

  photos_base_item_check_effects_and_update_info (self);
}


static void
photos_base_item_refresh_icon (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GFile *file;

  if (priv->thumb_path != NULL)
    {
      photos_base_item_refresh_thumb_path (self);
      return;
    }

  photos_base_item_update_icon_from_type (self);

  if (priv->collection)
    {
      photos_base_item_refresh_collection_icon (self);
      return;
    }

  if (priv->failed_thumbnailing)
    return;

  file = g_file_new_for_uri (priv->uri);
  g_file_query_info_async (file,
                           G_FILE_ATTRIBUTE_THUMBNAIL_PATH,
                           G_FILE_QUERY_INFO_NONE,
                           G_PRIORITY_DEFAULT,
                           NULL,
                           photos_base_item_file_query_info,
                           g_object_ref (self));
  g_object_unref (file);
}


static void
photos_base_item_update_info_from_type (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;

  if (strstr (priv->rdf_type, "nfo#DataContainer") != NULL)
    priv->collection = TRUE;

  PHOTOS_BASE_ITEM_GET_CLASS (self)->update_type_description (self);
}


static void
photos_base_item_populate_from_cursor (PhotosBaseItem *self, TrackerSparqlCursor *cursor)
{
  PhotosBaseItemPrivate *priv = self->priv;
  GTimeVal timeval;
  const gchar *date_created;
  const gchar *equipment;
  const gchar *flash;
  const gchar *mtime;
  const gchar *title;
  const gchar *uri;

  uri = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_URI, NULL);
  priv->uri = g_strdup ((uri == NULL) ? "" : uri);

  priv->id = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_URN, NULL));
  priv->identifier = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_IDENTIFIER, NULL));
  priv->author = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_AUTHOR, NULL));
  priv->resource_urn = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_RESOURCE_URN, NULL));
  priv->favorite = tracker_sparql_cursor_get_boolean (cursor, PHOTOS_QUERY_COLUMNS_RESOURCE_FAVORITE);

  mtime = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_MTIME, NULL);
  if (mtime != NULL)
    {
      g_time_val_from_iso8601 (mtime, &timeval);
      priv->mtime = (gint64) timeval.tv_sec;
    }
  else
    priv->mtime = g_get_real_time () / 1000000;

  priv->mime_type = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_MIME_TYPE, NULL));
  priv->rdf_type = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_RDF_TYPE, NULL));
  photos_base_item_update_info_from_type (self);

  date_created = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_DATE_CREATED, NULL);
  if (date_created != NULL)
    {
      g_time_val_from_iso8601 (date_created, &timeval);
      priv->date_created = (gint64) timeval.tv_sec;
    }
  else
    priv->date_created = -1;

  if (g_strcmp0 (priv->id, PHOTOS_COLLECTION_SCREENSHOT) == 0)
    title = _("Screenshots");
  else
    title = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_TITLE, NULL);

  if (title == NULL)
    title = "";
  priv->name = g_strdup (title);

  priv->width = tracker_sparql_cursor_get_integer (cursor, PHOTOS_QUERY_COLUMNS_WIDTH);
  priv->height = tracker_sparql_cursor_get_integer (cursor, PHOTOS_QUERY_COLUMNS_HEIGHT);

  equipment = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_EQUIPMENT, NULL);
  priv->equipment = g_quark_from_string (equipment);

  priv->exposure_time = tracker_sparql_cursor_get_double (cursor, PHOTOS_QUERY_COLUMNS_EXPOSURE_TIME);
  priv->fnumber = tracker_sparql_cursor_get_double (cursor, PHOTOS_QUERY_COLUMNS_FNUMBER);
  priv->focal_length = tracker_sparql_cursor_get_double (cursor, PHOTOS_QUERY_COLUMNS_FOCAL_LENGTH);
  priv->iso_speed = tracker_sparql_cursor_get_double (cursor, PHOTOS_QUERY_COLUMNS_ISO_SPEED);

  flash = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_FLASH, NULL);
  priv->flash = g_quark_from_string (flash);

  photos_base_item_refresh_icon (self);
}


static void
photos_base_item_print_operation_done (PhotosBaseItem *self, GtkPrintOperationResult result)
{
  if (result == GTK_PRINT_OPERATION_RESULT_APPLY)
      photos_selection_controller_set_selection_mode (self->priv->sel_cntrlr, FALSE);
}


static void
photos_base_item_print_load (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (source_object);
  GtkWindow *toplevel = GTK_WINDOW (user_data);
  GeglNode *node;
  GtkPrintOperation *print_op;

  node = photos_base_item_load_finish (self, res, NULL);
  if (node == NULL)
    goto out;

  print_op = photos_print_operation_new (self, node);
  g_signal_connect_data (print_op,
                         "done",
                         G_CALLBACK (photos_base_item_print_operation_done),
                         g_object_ref (self),
                         (GClosureNotify) g_object_unref,
                         G_CONNECT_SWAPPED);
  gtk_print_operation_run (print_op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, toplevel, NULL);

 out:
  g_clear_object (&node);
  g_object_unref (toplevel);
}


static void
photos_base_item_constructed (GObject *object)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (object);
  PhotosBaseItemPrivate *priv = self->priv;

  G_OBJECT_CLASS (photos_base_item_parent_class)->constructed (object);

  photos_base_item_populate_from_cursor (self, priv->cursor);
  g_clear_object (&priv->cursor); /* We will not need it any more */
}


static void
photos_base_item_dispose (GObject *object)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (object);
  PhotosBaseItemPrivate *priv = self->priv;

  g_clear_object (&priv->graph);
  g_clear_object (&priv->icon);
  g_clear_object (&priv->pristine_icon);
  g_clear_object (&priv->watcher);
  g_clear_object (&priv->sel_cntrlr);
  g_clear_object (&priv->cursor);

  G_OBJECT_CLASS (photos_base_item_parent_class)->dispose (object);
}


static void
photos_base_item_finalize (GObject *object)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (object);
  PhotosBaseItemPrivate *priv = self->priv;

  g_free (priv->author);
  g_free (priv->default_app_name);
  g_free (priv->id);
  g_free (priv->identifier);
  g_free (priv->mime_type);
  g_free (priv->name);
  g_free (priv->rdf_type);
  g_free (priv->resource_urn);
  g_free (priv->type_description);
  g_free (priv->uri);

  g_mutex_clear (&priv->mutex_download);
  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (photos_base_item_parent_class)->finalize (object);
}


static void
photos_base_item_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (object);
  PhotosBaseItemPrivate *priv = self->priv;

  switch (prop_id)
    {
    case PROP_ICON:
      g_value_set_object (value, priv->icon);
      break;

    case PROP_ID:
      g_value_set_string (value, priv->id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_base_item_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  PhotosBaseItem *self = PHOTOS_BASE_ITEM (object);
  PhotosBaseItemPrivate *priv = self->priv;

  switch (prop_id)
    {
    case PROP_CURSOR:
      priv->cursor = TRACKER_SPARQL_CURSOR (g_value_dup_object (value));
      break;

    case PROP_FAILED_THUMBNAILING:
      priv->failed_thumbnailing = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_base_item_init (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv;

  self->priv = photos_base_item_get_instance_private (self);
  priv = self->priv;

  g_mutex_init (&priv->mutex_download);
  g_mutex_init (&priv->mutex);

  priv->sel_cntrlr = photos_selection_controller_dup_singleton ();
}


static void
photos_base_item_class_init (PhotosBaseItemClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->constructed = photos_base_item_constructed;
  object_class->dispose = photos_base_item_dispose;
  object_class->finalize = photos_base_item_finalize;
  object_class->get_property = photos_base_item_get_property;
  object_class->set_property = photos_base_item_set_property;
  class->open = photos_base_item_default_open;
  class->set_favorite = photos_base_item_default_set_favorite;
  class->update_type_description = photos_base_item_default_update_type_description;

  g_object_class_install_property (object_class,
                                   PROP_CURSOR,
                                   g_param_spec_object ("cursor",
                                                        "TrackerSparqlCursor object",
                                                        "A cursor to iterate over the results of a query",
                                                        TRACKER_SPARQL_TYPE_CURSOR,
                                                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  g_object_class_install_property (object_class,
                                   PROP_FAILED_THUMBNAILING,
                                   g_param_spec_boolean ("failed-thumbnailing",
                                                         "Thumbnailing failed",
                                                         "Failed to create a thumbnail",
                                                         FALSE,
                                                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  g_object_class_install_property (object_class,
                                   PROP_ICON,
                                   g_param_spec_object ("icon",
                                                        "GdkPixbuf object",
                                                        "The thumbnail for this item",
                                                        GDK_TYPE_PIXBUF,
                                                        G_PARAM_READABLE));

  g_object_class_install_property (object_class,
                                   PROP_ID,
                                   g_param_spec_string ("id",
                                                        "Uniform Resource Name",
                                                        "An unique ID associated with this item",
                                                        "",
                                                        G_PARAM_READABLE));

  signals[INFO_UPDATED] = g_signal_new ("info-updated",
                                        G_TYPE_FROM_CLASS (class),
                                        G_SIGNAL_RUN_LAST,
                                        G_STRUCT_OFFSET (PhotosBaseItemClass,
                                                         info_updated),
                                        NULL, /* accumulator */
                                        NULL, /* accu_data */
                                        g_cclosure_marshal_VOID__VOID,
                                        G_TYPE_NONE,
                                        0);

  create_thumbnail_pool = g_thread_pool_new (photos_base_item_create_thumbnail_in_thread_func,
                                             NULL,
                                             1,
                                             FALSE,
                                             NULL);
  g_assert (create_thumbnail_pool != NULL);
}


gboolean
photos_base_item_can_trash (PhotosBaseItem *self)
{
  return self->priv->collection;
}


void
photos_base_item_destroy (PhotosBaseItem *self)
{
  /* TODO: SearchCategoryManager */
  g_clear_object (&self->priv->watcher);
}


gchar *
photos_base_item_download (PhotosBaseItem *self, GCancellable *cancellable, GError **error)
{
  return PHOTOS_BASE_ITEM_GET_CLASS (self)->download(self, cancellable, error);
}


void
photos_base_item_download_async (PhotosBaseItem *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  GTask *task;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_check_cancellable (task, TRUE);
  g_task_set_source_tag (task, photos_base_item_download_async);

  g_task_run_in_thread (task, photos_base_item_download_in_thread_func);
  g_object_unref (task);
}


gchar *
photos_base_item_download_finish (PhotosBaseItem *self, GAsyncResult *res, GError **error)
{
  GTask *task = G_TASK (res);

  g_return_val_if_fail (g_task_is_valid (res, self), NULL);
  g_return_val_if_fail (g_task_get_source_tag (task) == photos_base_item_download_async, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return g_task_propagate_pointer (task, error);
}


const gchar *
photos_base_item_get_author (PhotosBaseItem *self)
{
  return self->priv->author;
}


GeglRectangle
photos_base_item_get_bbox (PhotosBaseItem *self)
{
  return self->priv->bbox;
}


gint64
photos_base_item_get_date_created (PhotosBaseItem *self)
{
  return self->priv->date_created;
}


const gchar *
photos_base_item_get_default_app_name (PhotosBaseItem *self)
{
  return self->priv->default_app_name;
}


GQuark
photos_base_item_get_equipment (PhotosBaseItem *self)
{
  return self->priv->equipment;
}


gdouble
photos_base_item_get_exposure_time (PhotosBaseItem *self)
{
  return self->priv->exposure_time;
}


GQuark
photos_base_item_get_flash (PhotosBaseItem *self)
{
  return self->priv->flash;
}


gdouble
photos_base_item_get_fnumber (PhotosBaseItem *self)
{
  return self->priv->fnumber;
}


gdouble
photos_base_item_get_focal_length (PhotosBaseItem *self)
{
  return self->priv->focal_length;
}


gint64
photos_base_item_get_height (PhotosBaseItem *self)
{
  return self->priv->height;
}


GdkPixbuf *
photos_base_item_get_icon (PhotosBaseItem *self)
{
  return self->priv->icon;
}


const gchar *
photos_base_item_get_id (PhotosBaseItem *self)
{
  return self->priv->id;
}


const gchar *
photos_base_item_get_identifier (PhotosBaseItem *self)
{
  return self->priv->identifier;
}


gdouble
photos_base_item_get_iso_speed (PhotosBaseItem *self)
{
  return self->priv->iso_speed;
}


const gchar *
photos_base_item_get_mime_type (PhotosBaseItem *self)
{
  return self->priv->mime_type;
}


gint64
photos_base_item_get_mtime (PhotosBaseItem *self)
{
  return self->priv->mtime;
}


const gchar *
photos_base_item_get_name (PhotosBaseItem *self)
{
  return self->priv->name;
}


GdkPixbuf *
photos_base_item_get_pristine_icon (PhotosBaseItem *self)
{
  return self->priv->pristine_icon;
}


const gchar *
photos_base_item_get_resource_urn (PhotosBaseItem *self)
{
  return self->priv->resource_urn;
}


const gchar *
photos_base_item_get_source_name (PhotosBaseItem *self)
{
  return PHOTOS_BASE_ITEM_GET_CLASS (self)->get_source_name(self);
}


const gchar *
photos_base_item_get_type_description (PhotosBaseItem *self)
{
  return self->priv->type_description;
}


const gchar *
photos_base_item_get_uri (PhotosBaseItem *self)
{
  return self->priv->uri;
}


gchar *
photos_base_item_get_where (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  gchar *ret_val;

  if (priv->collection)
    ret_val = g_strconcat ("{ ?urn nie:isPartOf <", priv->id, "> }", NULL);
  else
    ret_val = g_strdup ("");

  return ret_val;
}


gint64
photos_base_item_get_width (PhotosBaseItem *self)
{
  return self->priv->width;
}


gboolean
photos_base_item_is_collection (PhotosBaseItem *self)
{
  return self->priv->collection;
}


gboolean
photos_base_item_is_favorite (PhotosBaseItem *self)
{
  return self->priv->favorite;
}


void
photos_base_item_load_async (PhotosBaseItem *self,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  GSimpleAsyncResult *simple;

  g_return_if_fail (PHOTOS_IS_BASE_ITEM (self));

  simple = g_simple_async_result_new (G_OBJECT (self),
                                      callback,
                                      user_data,
                                      photos_base_item_load_async);
  g_simple_async_result_set_check_cancellable (simple, cancellable);

  g_simple_async_result_run_in_thread (simple,
                                       photos_base_item_load_in_thread_func,
                                       G_PRIORITY_DEFAULT,
                                       cancellable);
  g_object_unref (simple);
}


GeglNode *
photos_base_item_load_finish (PhotosBaseItem *self, GAsyncResult *res, GError **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GeglNode *ret_val = NULL;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (self), photos_base_item_load_async), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  ret_val = GEGL_NODE (g_simple_async_result_get_op_res_gpointer (simple));
  g_object_ref (ret_val);

 out:
  return ret_val;
}


void
photos_base_item_open (PhotosBaseItem *self, GdkScreen *screen, guint32 timestamp)
{
  PHOTOS_BASE_ITEM_GET_CLASS (self)->open (self, screen, timestamp);
}


void
photos_base_item_print (PhotosBaseItem *self, GtkWidget *toplevel)
{
  photos_base_item_load_async (self, NULL, photos_base_item_print_load, g_object_ref (toplevel));
}


void
photos_base_item_refresh (PhotosBaseItem *self)
{
  PhotosSingleItemJob *job;

  job = photos_single_item_job_new (self->priv->id);
  photos_single_item_job_run (job, PHOTOS_QUERY_FLAGS_NONE, photos_base_item_refresh_executed, g_object_ref (self));
  g_object_unref (job);
}


void
photos_base_item_set_default_app_name (PhotosBaseItem *self, const gchar *default_app_name)
{
  PhotosBaseItemPrivate *priv = self->priv;

  g_free (priv->default_app_name);
  priv->default_app_name = g_strdup (default_app_name);
}


void
photos_base_item_set_favorite (PhotosBaseItem *self, gboolean favorite)
{
  PHOTOS_BASE_ITEM_GET_CLASS (self)->set_favorite (self, favorite);
}


void
photos_base_item_trash (PhotosBaseItem *self)
{
  PhotosBaseItemPrivate *priv = self->priv;
  PhotosDeleteItemJob *job;

  if (!priv->collection)
    return;

  job = photos_delete_item_job_new (priv->id);
  photos_delete_item_job_run (job, NULL, NULL);
  g_object_unref (job);
}
