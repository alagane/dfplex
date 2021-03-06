/*
 * Dwarfplex is based on Webfort, created by Vitaly Pronkin on 14/05/14.
 * Copyright (c) 2014 mifki, ISC license.
 * Copyright (c) 2020 white-rabbit, ISC license
 */
 
#include "Client.hpp"
#include "command.hpp"
#include "config.hpp"
#include "dfplex.hpp"
#include "hackutil.hpp"
#include "input.hpp"
#include "keymap.hpp"
#include "screenbuf.hpp"
#include "server.hpp"
#include "state.hpp"

#include <stdint.h>
#include <iostream>
#include <map>
#include <vector>
#include <list>
#include <cassert>

#include "tinythread.h"
#include "MemAccess.h"
#include "modules/EventManager.h"
#include "modules/Gui.h"
#include "modules/MapCache.h"
#include "modules/Screen.h"
#include "modules/Units.h"
#include "modules/World.h"
#include "PluginManager.h"

#include "df/announcement_flags.h"
#include "df/announcements.h"
#include "df/building.h"
#include "df/buildings_other_id.h"
#include "df/d_init.h"
#include "df/enabler.h"
#include "df/graphic.h"
#include "df/historical_figure.h"
#include "df/interfacest.h"
#include "df/items_other_id.h"
#include "df/renderer.h"
#include "df/report.h"
#include "df/squad_position.h"
#include "df/squad.h"
#include "df/ui_build_selector.h"
#include "df/ui_sidebar_menus.h"
#include "df/ui_unit_view_mode.h"
#include "df/unit.h"
#include "df/viewscreen_announcelistst.h"
#include "df/viewscreen_createquotast.h"
#include "df/viewscreen_customize_unitst.h"
#include "df/viewscreen_civlistst.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/viewscreen_loadgamest.h"
#include "df/viewscreen_meetingst.h"
#include "df/viewscreen_movieplayerst.h"
#include "df/viewscreen_optionst.h"
#include "df/viewscreen_reportlistst.h"
#include "df/viewscreen_textviewerst.h"
#include "df/viewscreen_tradelistst.h"
#include "df/viewscreen_titlest.h"
#include "df/viewscreen_unitst.h"
#include "df/viewscreen.h"
#include "df/world.h"

using namespace DFHack;
using namespace df::enums;
using df::global::world;
using std::string;
using std::vector;
using df::global::gps;
using df::global::init;

using df::global::enabler;
using df::renderer;

typedef bool (*getcoords_t)(int32_t& x, int32_t& y, int32_t& z);
static void getOptCoord(bool& io_set, Coord& o_coord, getcoords_t getcoords)
{
    Coord c;
    (void)getcoords(c.x, c.y, c.z);
    if (c || io_set)
    {
        io_set = true;
        o_coord = std::move(c);
    }
}

// helper function for restore_state.
static void restore_cursor(Client* client)
{
    UIState& ui = client->ui;
    
    // sets viewcoords
    if (ui.m_viewcoord_set)
    {
        Gui::setViewCoords(ui.m_viewcoord.x, ui.m_viewcoord.y, ui.m_viewcoord.z);
    }
    if (ui.m_following_client && ui.m_client_screen_cycle.get())
    {
        Client* dst = get_client(ui.m_client_screen_cycle.get());
        if (dst)
        {
            center_view_on_coord(dst->ui.m_stored_viewcoord.operator+(
                {dst->ui.m_map_dimx/2, dst->ui.m_map_dimy/2, 0})
            );
        }
    }
    
    // sets cursor coords
    if (ui.m_cursorcoord_set)
    {
        Gui::setCursorCoords(ui.m_cursorcoord.x, ui.m_cursorcoord.y, ui.m_cursorcoord.z);
    }
    if (ui.m_designationcoord_set)
    {
        Gui::setDesignationCoords(ui.m_designationcoord.x, ui.m_designationcoord.y, ui.m_designationcoord.z);
    }
    if (ui.m_squadcoord_start_set)
    {
        df::global::ui->squads.rect_start.x = ui.m_squadcoord_start.x;
        df::global::ui->squads.rect_start.y = ui.m_squadcoord_start.y;
        df::global::ui->squads.rect_start.z = ui.m_squadcoord_start.z;
    }
    if (ui.m_burrowcoord_set)
    {
        df::global::ui->burrows.rect_start.x = ui.m_burrowcoord.x;
        df::global::ui->burrows.rect_start.y = ui.m_burrowcoord.y;
        df::global::ui->burrows.rect_start.z = ui.m_burrowcoord.z;
    }
    df::global::ui->burrows.brush_erasing = ui.m_brush_erasing;
}

