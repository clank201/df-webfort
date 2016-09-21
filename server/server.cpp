/*
 * server.cpp
 * Part of Web Fortress
 *
 * Copyright (c) 2014 mifki, ISC license.
 */

#include "server.hpp"

#define WF_VERSION "WebFortress-v2.0"
#define WF_INVALID "WebFortress-invalid"

#include <cassert>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace lib = websocketpp::lib;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

typedef ws::server<ws::config::asio> server;

typedef server::message_ptr message_ptr;

static conn_hdl null_conn = std::weak_ptr<void>();
static Client* null_client;

static conn_hdl active_conn = null_conn;

static std::ostream* out;
static std::ostream* logstream;
static DFHack::color_ostream* raw_out;

conn_map clients;

#include "config.hpp"
#include "webfort.hpp"
#include "input.hpp"

#include "MemAccess.h"
#include "Console.h"
#include "modules/World.h"
#include "df/global_objects.h"
#include "df/graphic.h"
using df::global::gps;


class logbuf : public std::stringbuf {
public:
    logbuf(DFHack::color_ostream* i_out) : std::stringbuf()
    {
        dfout = i_out;
    }
    int sync()
    {
        std::string o = this->str();
        size_t i = -1;
        // remove empty lines.
        while ((i = o.find("\n\n")) != std::string::npos) {
            o.replace(i, 2, "\n");
        }
        // Remove uninformative [application]
        while ((i = o.find("[application]")) != std::string::npos) {
            o.replace(i, 13, "[WEBFORT]");
        }

        std::cout << o;
        *dfout << o;

        dfout->flush();
        dfout->color(DFHack::COLOR_RESET);
        std::cout.flush();
        str("");
        return 0;
    }
    DFHack::color_ostream* dfout;
};

class appbuf : public std::stringbuf {
public:
    appbuf(server* i_srv) : std::stringbuf()
    {
        srv = i_srv;
    }
    int sync()
    {
        srv->get_alog().write(ws::log::alevel::app, this->str());
        str("");
        return 0;
    }
private:
    server* srv;
};

class logstream_t : public std::ostream {
public:
    logstream_t(DFHack::color_ostream* i_out)
        : std::ostream(&m_lb), m_lb(i_out)
    {}
private:
    logbuf m_lb;
};

class appstream : public std::ostream {
public:
    appstream(server* i_srv)
        : std::ostream(&m_ab),  m_ab(i_srv)
    {}
private:
    appbuf m_ab;
};


Client* get_client(conn_hdl hdl)
{
    auto it = clients.find(hdl);
    if (it == clients.end()) {
        return null_client;
    }
    return it->second;
}

int32_t round_timer()
{
    if (INGAME_TIME) {
        // FIXME: check if we are actually in-game
        return DFHack::World::ReadCurrentTick(); // uint32_t
    } else {
        return time(NULL); // time_t, usually int32_t
    }
}

#define IDLE_TIMEOUT 10
time_t itime = 0;
bool timed_out = true;
void reset_idle_timer()
{
    itime = time(NULL);
    timed_out = false;
}

void idle_timer()
{
    if (!timed_out && active_conn == null_conn) {
        time_t now = time(NULL);
        time_t diff = now - itime;
        if (diff > IDLE_TIMEOUT) {
            *out << "Quicksave triggered." << std::endl;
            quicksave(raw_out);
            timed_out = true;
        }
    }
}

void set_active(conn_hdl newc)
{
    if (active_conn == newc) { return; }
    Client* newcl = get_client(newc); // fail early
    active_conn = newc;

    if (active_conn != null_conn) {
        newcl->atime = round_timer();
        memset(newcl->mod, 0, sizeof(newcl->mod));

        std::stringstream ss;
        if (newcl->nick == "") {
            ss << "A wandering spirit";
        } else {
            deify(raw_out, newcl->nick);
            ss << "The spirit " << newcl->nick;
        }
        ss << " has seized control.";
        show_announcement(ss.str());
    } else if (AUTOSAVE_WHILE_IDLE) {
        reset_idle_timer();
    }

    if (!(*df::global::pause_state)) {
        simkey(1, 0, SDL::K_SPACE, ' ');
        simkey(0, 0, SDL::K_SPACE, ' ');
    }

    if (newcl->nick == "") {
        *out << newcl->addr;
    } else {
        *out << newcl->nick;
    }
    *out << " is now active." << std::endl;
}

