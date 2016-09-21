/*
 * webfort.cpp
 * Web Fortress
 *
 * Created by Vitaly Pronkin on 14/05/14.
 * Copyright (c) 2014 mifki, ISC license.
 */
#include "webfort.hpp"

#include <stdint.h>
#include <iostream>
#include <map>
#include <vector>
#include <cassert>

#include "MemAccess.h"
#include "PluginManager.h"
#include "modules/MapCache.h"
#include "modules/Gui.h"
#include "modules/World.h"
#include "df/graphic.h"
#include "df/enabler.h"
#include "df/renderer.h"
#include "df/building.h"
#include "df/buildings_other_id.h"
#include "df/unit.h"
#include "df/items_other_id.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/viewscreen_setupadventurest.h"
#include "df/viewscreen_dungeonmodest.h"
#include "df/viewscreen_choose_start_sitest.h"
#include "df/viewscreen_new_regionst.h"
#include "df/viewscreen_layer_export_play_mapst.h"
#include "df/viewscreen_overallstatusst.h"
#include "df/viewscreen_movieplayerst.h"

#include "server.hpp"

using namespace df::enums;
using df::global::world;
using std::string;
using std::vector;
using df::global::gps;
using df::global::ui;
using df::global::init;

using df::global::enabler;
using df::renderer;

DFHACK_PLUGIN("webfort");
DFHACK_PLUGIN_IS_ENABLED(enabled);

struct tileref {
    int tilesetidx;
    int tile;
};

// Shared in server.h
unsigned char sc[256*256*5];
int newwidth, newheight;
volatile bool needsresize;

// #define IS_SCREEN(_sc) strict_virtual_cast<df::_sc>(ws)
#define IS_SCREEN(_sc) (id == &df::_sc::_identity)

/*
 * Detects if it is safe for a non-privileged user to trigger an ESC keybind.
 * It should not be safe if it would lead to the menu normally accessible by
 * hitting ESC in dwarf mode, as this would give access to keybind changes,
 * fort abandonment etc.
 */
bool is_safe_to_escape()
{
    df::viewscreen * ws = Gui::getCurViewscreen();
    virtual_identity* id = virtual_identity::get(ws);
    if (IS_SCREEN(viewscreen_dwarfmodest) &&
            ui->main.mode == df::ui_sidebar_mode::Default) {
        return false;
    }
    // TODO: adventurer mode
    if (IS_SCREEN(viewscreen_dungeonmodest)) {
    }
    return true;
}

void show_announcement(std::string announcement)
{
    DFHack::Gui::showAnnouncement(announcement);
}

bool is_dwarf_mode()
{
    t_gamemodes gm;
    World::ReadGameMode(gm);
    return gm.g_mode == game_mode::DWARF;
}

void deify(DFHack::color_ostream* raw_out, std::string nick)
{
    if (is_dwarf_mode()) {
        Core::getInstance().runCommand(*raw_out, "webfort/deify " + nick);
    }
}

void quicksave(DFHack::color_ostream* out)
{
    Core::getInstance().runCommand(*out, "quicksave");
}

/*
 * The source of this function is taken from mifki's TWBT plugin.
 * If you want to edit it, edit it upstream and then diff it here.
 */
