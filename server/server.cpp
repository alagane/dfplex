/*
 * Dwarfplex is based on Webfort, created by Vitaly Pronkin on 14/05/14.
 * Copyright (c) 2014 mifki, ISC license.
 * Copyright (c) 2020 white-rabbit, ISC license
 */

#include "dfplex.hpp"
#include "server.hpp"
#include "config.hpp"
#include "serverlog.hpp"
#include "DFHackVersion.h"
#include "Core.h"
#include "tinythread.h"

#include <cassert>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace ws = websocketpp;
// FIXME: use unique_ptr or the boost equivalent
typedef ws::connection_hdl conn_hdl;

static std::owner_less<conn_hdl> conn_lt;
inline bool operator==(const conn_hdl& p, const conn_hdl& q)
{
    return (!conn_lt(p, q) && !conn_lt(q, p));
}
inline bool operator!=(const conn_hdl& p, const conn_hdl& q)
{
    return conn_lt(p, q) || conn_lt(q, p);
}

namespace lib = websocketpp::lib;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

typedef ws::server<ws::config::asio> server;

typedef server::message_ptr message_ptr;

static conn_hdl null_conn = std::weak_ptr<void>();

std::map<conn_hdl, Client*, std::owner_less<conn_hdl>> conn_map;
std::set<Client*> clients;

#include "config.hpp"
#include "dfplex.hpp"
#include "input.hpp"

#include "MemAccess.h"
#include "Console.h"
#include "modules/World.h"
#include "df/global_objects.h"
#include "df/graphic.h"
using df::global::gps;

static unsigned char buf[0x100000];

static std::ostream* out;

class logbuf : public std::stringbuf {
public:
    logbuf() : std::stringbuf()
    { }
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
            o.replace(i, 13, "[DFPLEX]");
        }

        // color warnings and errors
        const bool err = (o.find("ERROR") != std::string::npos);

        if (err)
        {
            DFHack::Core::printerr("%s", o.c_str());
        }
        else
        {
            DFHack::Core::print("%s", o.c_str());
        }
        str("");
        return 0;
    }
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

// TODO: migrate to dfplex.cpp?
size_t get_client_count()
{
    return clients.size();
}

void remove_client(Client* cl)
{
    ClientUpdateInfo info;
    info.is_multiplex = false;
    info.on_destroy = true;
    if (cl)
    {
        for (auto iter = clients.begin(); iter != clients.end(); ++iter)
        {
            if (*iter == cl)
            {
                if ((*iter)->update_cb)
                {
                    (*iter)->update_cb(*iter, info);
                }
                delete *iter;
                clients.erase(iter);
                break;
            }
        }
    }
}

Client* add_client()
{
    Client* cl = *clients.emplace(new Client()).first;
    
    DFPlex::log_message("A new client has joined.");
    
    // clear screen
    memset(cl->sc, 0, sizeof(cl->sc));

    return cl;
}

Client* add_client(client_update_cb&& cb)
{
    Client* cl = add_client();
    cl->update_cb = std::move(cb);
    return cl;
}

Client* get_client(int32_t n)
{
    if (n < 0) return nullptr;
    
    auto it = clients.begin();
    for (size_t i = 0; i < static_cast<size_t>(n); ++i)
    {
        if (it == clients.end()) return nullptr;
        it++;
    }
    if (it == clients.end()) return nullptr;
    assert(*it);
    return *it;
}

Client* get_client(const ClientIdentity* id)
{
    if (!id) return nullptr;
    auto it = clients.begin();
    for (size_t i = 0; true; ++i)
    {
        if (it == clients.end()) return nullptr;
        if ((*it)->id.get() == id) return *it;
        it++;
    }
    
    // paranoia
    return nullptr;
}

int get_client_index(const ClientIdentity* id)
{
    if (!id) return -1;
    auto it = clients.begin();
    for (size_t i = 0; true; ++i)
    {
        if (it == clients.end()) return -1;
        if ((*it)->id.get() == id) return i;
        it++;
    }
    
    // paranoia
    return -1;
}