int32_t get_time_left(bool* time_up = nullptr)
{
    int32_t time_left = -1;
    Client* active_cl = get_client(active_conn);

    if (TURNTIME != 0 && (active_conn != null_conn) && clients.size() > 1) {
        time_t now = round_timer();
        int played = now - active_cl->atime;
        if (played < TURNTIME) {
            time_left = TURNTIME - played;
        } else if (time_up != nullptr) {
            *time_up = true;
        }
    }
    return time_left;
}

std::string str(std::string s)
{
    return "\"" + s + "\"";
}

#define STATUS_ROUTE "/api/status.json"
std::string status_json()
{
    std::stringstream json;
    int active_players = clients.size();
    Client* active_cl = get_client(active_conn);
    std::string current_player = active_cl->nick;
    int32_t time_left = get_time_left();
    bool is_somebody_playing = active_conn != null_conn;

    json << std::boolalpha << "{"
        <<  " \"active_players\": " << active_players
        << ", \"current_player\": " << str(current_player)
        << ", \"time_left\": " << time_left
        << ", \"is_somebody_playing\": " << is_somebody_playing
        << ", \"using_ingame_time\": " << INGAME_TIME
        << ", \"dfhack_version\": " << str("0.43.03-r1")
        << ", \"webfort_version\": " << str(WF_VERSION)
        << " }\n";

    return json.str();
}

void on_http(server* s, conn_hdl hdl)
{
    server::connection_ptr con = s->get_con_from_hdl(hdl);
    std::stringstream output;
    std::string route = con->get_resource();
    if (route == STATUS_ROUTE) {
        con->set_status(websocketpp::http::status_code::ok);
        con->replace_header("Content-Type", "application/json");
        con->replace_header("Access-Control-Allow-Origin", "*");
        con->set_body(status_json());
    }
}

bool validate_open(server* s, conn_hdl hdl)
{
    auto raw_conn = s->get_con_from_hdl(hdl);

    std::vector<std::string> protos = raw_conn->get_requested_subprotocols();
    if (std::find(protos.begin(), protos.end(), WF_VERSION) != protos.end()) {
        raw_conn->select_subprotocol(WF_VERSION);
    } else if (std::find(protos.begin(), protos.end(), WF_INVALID) != protos.end()) {
        raw_conn->select_subprotocol(WF_INVALID);
    }

    return true;
}

void on_open(server* s, conn_hdl hdl)
{
    if (s->get_con_from_hdl(hdl)->get_subprotocol() == WF_INVALID) {
        s->close(hdl, 4000, "Invalid version, expected '" WF_VERSION "'.");
        return;
    }

    if (clients.size() >= MAX_CLIENTS) {
        s->close(hdl, 4001, "Server is full.");
        return;
    }

    auto raw_conn = s->get_con_from_hdl(hdl);
    auto path = split(raw_conn->get_resource().substr(1).c_str(), '/');
    std::string nick = path[0];
    std::string user_secret = (path.size() > 1) ? path[1] : "";

    if (nick == "__NOBODY") {
        s->close(hdl, 4002, "Invalid nickname.");
        return;
    }

    Client* cl = new Client;
    cl->is_admin = (user_secret == SECRET);

    cl->addr = raw_conn->get_remote_endpoint();
    cl->nick = nick;
    cl->atime = round_timer();
    memset(cl->mod, 0, sizeof(cl->mod));

    assert(cl->addr != "");
    assert(cl->nick != "__NOBODY");
    clients[hdl] = cl;
}