static bool is_text_tile(int x, int y, bool &is_map)
{
    df::viewscreen* ws = Gui::getCurViewscreen();
    virtual_identity* id = virtual_identity::get(ws);
    assert(ws != NULL);

    int32_t w = gps->dimx, h = gps->dimy;

    is_map = false;

    if (!x || !y || x == w - 1 || y == h - 1)
       return true;

    if (IS_SCREEN(viewscreen_dwarfmodest))
    {
        uint8_t menu_width, area_map_width;
        Gui::getMenuWidth(menu_width, area_map_width);
        int32_t menu_left = w - 1, menu_right = w - 1;

        bool menuforced = (ui->main.mode != df::ui_sidebar_mode::Default || df::global::cursor->x != -30000);

        if ((menuforced || menu_width == 1) && area_map_width == 2) // Menu + area map
        {
            menu_left = w - 56;
            menu_right = w - 25;
        }
        else if (menu_width == 2 && area_map_width == 2) // Area map only
        {
            menu_left = menu_right = w - 25;
        }
        else if (menu_width == 1) // Wide menu
            menu_left = w - 56;
        else if (menuforced || (menu_width == 2 && area_map_width == 3)) // Menu only
            menu_left = w - 32;

        if (x >= menu_left && x <= menu_right)
        {
            if (menuforced && ui->main.mode == df::ui_sidebar_mode::Burrows && ui->burrows.in_define_mode)
            {
                // Make burrow symbols use graphics font
                if ((y != 12 && y != 13 && !(x == menu_left + 2 && y == 2)) || x == menu_left || x == menu_right)
                    return true;
            }
            else
                return true;
        }

        is_map = (x > 0 && x < menu_left);

        return false;
    }

    if (IS_SCREEN(viewscreen_dungeonmodest))
    {
        // TODO: Adventure mode

        if (y >= h-2)
            return true;

        return false;
    }

    //no idea what it is supposed to do, but it doesn't compile
    /*
    if (IS_SCREEN(viewscreen_setupadventurest)) 
    {
        df::viewscreen_setupadventurest *s = static_cast<df::viewscreen_setupadventurest*>(ws);
        if (s->subscreen != df::viewscreen_setupadventurest::Nemesis)
            return true;
        else if (x < 58 || x >= 78 || y == 0 || y >= 21)
            return true;

        return false;
    }
    */

    if (IS_SCREEN(viewscreen_choose_start_sitest))
    {
        if (y <= 1 || y >= h - 6 || x == 0 || x >= 57)
            return true;

        return false;
    }

    if (IS_SCREEN(viewscreen_new_regionst))
    {
        if (y <= 1 || y >= h - 2 || x <= 37 || x == w - 1)
            return true;

        return false;
    }

    if (IS_SCREEN(viewscreen_layer_export_play_mapst))
    {
        if (x == w - 1 || x < w - 23)
            return true;

        return false;
    }

    if (IS_SCREEN(viewscreen_overallstatusst))
    {
        if ((x == 46 || x == 71) && y >= 8)
            return false;

        return true;
    }

    if (IS_SCREEN(viewscreen_movieplayerst))
    {
        df::viewscreen_movieplayerst *s = static_cast<df::viewscreen_movieplayerst*>(ws);
        return !s->is_playing;
    }

    /*if (IS_SCREEN(viewscreen_petst))
    {
        if (x == 41 && y >= 7)
            return false;

        return true;
    }*/

    return true;
}

void update_tilebuf(df::renderer *r, int x, int y)
{
    assert(0 <= x && x < gps->dimx);
    assert(0 <= y && y < gps->dimy);
    const int tile = x * gps->dimy + y;
    const unsigned char *s = r->screen + tile*4;
    unsigned char *ss = sc + tile*4;
    *(unsigned int*)ss = *(unsigned int*)s;

    bool is_map;
    if (is_text_tile(x, y, is_map)) {
        ss[2] |= 64;
    }

    for (auto i = clients.begin(); i != clients.end(); i++) {
       i->second->mod[tile] = 0;
    }
}

void update_all_tiles(df::renderer *r)
{
    assert(!!gps);
    assert(!!df::global::enabler);

    for (int32_t x = 0; x < gps->dimx; x++) {
        for (int32_t y = 0; y < gps->dimy; y++) {
            update_tilebuf(r, x, y);
        }
    }
}

#include "renderer_wrap.hpp"
struct renderhook : public renderer_wrap {
public:
    renderhook(renderer* r):renderer_wrap(r) {}
    void update_tile(int32_t x, int32_t y)
    {
        renderer_wrap::update_tile(x, y);
        update_tilebuf(this, x, y);
    }
    void update_all()
    {
        renderer_wrap::update_all();
        update_all_tiles(this);
    }
};

WFServer* s_hdl;

void hook()
{
    if (enabled)
        return;

    enabled = true;

    s_hdl->start();
    enabler->renderer = new renderhook(enabler->renderer);
    enabler->renderer->update_all();
}

void unhook()
{
    if (!enabled)
        return;

    enabled = false;

    delete enabler->renderer;
    s_hdl->stop();
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool to_enable)
{
    // FIXME: add hooks
    return CR_OK;
}

DFhackCExport command_result plugin_init(color_ostream &out, vector <PluginCommand> &commands)
{
    auto dflags = init->display.flag;
    if (dflags.is_set(init_display_flags::TEXT)) {
        out.color(COLOR_RED);
        out << "Error: For Webfort, PRINT_MODE needs to not be TEXT" << std::endl;
        out.color(COLOR_RESET);
        return CR_OK;
    }
    s_hdl = new WFServer(out);

    hook();

    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
    if (enabled)
        unhook();

    delete s_hdl;
    return CR_OK;
}

/* vim: set et sw=4 : */
