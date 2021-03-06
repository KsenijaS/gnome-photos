/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2014 Red Hat, Inc.
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

#ifndef PHOTOS_DROPDOWN_H
#define PHOTOS_DROPDOWN_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PHOTOS_TYPE_DROPDOWN (photos_dropdown_get_type ())

#define PHOTOS_DROPDOWN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
   PHOTOS_TYPE_DROPDOWN, PhotosDropdown))

#define PHOTOS_DROPDOWN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
   PHOTOS_TYPE_DROPDOWN, PhotosDropdownClass))

#define PHOTOS_IS_DROPDOWN(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
   PHOTOS_TYPE_DROPDOWN))

#define PHOTOS_IS_DROPDOWN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
   PHOTOS_TYPE_DROPDOWN))

#define PHOTOS_DROPDOWN_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
   PHOTOS_TYPE_DROPDOWN, PhotosDropdownClass))

typedef struct _PhotosDropdown        PhotosDropdown;
typedef struct _PhotosDropdownClass   PhotosDropdownClass;
typedef struct _PhotosDropdownPrivate PhotosDropdownPrivate;

struct _PhotosDropdown
{
  GtkPopover parent_instance;
  PhotosDropdownPrivate *priv;
};

struct _PhotosDropdownClass
{
  GtkPopoverClass parent_class;
};

GType                     photos_dropdown_get_type             (void) G_GNUC_CONST;

GtkWidget                *photos_dropdown_new                  (GtkWidget *relative_to);

G_END_DECLS

#endif /* PHOTOS_DROPDOWN_H */