void on_close(server* s, conn_hdl c)
{
    Client* cl = get_client(c);
    if (cl != null_client) {
        if (c == active_conn) {
            set_active(null_conn);
        }
        delete cl;
    }
    clients.erase(c);
}

static unsigned char buf[64*1024];
void tock(server* s, conn_hdl hdl)
{
    Client* cl = get_client(hdl);
    Client* active_cl = get_client(active_conn);
    bool time_up = false;

    idle_timer();
    int32_t time_left = get_time_left(&time_up);

    if (time_up) {
        *out << active_cl->nick << " has run out of time." << std::endl;
        set_active(null_conn);
    }

    unsigned char *b = buf;
    // [0] msgtype
    *(b++) = 110;

    uint8_t client_count = clients.size();
    // [1] # of connected clients. 128 bit set if client is active player.
    *(b++) = client_count;

    // [2] Bitfield.
    uint8_t bits = 0;
    bits |= hdl == active_conn?       1 : 0; // are you the active player?
    bits |= null_conn == active_conn? 2 : 0; // is nobody playing?
    bits |= INGAME_TIME?              4 : 0; // are we using in-game time?

    *(b++) = bits;

    // [3-6] time left, in seconds. -1 if no timer.
    memcpy(b, &time_left, sizeof(time_left));
    b += sizeof(time_left);

    // [7-8] game dimensions
    *(b++) = gps->dimx;
    *(b++) = gps->dimy;

    // [9] Length of current active player's nick, including '\0'.
    uint8_t nick_len = active_cl->nick.length() + 1;
    *(b++) = nick_len;

    unsigned char *mod = cl->mod;

    // [10-M] null-terminated string: active player's nick
    memcpy(b, active_cl->nick.c_str(), nick_len);
    b += nick_len;

    // [M-N] Changed tiles. 5 bytes per tile
    for (int y = 0; y < gps->dimy; y++) {
        for (int x = 0; x < gps->dimx; x++) {
            const int tile = x * gps->dimy + y;
            unsigned char *s = sc + tile*4;
            if (mod[tile])
                continue;

            *(b++) = x;
            *(b++) = y;
            *(b++) = s[0];
            *(b++) = s[2];

            int bold = (s[3] != 0) * 8;
            int fg   = (s[1] + bold) % 16;

            *(b++) = fg;
            mod[tile] = 1;
        }
    }
    s->send(hdl, (const void*) buf, (size_t)(b-buf), ws::frame::opcode::binary);
}

void on_message(server* s, conn_hdl hdl, message_ptr msg)
{
    auto str = msg->get_payload();
    const unsigned char *mdata = (const unsigned char*) str.c_str();
    int msz = str.size();

    if (mdata[0] == 112 && msz == 3) { // ResizeEvent
        if (hdl == active_conn) {
            newwidth = mdata[1];
            newheight = mdata[2];
            needsresize = true;
        }
    } else if (mdata[0] == 111 && msz == 4) { // KeyEvent
        if (hdl == active_conn) {
            Client* cl = get_client(hdl);
            SDL::Key k = mdata[2] ? (SDL::Key)mdata[2] : mapInputCodeToSDL(mdata[1]);
            bool is_safe_key = cl->is_admin ||
                k != SDL::K_ESCAPE ||
                is_safe_to_escape();
            if (k != SDL::K_UNKNOWN && is_safe_key) {
                int jsmods = mdata[3];
                int sdlmods = 0;

                if (jsmods & 1) {
                    simkey(1, 0, SDL::K_LALT, 0);
                    sdlmods |= SDL::KMOD_ALT;
                }
                if (jsmods & 2) {
                    simkey(1, 0, SDL::K_LSHIFT, 0);
                    sdlmods |= SDL::KMOD_SHIFT;
                }
                if (jsmods & 4) {
                    simkey(1, 0, SDL::K_LCTRL, 0);
                    sdlmods |= SDL::KMOD_CTRL;
                }

                simkey(1, sdlmods, k, mdata[2]);
                simkey(0, sdlmods, k, mdata[2]);

                if (jsmods & 1) {
                    simkey(0, 0, SDL::K_LALT, 0);
                }
                if (jsmods & 2) {
                    simkey(0, 0, SDL::K_LSHIFT, 0);
                }
                if (jsmods & 4) {
                    simkey(0, 0, SDL::K_LCTRL, 0);
                }
            }
        }
    } else if (mdata[0] == 115) { // refreshScreen
        Client* cl = get_client(hdl);
        memset(cl->mod, 0, sizeof(cl->mod));
    } else if (mdata[0] == 116) { // requestTurn
        assert(active_conn == active_conn);
        assert(hdl != null_conn);
        if (hdl == active_conn) {
            set_active(null_conn);
        } else if (active_conn == null_conn) {
            set_active(hdl);
        }
    } else {
        tock(s, hdl);
    }

    return;
}