Client* get_client(conn_hdl hdl)
{
    auto iter = conn_map.find(hdl);
    if (iter == conn_map.end()) return nullptr;
    return iter->second;
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
    std::string current_player = "";
    int32_t time_left = -1;
    bool is_somebody_playing = active_players > 0;

    json << std::boolalpha << "{"
        <<  " \"active_players\": " << active_players
        << ", \"current_player\": " << str(current_player)
        << ", \"time_left\": " << -1
        << ", \"is_somebody_playing\": " << is_somebody_playing
        << ", \"using_ingame_time\": " << false
        << ", \"dfhack_version\": " << str(DFHACK_VERSION)
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
    tthread::lock_guard<decltype(dfplex_mutex)> guard(dfplex_mutex);
    if (s->get_con_from_hdl(hdl)->get_subprotocol() == WF_INVALID) {
        s->close(hdl, 4000, "Invalid version, expected '" WF_VERSION "'.");
        return;
    }

    if (clients.size() >= MAX_CLIENTS && MAX_CLIENTS != 0) {
        s->close(hdl, 4001, "Server is full.");
        return;
    }

    auto raw_conn = s->get_con_from_hdl(hdl);
    std::string addr = raw_conn->get_raw_socket().remote_endpoint().address().to_string();
    
    if (std::find(g_ban_list.begin(), g_ban_list.end(), addr) != g_ban_list.end())
    {
        s->close(hdl, 4003, "Banned.");
        return;
    }
    
	auto path = split(raw_conn->get_resource().substr(1).c_str(), '/');
    std::string nick = path[0];
	std::string user_secret = (path.size() > 1) ? path[1] : "";

    if (nick == "__NOBODY") {
        s->close(hdl, 4002, "Invalid nickname.");
        return;
    }

    Client* cl = add_client();
	cl->id->is_admin = (user_secret == SECRET);
    cl->id->addr = addr;
    cl->id->nick = nick;
    
    DFPlex::log_message("  Client addr: \"" + addr + "\"");
    if (cl->id->is_admin)
    {
        DFPlex::log_message("  Client is admin.");
    }
    if (cl->id->nick.length())
    {
        DFPlex::log_message("  Client nick: " + nick);
    }
    
    clients.emplace(cl);
    conn_map[hdl] = cl;
}

void on_close(server* s, conn_hdl c)
{
    tthread::lock_guard<decltype(dfplex_mutex)> guard(dfplex_mutex);
    
    remove_client(get_client(c));
    
    conn_map.erase(c);
}

void tock(server* s, conn_hdl hdl)
{
    // not const b/c we modify the "modified" flag per-tile for delta encoding.
    Client* cl = get_client(hdl);
    if (!cl) return;
    
    unsigned char *b = buf;
    // [0] msgtype
    *(b++) = 110;

    uint8_t client_count = clients.size();
    // [1] # of connected clients.
    *(b++) = client_count;

    // [2] is active
    *(b++) = 1;

    // [3-6] load (sum of frames) -- REMOVED
    const int32_t load = 0;
    memcpy(b, &load, sizeof(load));
    b += sizeof(load);

    // [7-8] game dimensions
    *(b++) = cl->dimx;
    *(b++) = cl->dimy;

    // dfplex sent the active player's nickname here.
    // this is now used to send info_message.
    // [9] (length info_message.)
    uint8_t info_len = std::min<uint32_t>(0xff, cl->info_message.length() + 1);
    *(b++) = info_len;
    
    // [10-M] info message.
    memcpy(b, cl->info_message.c_str(), info_len);
    b += info_len;
    
    // [?] (length debug info)
    if (cl->m_debug_enabled)
    {
        uint16_t debug_info_len = std::min<uint32_t>(0xffff, cl->m_debug_info.length() + 1);
        *(b++) = debug_info_len & 0x00ff;
        *(b++) = (debug_info_len & 0xff00) >> 8;
        
        // [?] info message.
        memcpy(b, cl->m_debug_info.c_str(), debug_info_len);
        b += debug_info_len;
    }
    else
    {
        // send no debug info.
        *(b++) = 0;
        *(b++) = 0;
    }

    // [M-N] Changed tiles. 5 bytes per tile
    for (int y = 0; y < cl->dimy; y++) {
        for (int x = 0; x < cl->dimx; x++) {
            const int tile = x * cl->dimy + y;
            if (tile >= 256 * 256) break;
            if (b >= buf + sizeof(buf) - 0x400)
            {
                return;
            }
            ClientTile& ct = cl->sc[tile]; // client's tile
            if (ct.modified)
            {
                ct.modified = false;
                *(b++) = x;
                *(b++) = y;
                *(b++) = ct.pen.ch;
                *(b++) = ct.pen.bg | (ct.is_text << 6) | (ct.is_overworld << 7);

                int bold = ct.pen.bold << 3;
                int fg   = (ct.pen.fg + bold) & 0x0f;

                *(b++) = fg;
            }
        }
    }
    s->send(hdl, (const void*) buf, (size_t)(b-buf), ws::frame::opcode::binary);
}

