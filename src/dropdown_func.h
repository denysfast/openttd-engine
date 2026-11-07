/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file dropdown_func.h Functions related to the drop down widget. */

#ifndef DROPDOWN_FUNC_H
#define DROPDOWN_FUNC_H

#include "core/enum_type.hpp"
#include "dropdown_type.h"
#include "window_gui.h"

/**
 * One row of an open dropdown list, as it is laid out and clipped on screen.
 */
struct DropDownRowGeometry {
	int result = 0;        ///< Result value the row reports when it is chosen.
	Rect rect{};           ///< Screen rectangle of the visible part of the row.
	bool selected = false; ///< Whether the row is drawn as the selected one.
	bool disabled = false; ///< Whether the row is shown but cannot be chosen.
};

/**
 * Geometry of the open dropdown list of one widget of a window.
 */
struct DropDownListGeometry {
	bool open = false;                     ///< Whether a dropdown list of that widget is open.
	Rect rect{};                           ///< Screen rectangle of the dropdown window.
	Rect items{};                          ///< Screen rectangle of the clipped item area.
	std::vector<DropDownRowGeometry> rows; ///< Rows that are visible right now, top to bottom.
};

/**
 * Get the geometry of the dropdown list a window opened for one of its widgets.
 *
 * Only the list that is open right now is reported, and only when it was opened by \a parent for
 * \a button: the dropdown of another window is not the list of that widget. The rows are the items
 * that are visible after filtering and scrolling, with the rectangle they occupy on screen clipped
 * to the item area, which is the same layout the drawing and the hit testing use. Nothing here
 * changes the list or the window.
 * @param parent The window that opened the list.
 * @param button The widget of that window the list belongs to.
 * @return The geometry; \c open is false when no such list is open.
 */
DropDownListGeometry GetOpenDropDownListGeometry(const Window *parent, WidgetID button);

/* Show drop down menu containing a fixed list of strings */
void ShowDropDownMenu(Window *w, std::span<const StringID> strings, int selected, WidgetID button, uint32_t disabled_mask, uint32_t hidden_mask, uint width = 0, DropDownOptions options = {}, std::string * const persistent_filter_text = nullptr);

/* Helper functions for commonly used drop down list items. */
std::unique_ptr<DropDownListItem> MakeDropDownListDividerItem();
std::unique_ptr<DropDownListItem> MakeDropDownListStringItem(StringID str, int value, bool masked = false, bool shaded = false);
std::unique_ptr<DropDownListItem> MakeDropDownListStringItem(std::string &&str, int value, bool masked = false, bool shaded = false);
std::unique_ptr<DropDownListItem> MakeDropDownListIconItem(SpriteID sprite, PaletteID palette, StringID str, int value, bool masked = false, bool shaded = false);
std::unique_ptr<DropDownListItem> MakeDropDownListIconItem(const Dimension &dim, SpriteID sprite, PaletteID palette, StringID str, int value, bool masked = false, bool shaded = false);
std::unique_ptr<DropDownListItem> MakeDropDownListCheckedItem(bool checked, StringID str, int value, bool masked = false, bool shaded = false, uint indent = 0);

/** @copydoc MakeDropDownListStringItem */
template <typename EnumType> requires is_scoped_enum_v<EnumType>
inline std::unique_ptr<DropDownListItem> MakeDropDownListStringItem(StringID str, EnumType value, bool masked = false, bool shaded = false)
{
	return MakeDropDownListStringItem(str, to_underlying(value), masked, shaded);
}

/** @copydoc MakeDropDownListStringItem */
template <typename EnumType> requires is_scoped_enum_v<EnumType>
inline std::unique_ptr<DropDownListItem> MakeDropDownListStringItem(std::string &&str, EnumType value, bool masked = false, bool shaded = false)
{
	return MakeDropDownListStringItem(std::move(str), to_underlying(value), masked, shaded);
}

/** @copydoc MakeDropDownListIconItem */
template <typename EnumType> requires is_scoped_enum_v<EnumType>
inline std::unique_ptr<DropDownListItem> MakeDropDownListIconItem(SpriteID sprite, PaletteID palette, StringID str, EnumType value, bool masked = false, bool shaded = false)
{
	return MakeDropDownListIconItem(sprite, palette, str, to_underlying(value), masked, shaded);
}

/** @copydoc MakeDropDownListIconItem(const Dimension &, SpriteID, PaletteID, StringID, int, bool, bool) */
template <typename EnumType> requires is_scoped_enum_v<EnumType>
inline std::unique_ptr<DropDownListItem> MakeDropDownListIconItem(const Dimension &dim, SpriteID sprite, PaletteID palette, StringID str, EnumType value, bool masked = false, bool shaded = false)
{
	return MakeDropDownListIconItem(dim, sprite, palette, str, to_underlying(value), masked, shaded);
}

/** @copydoc MakeDropDownListCheckedItem */
template <typename EnumType> requires is_scoped_enum_v<EnumType>
inline std::unique_ptr<DropDownListItem> MakeDropDownListCheckedItem(bool checked, StringID str, EnumType value, bool masked = false, bool shaded = false, uint indent = 0)
{
	return MakeDropDownListCheckedItem(checked, str, to_underlying(value), masked, shaded, indent);
}

#endif /* DROPDOWN_FUNC_H */