void on_init(conn_hdl hdl, boost::asio::ip::tcp::socket & s)
{
    s.set_option(boost::asio::ip::tcp::no_delay(true));
}

void wsloop(void *a_srv)
{
    try {
        ((server*)a_srv)->run();
    } catch (const std::exception & e) {
        *out << "ERROR: std::exception caught: " << e.what() << std::endl;
    } catch (lib::error_code e) {
        *out << "ERROR: ws++ exception caught: " << e.message() << std::endl;
    } catch (...) {
        *out << "ERROR: Unknown exception caught:" << std::endl;
    }
    return;
}

struct WFServerImpl {
    tthread::thread* loop;
    server srv;
    WFServerImpl(DFHack::color_ostream&);
    ~WFServerImpl();
    void start();
    void stop();
};

WFServerImpl::WFServerImpl(DFHack::color_ostream& i_raw_out)
{
    null_client = new Client;
    null_client->nick = "__NOBODY";

    raw_out = &i_raw_out;
    logstream = new logstream_t(raw_out);
    out = new appstream(&srv);
}

WFServerImpl::~WFServerImpl()
{
    delete null_client;
    delete logstream;
    delete out;
}

void WFServerImpl::start()
{
    load_config();
    try {
        srv.clear_access_channels(ws::log::alevel::all);
        srv.set_access_channels(
                ws::log::alevel::connect    |
                ws::log::alevel::disconnect |
                ws::log::alevel::app
        );
        srv.set_error_channels(
                ws::log::elevel::info   |
                ws::log::elevel::warn   |
                ws::log::elevel::rerror |
                ws::log::elevel::fatal
        );
        srv.init_asio();

        srv.get_alog().set_ostream(logstream);

        srv.set_socket_init_handler(&on_init);
        srv.set_http_handler(bind(&on_http, &srv, ::_1));
        srv.set_validate_handler(bind(&validate_open, &srv, ::_1));
        srv.set_open_handler(bind(&on_open, &srv, ::_1));
        srv.set_message_handler(bind(&on_message, &srv, ::_1, ::_2));
        srv.set_close_handler(bind(&on_close, &srv, ::_1));

        lib::error_code ec;
        srv.listen(PORT, ec);
        if (ec) {
            *out << "ERROR: Unable to start Webfort on port " << PORT
                  << ", is it being used somehere else?" << std::endl;
            return;
        }

        srv.start_accept();
        *out << "Web Fortress started on port " << PORT << std::endl;
    } catch (const std::exception & e) {
        *out << "Webfort failed to start: " << e.what() << std::endl;
    } catch (lib::error_code e) {
        *out << "Webfort failed to start: " << e.message() << std::endl;
    } catch (...) {
        *out << "Webfort failed to start: other exception" << std::endl;
    }
    loop = new tthread::thread(wsloop, &srv);
}

void WFServerImpl::stop()
{
    srv.stop();
}

WFServer::WFServer(DFHack::color_ostream& o) { impl = new WFServerImpl(o); }
WFServer::~WFServer()  { delete impl; }
void WFServer::start() { impl->start(); }
void WFServer::stop()  { impl->stop(); }
