/*
 * xkb.c - keyboard layout control functions
 *
 * Copyright © 2015 Aleksey Fedotov <lexa@cfotr.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "xkb.h"
#include "globalconf.h"
#include <xcb/xkb.h>


/* \brief switch keyboard layout
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lparam layout number, integer from 0 to 3
 */
int
luaA_xkb_set_layout_group(lua_State *L)
{
    unsigned group = luaL_checkinteger(L, 1);
    xcb_xkb_latch_lock_state (globalconf.connection, XCB_XKB_ID_USE_CORE_KBD,
                              0, 0, true, group, 0, 0, 0);
    return 0;
}

/* \brief get current layout number
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lreturn current layout number, integer from 0 to 3
 */
int
luaA_xkb_get_layout_group(lua_State *L)
{
    xcb_xkb_get_state_cookie_t state_c;
    state_c = xcb_xkb_get_state_unchecked (globalconf.connection,
                                           XCB_XKB_ID_USE_CORE_KBD);
    xcb_xkb_get_state_reply_t* state_r;
    state_r = xcb_xkb_get_state_reply (globalconf.connection,
                                       state_c, NULL);
    if (!state_r)
    {
        free(state_r);
        return 0;
    }
    lua_pushinteger(L, state_r->group);
    free(state_r);
    return 1;
}

/* \brief get layout short names
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lreturn string describing current layout settings, \
 * example: 'pc+us+de:2+inet(evdev)+group(alt_shift_toggle)+ctrl(nocaps)'
 */
int
luaA_xkb_get_group_names(lua_State *L)
{
    xcb_xkb_get_names_cookie_t name_c;
    name_c = xcb_xkb_get_names_unchecked (globalconf.connection,
                                          XCB_XKB_ID_USE_CORE_KBD,
                                          XCB_XKB_NAME_DETAIL_SYMBOLS);
    xcb_xkb_get_names_reply_t* name_r;
    name_r = xcb_xkb_get_names_reply (globalconf.connection, name_c, NULL);

    if (!name_r)
    {
        luaA_warn(L, "Failed to get xkb symbols name");
        return 0;
    }

    xcb_xkb_get_names_value_list_t name_list;
    void *buffer = xcb_xkb_get_names_value_list(name_r);
    xcb_xkb_get_names_value_list_unpack (
        buffer, name_r->nTypes, name_r->indicators,
        name_r->virtualMods, name_r->groupNames, name_r->nKeys,
        name_r->nKeyAliases, name_r->nRadioGroups, name_r->which,
        &name_list);

    xcb_get_atom_name_cookie_t atom_name_c;
    atom_name_c = xcb_get_atom_name_unchecked(globalconf.connection, name_list.symbolsName);
    xcb_get_atom_name_reply_t *atom_name_r;
    atom_name_r = xcb_get_atom_name_reply(globalconf.connection, atom_name_c, NULL);
    if (!atom_name_r) {
        luaA_warn(L, "Failed to get atom symbols name");
        free(name_r);
        return 0;
    }

    const char *name = xcb_get_atom_name_name(atom_name_r);
    size_t name_len = xcb_get_atom_name_name_length(atom_name_r);
    lua_pushlstring(L, name, name_len);

    free(atom_name_r);
    free(name_r);
    return 1;
}

/** The xkb notify event handler.
 * \param event The event.
 */
void event_handle_xkb_notify(xcb_generic_event_t* event)
{
    lua_State *L = globalconf_get_lua_State();
    /* The pad0 field of xcb_generic_event_t contains the event sub-type,
     * unfortunately xkb doesn't provide a usable struct for getting this in a
     * nicer way*/
    switch (event->pad0)
    {
      case XCB_XKB_NEW_KEYBOARD_NOTIFY:
        {
          xcb_xkb_new_keyboard_notify_event_t *new_keyboard_event = (void*)event;
          if (new_keyboard_event->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
          {
              signal_object_emit(L, &global_signals, "xkb::map_changed", 0);
          }
          break;
        }
      case XCB_XKB_NAMES_NOTIFY:
        {
          signal_object_emit(L, &global_signals, "xkb::map_changed", 0);
          break;
        }
      case XCB_XKB_STATE_NOTIFY:
        {
          xcb_xkb_state_notify_event_t *state_notify_event = (void*)event;
          if (state_notify_event->changed & XCB_XKB_STATE_PART_GROUP_STATE)
          {
              lua_pushnumber(L, state_notify_event->group);
              signal_object_emit(L, &global_signals, "xkb::group_changed", 1);
          }
          break;
        }
    }
}

/** Initialize XKB support
 */

void xkb_init(void)
{
    /* check that XKB extension present in this X server */
    const xcb_query_extension_reply_t *xkb_r;
    xkb_r = xcb_get_extension_data(globalconf.connection, &xcb_xkb_id);
    if (!xkb_r || !xkb_r->present)
    {
        fatal("Xkb extension not present");
    }

    /* check xkb version */
    xcb_xkb_use_extension_cookie_t ext_cookie = xcb_xkb_use_extension(globalconf.connection, 1, 0);
    xcb_xkb_use_extension_reply_t *ext_reply = xcb_xkb_use_extension_reply (globalconf.connection, ext_cookie, NULL);
    if (!ext_reply || !ext_reply->supported)
    {
        fatal("Required xkb extension is not supported");
    }
    unsigned int map = XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY;
    xcb_xkb_select_events_checked(globalconf.connection,
                                  XCB_XKB_ID_USE_CORE_KBD,
                                  map,
                                  0,
                                  map,
                                  0,
                                  0,
                                  0);

}