std::string UIState::debug_trace() const
{
    std::string trace = "";
    trace += "\nKey queue is:";
    size_t i = 0;
    for (const RestoreKey& key : m_restore_keys)
    {
        trace += "\n" + std::to_string(i);
        if (i == m_restore_progress && m_restore_progress_root > 0)
        {
            trace += " *";
        }
        else
        {
            trace += "  ";
        }
        trace += keybindings.getCommandNames(key.m_interface_keys);
        if (key.m_check_state)
        {
            trace += " -> " + key.m_post_menu + " +" + std::to_string(key.m_post_menu_depth);
        }
        if (key.m_observed_menu != "" && key.m_observed_menu != key.m_post_menu)
        {
            trace += " ";
            if (key.m_catch_observed_autorewind) trace += "[";
            trace += "[this frame: " + key.m_observed_menu + " +" + std::to_string(key.m_observed_menu_depth) + "]";
            if (key.m_catch_observed_autorewind) trace += "]";
        }
        if (key.m_pre_restore_cursor)
        {
            trace += " +pre-cursor"; // FIXME: write coordinates here
        }
        if (key.m_post_restore_cursor)
        {
            trace += " +post-cursor";
        }
        if (key.m_suppress_sidebar_refresh)
        {
            trace += " +no-sidebar-refresh";
        }
        if (key.m_freeze_cursor)
        {
            trace += " +freeze-cursor";
        }
        if (key.m_restore_squad_state)
        {
            trace += " +squad";
        }
        if (key.m_catch)
        {
            trace += " +catchpoint";
        }
        if (key.m_catch || key.m_check_state)
        {
            trace += " (from " + std::to_string(key.m_check_start) + ")";
        }
        
        ++i;
    }
    
    return trace;
}

bool menu_id_matches(const menu_id& a, const menu_id& b)
{
    if (a == K_NOCHECK) return true;
    
    if (a == b) return true;
    
    // negation
    if (startsWith(a, "^"))
    {
        return a.substr(1) != b;
    }
    
    return false;
}

// returns false on error.
static bool restore_unit_view_state(Client* client)
{
    using namespace df::enums::interface_key;
    
    df::viewscreen* vs;
    virtual_identity* id;
    (void)id;
    UPDATE_VS(vs, id);
    
    UIState& ui = client->ui;
    df::global::ui_unit_view_mode->value = ui.m_unit_view_mode;
    df::global::ui_sidebar_menus->show_combat = ui.m_show_combat;
    df::global::ui_sidebar_menus->show_labor = ui.m_show_labor;
    df::global::ui_sidebar_menus->show_misc = ui.m_show_misc;

    // set df::global::ui_selected_unit
    if (df::unit* unit = df::unit::find(ui.m_view_unit))
    {
        // go to unit's position.
        Coord pos = unit->pos;
        Gui::setCursorCoords(pos.x, pos.y, pos.z);
        Gui::refreshSidebar();
        
        // rapidly tap UNITVIEW_NEXT until the unit we desire is found.
        vs->feed_key(UNITVIEW_NEXT);
        int32_t unit_sel_start = *df::global::ui_selected_unit;
        bool success;
        while (true)
        {
            vs->feed_key(UNITVIEW_NEXT);
            if (df::unit* unit_selected = vector_get(world->units.active, *df::global::ui_selected_unit))
            {
                if (unit_selected->id == unit->id)
                {
                    success = true;
                    break;
                }
            }
            if (unit_sel_start == *df::global::ui_selected_unit)
            {
                success = false;
                break;
            }
        }
        
        if (!success)
        {
            // return to the stored cursor position.
            restore_cursor(client);
            Gui::refreshSidebar();
            return false;
        }
        else
        {
            ui.m_defer_restore_cursor = true;
            ui.m_suppress_sidebar_refresh = true;
            
            // restore labour menu position
            if (ui.m_unit_view_mode == df::ui_unit_view_mode::PrefLabor)
            {
                vs->feed_key(UNITVIEW_PRF);
                vs->feed_key(UNITVIEW_PRF_PROF);
                
                // set scroll position
                if (ui.m_view_unit_labor_submenu >= 0)
                {
                    for (int32_t i = 0; i < ui.m_view_unit_labor_submenu; ++i)
                    {
                        vs->feed_key(SECONDSCROLL_DOWN);
                    }
                    vs->feed_key(SELECT);
                }
                for (int32_t i = 0; i < ui.m_view_unit_labor_scroll; ++i)
                {
                    vs->feed_key(SECONDSCROLL_DOWN);
                }
            }
            return true;
        }
    }
    return false;
}