void on_message(server* s, conn_hdl hdl, message_ptr msg)
{
    tthread::lock_guard<decltype(dfplex_mutex)> guard(dfplex_mutex);
    
    auto str = msg->get_payload();
    const unsigned char *mdata = (const unsigned char*) str.c_str();
    int msz = str.size();
    
    Client* cl = get_client(hdl);
    if (!cl) return;

    if (mdata[0] == 117 && msz == 3) { // ResizeEvent
        cl->desired_dimx = mdata[1];
        cl->desired_dimy = mdata[2];
    } else if (mdata[0] == 111 && msz == 4) { // KeyEvent

        if (mdata[1]){
            KeyEvent match;
            match.type = EventType::type_key;
            match.mod = mdata[3];
            match.unicode = mdata[2];// retain unicode information
            match.key = mapInputCodeToSDL(mdata[1]);
            // does SDL1.2 have this function?
            // match.scancode = SDL_GetScancodeFromKey(key)
            // add to queue
            cl->keyqueue.push(match);
        } else if (mdata[2])
        {
            KeyEvent match;
            match.mod = 0; // unicode must not have modifiers.
            match.type = EventType::type_unicode;
            match.unicode = mdata[2];
            cl->keyqueue.push(match);
        }
    } else if (mdata[0] == 115) { // refreshScreen
        // in particular, this sets the modified flag to 0.
        memset(cl->sc, 0, sizeof(cl->sc));
    } else {
        tock(s, hdl);
    }
}

void on_init(conn_hdl hdl, boost::asio::ip::tcp::socket & s)
{
    s.set_option(boost::asio::ip::tcp::no_delay(true));
}

void wsthreadmain(void *i_raw_out)
{
    logbuf lb;
    std::ostream logstream(&lb);

    server srv;

    appbuf abuf(&srv);
    std::ostream astream(&abuf);
    out = &astream;

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

        srv.get_alog().set_ostream(&logstream);

        srv.set_socket_init_handler(&on_init);
        srv.set_http_handler(bind(&on_http, &srv, ::_1));
        srv.set_validate_handler(bind(&validate_open, &srv, ::_1));
        srv.set_open_handler(bind(&on_open, &srv, ::_1));
        srv.set_message_handler(bind(&on_message, &srv, ::_1, ::_2));
        srv.set_close_handler(bind(&on_close, &srv, ::_1));
        // See https://stackoverflow.com/a/548912
        // Prevent segfaults when restarting dwarf fortress, if the port was
        // not released properly on exit
        srv.set_reuse_addr(true);
        lib::error_code ec;

        // FIXME: this sometimes segfaults.
        srv.listen(PORT, ec);
        if (ec) {
            *out << "ERROR: Unable to start Dwarfplex on port " << PORT
                  << ", is it being used somehere else?" << std::endl;
            return;
        }

        srv.start_accept();
        *out << "Dwarfplex websocket serving on " << PORT << std::endl;
        *out << "(Do not connect to this in your browser.) " << std::endl;
    } catch (const std::exception & e) {
        *out << "Dwarfplex failed to start: " << e.what() << std::endl;
    } catch (lib::error_code e) {
        *out << "Dwarfplex failed to start: " << e.message() << std::endl;
    } catch (...) {
        *out << "Dwarfplex failed to start: other exception" << std::endl;
    }

    try {
        srv.run();
    } catch (const std::exception & e) {
        *out << "ERROR: std::exception caught: " << e.what() << std::endl;
    } catch (lib::error_code e) {
        *out << "ERROR: ws++ exception caught: " << e.message() << std::endl;
    } catch (...) {
        *out << "ERROR: Unknown exception caught:" << std::endl;
    }
    return;
}