static void restore_squads_state(Client* client)
{
    UIState& ui = client->ui;
    
    auto& squads = df::global::ui->squads;
    auto& ui_squads = ui.m_squads;
    
    // need to clear this on entry.
    squads.in_kill_rect = false;
    squads.in_kill_order = false;
    squads.in_kill_list = false;
    squads.in_move_order = false;
    
    // selected individuals
    squads.in_select_indiv = ui_squads.in_select_indiv;
    
    // sanitize
    squads.indiv_selected.clear();
    for (int32_t figure_id : ui_squads.indiv_selected)
    {
        df::historical_figure* figure = df::historical_figure::find(figure_id);
        if (!figure) continue;
        df::unit* unit = df::unit::find(figure->unit_id);
        if (!unit) continue;
        
        // ensure unit still part of a squad.
        bool found_squad = false;
        for (df::squad* squad : squads.list)
        {
            if (squad && squad->id == unit->military.squad_id)
            {
                found_squad = true;
                break;
            }
        }
        if (!found_squad) continue;
        
        // ensure unit is assigned to a squad position.
        df::squad* squad = df::squad::find(unit->military.squad_id);
        if (!squad) continue;
        
        bool found_position = false;
        for (df::squad_position* squad_position : squad->positions)
        {
            if (squad_position && squad_position->occupant == figure_id)
            {
                found_position = true;
            }
        }
        if (!found_position) continue;
        
        // we were unable to prove that this figure is not valid to select,
        // so we will allow this figure to remain selected.
        squads.indiv_selected.push_back(figure_id);
    }
    
    // selected squads
    for (size_t i = 0; i < squads.list.size() && i < squads.sel_squads.size(); ++i)
    {
        squads.sel_squads.at(i) = false;
        
        df::squad* squad = squads.list.at(i);
        if (!squad) continue;
        
        squads.sel_squads.at(i) =
            (
                std::find(ui_squads.squad_selected.begin(), ui_squads.squad_selected.end(), squad->id)
                != ui_squads.squad_selected.end()
            );
    }
}

// some squads state must be done after 
static void restore_squads_state_post(Client* client)
{
    UIState& ui = client->ui;
    
    auto& squads = df::global::ui->squads;
    auto& ui_squads = ui.m_squads;
    
    // selected kill targets
    if (squads.in_kill_list || squads.in_kill_rect)
    {
        const std::vector<df::unit*>& targets = (squads.in_kill_rect)
            ? squads.kill_rect_targets
            : squads.kill_targets;
        for (size_t i = 0; i < targets.size() && i < squads.sel_kill_targets.size(); ++i)
        {
            squads.sel_kill_targets.at(i) = false;
            
            df::unit* unit = targets.at(i);
            if (!unit) continue;
            
            squads.sel_kill_targets.at(i) =
                (
                    std::find(ui_squads.kill_selected.begin(), ui_squads.kill_selected.end(), unit->id)
                    != ui_squads.kill_selected.end()
                );
        }
    }
}

// helper function for restore_state.
static void restore_data(Client* client)
{
    df::viewscreen* vs;
    virtual_identity* id;
    UPDATE_VS(vs, id);
    (void)id;
    
    UIState& ui = client->ui;
    Gui::setMenuWidth(ui.m_menu_width, ui.m_area_map_width);
    df::global::ui_sidebar_menus->designation.marker_only = ui.m_designate_marker;
    df::global::ui_sidebar_menus->designation.priority_set = ui.m_designate_priority_set;
    df::global::ui_sidebar_menus->designation.priority = ui.m_designate_priority;
    df::global::ui_sidebar_menus->location.in_create = false;
    df::global::ui_sidebar_menus->location.in_choose_deity = false;
}

// restores some state that comes after the keyqueue
static void restore_post_state(Client* client)
{
    UIState& ui = client->ui;
    if (df::global::ui_building_in_resize)
    {
        *df::global::ui_building_in_resize = ui.m_building_in_resize;
        *df::global::ui_building_resize_radius = ui.m_building_resize_radius;
    }
    
    restore_squads_state_post(client);
}

// helper function for restore_state
// returns true on error.
static bool stabilize_list_menu(Client* client)
{
    using namespace df::enums::interface_key;
    UIState& ui = client->ui;
        
    df::viewscreen* vs;
    virtual_identity* id;
    UPDATE_VS(vs, id);
    
    size_t vs_depth = get_vs_depth(vs);
    if (vs_depth >= ui.m_list_cursor.size()) return false;
    
    int32_t list_cursor = ui.m_list_cursor.at(vs_depth);
    
    if (list_cursor == -1) return false;
    
    if (id == &df::viewscreen_announcelistst::_identity)
    {
        df::viewscreen_announcelistst* vs_a = static_cast<df::viewscreen_announcelistst*>(vs);
        int32_t index = 0;
        for (df::report* report : vs_a->reports)
        {
            if (report->id == list_cursor)
            {
                vs_a->sel_idx = index;
                
                // refresh
                vs->feed_key(STANDARDSCROLL_UP);
                vs->feed_key(STANDARDSCROLL_DOWN);
                return false;
            }
            index++;
        }
        
        goto fail;
    }
    if (id == &df::viewscreen_reportlistst::_identity)
    {
        #if 0
        df::viewscreen_reportlistst* vs_r = static_cast<df::viewscreen_reportlistst*>(vs);
        int32_t index = 0;
        for (df::report* report : df::global::world->status.reports)
        {
            if (report->id == list_cursor)
            {
                vs_r->cursor = index;
                // refresh
                vs->feed_key(STANDARDSCROLL_UP);
                vs->feed_key(STANDARDSCROLL_DOWN);
                return false;
            }
            index++;
        }
        #endif
        
        goto fail;
    }
    if (id == &df::viewscreen_civlistst::_identity)
    {
        if (ui.m_civ_x != -1 && ui.m_civ_y != -1)
        {
            df::viewscreen_civlistst* vs_c = static_cast<df::viewscreen_civlistst*>(vs);
            vs_c->map_x = ui.m_civ_x;
            vs_c->map_y = ui.m_civ_y;
        }
    }
    
    return false;
    
fail:
    ui.m_list_cursor.resize(vs_depth);
    return true;
}

bool tradelist_advance(Client* client)
{
    df::viewscreen* vs;
    virtual_identity* id;
    UPDATE_VS(vs, id);
    if (id == &df::viewscreen_tradelistst::_identity)
    {
        // apply this 32 times for paranoia.
        for (size_t i = 0; i < 32; ++i)
        {
            vs->logic();
            UPDATE_VS(vs, id);
        }
        return true;
    }
    return false;
}

// restores UI/view state for client
// -- preconditions --
// must be in the dfmode root menu.
// -- return value --
// returns true if (seemingly) successful, false if the state
// was not successfully restored.
std::string restore_state_error;
RestoreResult restore_state(Client* client)
{
    restore_state_error = "";
    UIState& ui = client->ui;
    
    if (ui.m_restore_progress_root < UIState::K_RESTORE_PROGRESS_ROOT_MAX)
    {
        if (return_to_root())
        {
            // move on to next part (applying keys)
            ui.m_restore_progress_root = UIState::K_RESTORE_PROGRESS_ROOT_MAX;
        }
        else
        {
            // clearly we aren't able to get back to the root for some reason.
            // abort and totally reset ui state for safety.
            ui.reset();
            restore_state_error = "Failed to return to root.";
            return RestoreResult::ABORT_PLEX;
        }
    }
    
    restore_data(client);
    
    df::viewscreen* vs;
    virtual_identity* id;
    UPDATE_VS(vs, id);
    (void)id;
    
    // check for keystack overflow
    if (ui.m_restore_keys.size() > KEYSTACK_MAX && KEYSTACK_MAX > 0)
    {
        restore_state_error = "Key stack overflow (" + std::to_string(ui.m_restore_keys.size()) + ")";
        ui.m_restore_keys.clear();
        return RestoreResult::FAIL;
    }
    
    // apply state keys
    while (ui.m_restore_progress < ui.m_restore_keys.size())
    {
        RestoreKey& rkey = ui.m_restore_keys.at(ui.m_restore_progress);
        
        // feed requires a non-const pointer to the set, so we copy it here.
        // OPTIMIZE: is this really necessary?
        std::set<df::interface_key> keys = rkey.m_interface_keys;
        
        if (rkey.m_suppress_sidebar_refresh)
        {
            ui.m_suppress_sidebar_refresh = true;
        }
        
        if (rkey.m_pre_restore_cursor)
        {
            // note: key's cursor coords.
            Gui::setCursorCoords(rkey.m_cursor.x, rkey.m_cursor.y, rkey.m_cursor.z);
            Gui::refreshSidebar();
        }
        
        if (!keys.empty())
        {
            vs->feed(&keys);
            UPDATE_VS(vs, id);
        }
        
        if (rkey.m_post_restore_cursor)
        {
            // note: ui cursor coords.
            // Gui::setCursorCoords(ui.m_cursorcoord.x, ui.m_cursorcoord.y, ui.m_cursorcoord.z);
            restore_cursor(client);
            Gui::refreshSidebar();
        }
        
        if (rkey.m_freeze_cursor)
        {
            ui.m_freeze_cursor = true;
        }
        
        if (rkey.m_restore_unit_view_state)
        {
            restore_unit_view_state(client);
        }
        
        if (rkey.m_restore_stockpile_state)
        {
            if (ui.m_custom_stockpile_set)
            {
                df::global::ui->stockpile.custom_settings = ui.m_custom_stockpile;
            }
        }
        
        // change the restorekey's observed menu.
        menu_id menu_id = get_current_menu_id();
        size_t menu_depth = get_vs_depth(vs);
        
        if (stabilize_list_menu(client))
        {
            restore_state_error = "Failed to stabilize cursor in " + menu_id;
            
            // erase past after this key.
            ui.m_restore_keys.erase(
                ui.m_restore_keys.begin() + ui.m_restore_progress + 1,
                ui.m_restore_keys.end()
            );
            
            return RestoreResult::FAIL;
        }

        if (rkey.m_blockcatch)
        {
            if (rkey.m_observed_menu != menu_id || rkey.m_observed_menu_depth != menu_depth)
            {
                goto stack_error;
            }
        }
        rkey.m_observed_menu = menu_id;
        rkey.m_observed_menu_depth = menu_depth;
        
        if (rkey.m_restore_squad_state)
        {
            restore_squads_state(client);
        }
        
        // validate that we have ended up in the right spot.
        if (rkey.m_check_state)
        {
            if (menu_depth != rkey.m_post_menu_depth || !menu_id_matches(rkey.m_post_menu, menu_id))
            {
                // we did not arrive at the menu we expected to :(
            stack_error:
                
                // error trace
                if (rkey.m_check_state)
                {
                    restore_state_error = "Expected " + rkey.m_post_menu + " +" + std::to_string(rkey.m_post_menu_depth);
                }
                else if (rkey.m_blockcatch)
                {
                    restore_state_error = "Expected observed " + rkey.m_observed_menu + " +" + std::to_string(rkey.m_observed_menu_depth);
                    rkey.m_observed_menu = menu_id;
                    rkey.m_observed_menu_depth = menu_depth;
                }
                else
                {
                    restore_state_error = "Unkown reason";
                }
                
                restore_state_error += "; arrived at " + menu_id + " +" + std::to_string(menu_depth);
            
                restore_state_error += "\n" + ui.debug_trace();
                restore_state_error += "\n(erasing from " + std::to_string(rkey.m_check_start) + " on)\n";
                
                // remove all keys past rkey.m_check_start
                ui.m_restore_keys.erase(
                    ui.m_restore_keys.begin() + rkey.m_check_start,
                    ui.m_restore_keys.end()
                );
                
                // we erased to before the current spot, so we have to retry as
                // we have no ability to accurately rewind.
                if (rkey.m_check_start <= ui.m_restore_progress)
                {
                    // fail and try again from start next time.
                    ui.next();
                    return RestoreResult::FAIL;
                }
            }
        }
        
        // tradelist immediately advances to a new screen
        // ometimes, so we must simulate that.
        if (tradelist_advance(client))
        {
            UPDATE_VS(vs, id);
            //vs->feed_key(df::enums::interface_key::STANDARDSCROLL_DOWN);
        }
        
        ui.m_restore_progress++;
    }
    
    if (!ui.m_defer_restore_cursor && !ui.m_freeze_cursor)
    {
        restore_cursor(client);
    }
    restore_post_state(client);
    
    if (!ui.m_suppress_sidebar_refresh)
    {
        Gui::refreshSidebar();
    }
    
    // reset these for next stack traversal.
    ui.next();
    return RestoreResult::SUCCESS;
}

static bool isBuildMenu(){
    return df::global::ui->main.mode == df::enums::ui_sidebar_mode::Build;
}

static bool isBuildPositionMenu(){
    using df::global::ui_build_selector;
    if (ui_build_selector)
    {
        // Not selecting, or no choices?
        if (ui_build_selector->building_type < 0)
            return false;
        else if (ui_build_selector->stage != 2)
        {
            if (ui_build_selector->stage != 1)
                return false;
            else
                return true;
        }
    }
    return false;
}

// captures some of the state of the UI.
// does not capture the restore keypresses -- those are processed
// online in command.cpp
void capture_post_state(Client* client)
{
    df::viewscreen* vs;
    virtual_identity* id;
    UPDATE_VS(vs, id);
    
    UIState& ui = client->ui;
    
    if (!ui.m_freeze_cursor)
    {
        if (!ui.m_following_client)
        {
            const bool prev_set = ui.m_viewcoord_set;
            const Coord prev_coord = ui.m_viewcoord;
            getOptCoord(ui.m_viewcoord_set, ui.m_viewcoord, Gui::getViewCoords);
            
            // update stored viewcoord if the camera moves.
            if (!prev_set)
            {
                ui.m_stored_viewcoord = ui.m_viewcoord;
            }
            // For some reason, explicitly using operator!= is required here.
            else if (ui.m_viewcoord.operator!=(prev_coord) && !ui.m_stored_viewcoord_skip)
            {
                ui.m_stored_viewcoord = ui.m_viewcoord;
                ui.m_stored_camera_return = false;
            }
            ui.m_stored_viewcoord_skip = false;
        }
        
        getOptCoord(ui.m_cursorcoord_set, ui.m_cursorcoord, Gui::getCursorCoords);
        getOptCoord(ui.m_designationcoord_set, ui.m_designationcoord, Gui::getDesignationCoords);
        ui.m_burrowcoord_set = true;
        ui.m_burrowcoord = df::global::ui->burrows.rect_start;
        if (is_at_root())
        {
            // this is a hacky fix for the lingering cursor bug
            ui.m_cursorcoord_set = false;
            ui.m_designationcoord_set = false;
        }

        // build position menu
        menu_id menu =  get_current_menu_id();

        if (ui.m_cursorcoord_set && id == &df::viewscreen_dwarfmodest::_identity
            && isBuildMenu()
            && isBuildPositionMenu())
        {
            // save and restore build menu cursor
            ui.m_buildcoord_set = true;
            ui.m_buildcoord = ui.m_cursorcoord;
        } else if (id != &df::viewscreen_dwarfmodest::_identity || !isBuildMenu()){
            ui.m_buildcoord_set = false;
        }

        ui.m_squadcoord_start_set = true;
        ui.m_squadcoord_start.x = df::global::ui->squads.rect_start.x;
        ui.m_squadcoord_start.y = df::global::ui->squads.rect_start.y;
        ui.m_squadcoord_start.z = df::global::ui->squads.rect_start.z;
    }
    ui.m_freeze_cursor = false;
    
    ui.m_designate_marker = df::global::ui_sidebar_menus->designation.marker_only;
    ui.m_designate_priority_set = df::global::ui_sidebar_menus->designation.priority_set;
    ui.m_designate_priority = df::global::ui_sidebar_menus->designation.priority;
    
    if (df::global::ui->main.mode == df::enums::ui_sidebar_mode::Stockpiles)
    {
        ui.m_custom_stockpile_set = true;
        ui.m_custom_stockpile = df::global::ui->stockpile.custom_settings;
    }

    // map view dimensions
    if (is_dwarf_mode())
    {
        auto dims = Gui::getDwarfmodeViewDims();
        ui.m_map_dimx = dims.map_x2 - dims.map_x1;
        ui.m_map_dimy = dims.y2 - dims.y1;
    }
    else
    {
        ui.m_map_dimx = ui.m_map_dimy = -1;
    }

    // burrows
    ui.m_brush_erasing = df::global::ui->burrows.brush_erasing;
    
    // unit view menu
    ui.m_unit_view_mode = df::global::ui_unit_view_mode->value;
    ui.m_show_combat = df::global::ui_sidebar_menus->show_combat;
    ui.m_show_labor = df::global::ui_sidebar_menus->show_labor;
    ui.m_show_misc = df::global::ui_sidebar_menus->show_misc;
    if (id == &df::viewscreen_dwarfmodest::_identity)
    {
        ui.m_view_unit = -1;
        ui.m_view_unit_labor_scroll = 0;
        ui.m_view_unit_labor_submenu = -1;
        if (id == &df::viewscreen_dwarfmodest::_identity
            && df::global::ui->main.mode == df::ui_sidebar_mode::ViewUnits)
        {
            if (df::global::ui_selected_unit)
            {
                int32_t selected_unit = *df::global::ui_selected_unit;
                if (df::unit* unit = vector_get(world->units.active, selected_unit))
                {
                    ui.m_view_unit = unit->id;
                }
            }
            if (df::global::ui_unit_view_mode && df::global::ui_unit_view_mode->value == df::ui_unit_view_mode::PrefLabor)
            {
                df::viewscreen_dwarfmodest* vs_dwarf = static_cast<df::viewscreen_dwarfmodest*>(vs);
                
                ui.m_view_unit_labor_scroll = *df::global::ui_look_cursor;
                if (vs_dwarf->sideSubmenu)
                {
                    ui.m_view_unit_labor_submenu = vs_dwarf->unit_labors_sidemenu_uplevel_idx;
                }
                else
                {
                    ui.m_view_unit_labor_submenu = -1;
                }
            }
        }
    }
    
    Gui::getMenuWidth(ui.m_menu_width, ui.m_area_map_width);
    
    if (df::global::ui_building_in_resize)
    {
        ui.m_building_in_resize = *df::global::ui_building_in_resize;
        ui.m_building_resize_radius = *df::global::ui_building_resize_radius;
    }
    
    ui.m_designationcoord_share = false;
    ui.m_squadcoord_share = false;
    ui.m_burrowcoord_share = false;
    
    // menu stabilizing
    ui.m_list_cursor.clear();
    ui.m_civ_x = -1;
    ui.m_civ_y = -1;
    
    df::viewscreen* _vs = &df::global::gview->view;
    while (_vs)
    {
        if (!Screen::isDismissed(_vs))
        {
            df::virtual_identity* _id = virtual_identity::get(_vs);
            if (_id == &df::viewscreen_announcelistst::_identity)
            {
                df::viewscreen_announcelistst* vs_a = static_cast<df::viewscreen_announcelistst*>(vs);
                if (df::report* report = vector_get(vs_a->reports, vs_a->sel_idx))
                {
                    int32_t vs_depth = get_vs_depth(_vs);
                    ui.m_list_cursor.resize(vs_depth + 1);
                    ui.m_list_cursor[vs_depth] = report->id;
                }
            }
            if (_id == &df::viewscreen_reportlistst::_identity)
            {
                df::viewscreen_reportlistst* vs_r = static_cast<df::viewscreen_reportlistst*>(vs);
                if (df::report* report = vector_get(df::global::world->status.reports, vs_r->cursor))
                {
                    int32_t vs_depth = get_vs_depth(_vs);
                    ui.m_list_cursor.resize(vs_depth + 1);
                    ui.m_list_cursor[vs_depth] = report->id;
                }
            }
            if (_id == &df::viewscreen_civlistst::_identity)
            {
                df::viewscreen_civlistst* vs_c = static_cast<df::viewscreen_civlistst*>(vs);
                ui.m_civ_x = vs_c->map_x;
                ui.m_civ_y = vs_c->map_y;
            }
        }
        
        _vs = _vs->child;
    }
    
    // squads
    if (id == &df::viewscreen_dwarfmodest::_identity
        && df::global::ui->main.mode == df::ui_sidebar_mode::Squads)
    {
        auto& squads = df::global::ui->squads;
        auto& ui_squads = ui.m_squads;
        
        // selected individuals
        ui_squads.in_select_indiv = squads.in_select_indiv;
        ui_squads.indiv_selected = squads.indiv_selected;
        
        // selected squads
        ui_squads.squad_selected.clear();
        for (size_t i = 0; i < squads.list.size() && i < squads.sel_squads.size(); ++i)
        {
            df::squad* squad = squads.list.at(i);
            if (squad && squads.sel_squads.at(i))
            {
                ui_squads.squad_selected.push_back(squad->id);
            }
        }
        
        ui.m_squadcoord_share = squads.in_kill_rect;
        
        // selected kill targets
        ui_squads.kill_selected.clear();
        if (squads.in_kill_list || squads.in_kill_rect)
        {
            const std::vector<df::unit*>& targets = (squads.in_kill_rect)
                ? squads.kill_rect_targets
                : squads.kill_targets;
            for (size_t i = 0; i < targets.size() && i < squads.sel_kill_targets.size(); ++i)
            {
                if (squads.sel_kill_targets.at(i))
                {
                    const df::unit* unit = targets.at(i);
                    if (unit)
                    {
                        ui_squads.kill_selected.push_back(unit->id);
                    }
                }
            }
        }
    }
    else
    {
        // clear some state?
    }
    
    if (id == &df::viewscreen_dwarfmodest::_identity
        &&(
            is_designation_mode(df::global::ui->main.mode)
            || is_designation_mode_sub(df::global::ui->main.mode)
        )
    )
    {
        ui.m_designationcoord_share = true;
    }
    
    if (id == &df::viewscreen_dwarfmodest::_identity
        &&(
            df::global::ui->main.mode == df::ui_sidebar_mode::Burrows
        )
    )
    {
        ui.m_burrowcoord_share = true;
    }

    // decide whether this ui position requires a pause.
    if (is_realtime_dwarf_menu())
    {
        ui.m_pause_required = false;
    }
    else
    {
        ui.m_pause_required = true;
    }
}

void deferred_state_restore(Client* client)
{
    UIState& ui = client->ui;
    if (ui.m_defer_restore_cursor)
    {
        restore_cursor(client);
        ui.m_defer_restore_cursor = false;
    }
}