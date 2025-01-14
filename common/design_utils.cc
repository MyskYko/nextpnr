/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
 *  Copyright (C) 2018  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "design_utils.h"
#include <algorithm>
#include <map>
#include "log.h"
#include "util.h"
NEXTPNR_NAMESPACE_BEGIN

void replace_port(CellInfo *old_cell, IdString old_name, CellInfo *rep_cell, IdString rep_name)
{
    if (!old_cell->ports.count(old_name))
        return;
    PortInfo &old = old_cell->ports.at(old_name);

    // Create port on the replacement cell if it doesn't already exist
    if (!rep_cell->ports.count(rep_name)) {
        rep_cell->ports[rep_name].name = rep_name;
        rep_cell->ports[rep_name].type = old.type;
    }

    PortInfo &rep = rep_cell->ports.at(rep_name);
    NPNR_ASSERT(old.type == rep.type);

    rep.net = old.net;
    old.net = nullptr;
    if (rep.type == PORT_OUT) {
        if (rep.net != nullptr) {
            rep.net->driver.cell = rep_cell;
            rep.net->driver.port = rep_name;
        }
    } else if (rep.type == PORT_IN) {
        if (rep.net != nullptr) {
            for (PortRef &load : rep.net->users) {
                if (load.cell == old_cell && load.port == old_name) {
                    load.cell = rep_cell;
                    load.port = rep_name;
                }
            }
        }
    } else {
        NPNR_ASSERT(false);
    }
}

// Print utilisation of a design
void print_utilisation(const Context *ctx)
{
    // Sort by Bel type
    std::map<IdString, int> used_types;
    for (auto &cell : ctx->cells) {
        used_types[ctx->getBelBucketName(ctx->getBelBucketForCellType(cell.second.get()->type))]++;
    }
    std::map<IdString, int> available_types;
    for (auto bel : ctx->getBels()) {
        if (!ctx->getBelHidden(bel)) {
            available_types[ctx->getBelBucketName(ctx->getBelBucketForBel(bel))]++;
        }
    }
    log_break();
    log_info("Device utilisation:\n");
    for (auto type : available_types) {
        IdString type_id = type.first;
        int used_bels = get_or_default(used_types, type.first, 0);
        log_info("\t%20s: %5d/%5d %5d%%\n", type_id.c_str(ctx), used_bels, type.second, 100 * used_bels / type.second);
    }
    log_break();
}

// Connect a net to a port
void connect_port(const Context *ctx, NetInfo *net, CellInfo *cell, IdString port_name)
{
    if (net == nullptr)
        return;
    PortInfo &port = cell->ports.at(port_name);
    NPNR_ASSERT(port.net == nullptr);
    port.net = net;
    if (port.type == PORT_OUT) {
        NPNR_ASSERT(net->driver.cell == nullptr);
        net->driver.cell = cell;
        net->driver.port = port_name;
    } else if (port.type == PORT_IN || port.type == PORT_INOUT) {
        PortRef user;
        user.cell = cell;
        user.port = port_name;
        net->users.push_back(user);
    } else {
        NPNR_ASSERT_FALSE("invalid port type for connect_port");
    }
}

void disconnect_port(const Context *ctx, CellInfo *cell, IdString port_name)
{
    if (!cell->ports.count(port_name))
        return;
    PortInfo &port = cell->ports.at(port_name);
    if (port.net != nullptr) {
        port.net->users.erase(std::remove_if(port.net->users.begin(), port.net->users.end(),
                                             [cell, port_name](const PortRef &user) {
                                                 return user.cell == cell && user.port == port_name;
                                             }),
                              port.net->users.end());
        if (port.net->driver.cell == cell && port.net->driver.port == port_name)
            port.net->driver.cell = nullptr;
        port.net = nullptr;
    }
}

void connect_ports(Context *ctx, CellInfo *cell1, IdString port1_name, CellInfo *cell2, IdString port2_name)
{
    PortInfo &port1 = cell1->ports.at(port1_name);
    if (port1.net == nullptr) {
        // No net on port1; need to create one
        std::unique_ptr<NetInfo> p1net(new NetInfo());
        p1net->name = ctx->id(cell1->name.str(ctx) + "$conn$" + port1_name.str(ctx));
        connect_port(ctx, p1net.get(), cell1, port1_name);
        IdString p1name = p1net->name;
        NPNR_ASSERT(!ctx->cells.count(p1name));
        ctx->nets[p1name] = std::move(p1net);
    }
    connect_port(ctx, port1.net, cell2, port2_name);
}

void rename_port(Context *ctx, CellInfo *cell, IdString old_name, IdString new_name)
{
    if (!cell->ports.count(old_name))
        return;
    PortInfo pi = cell->ports.at(old_name);
    if (pi.net != nullptr) {
        if (pi.net->driver.cell == cell && pi.net->driver.port == old_name)
            pi.net->driver.port = new_name;
        for (auto &usr : pi.net->users)
            if (usr.cell == cell && usr.port == old_name)
                usr.port = new_name;
    }
    cell->ports.erase(old_name);
    pi.name = new_name;
    cell->ports[new_name] = pi;
}

void rename_net(Context *ctx, NetInfo *net, IdString new_name)
{
    if (net == nullptr)
        return;
    NPNR_ASSERT(!ctx->nets.count(new_name));
    std::swap(ctx->nets[net->name], ctx->nets[new_name]);
    ctx->nets.erase(net->name);
    net->name = new_name;
}

void replace_bus(Context *ctx, CellInfo *old_cell, IdString old_name, int old_offset, bool old_brackets,
                 CellInfo *new_cell, IdString new_name, int new_offset, bool new_brackets, int width)
{
    for (int i = 0; i < width; i++) {
        IdString old_port = ctx->id(stringf(old_brackets ? "%s[%d]" : "%s%d", old_name.c_str(ctx), i + old_offset));
        IdString new_port = ctx->id(stringf(new_brackets ? "%s[%d]" : "%s%d", new_name.c_str(ctx), i + new_offset));
        replace_port(old_cell, old_port, new_cell, new_port);
    }
}

void copy_port(Context *ctx, CellInfo *old_cell, IdString old_name, CellInfo *new_cell, IdString new_name)
{
    if (!old_cell->ports.count(old_name))
        return;
    new_cell->ports[new_name].name = new_name;
    new_cell->ports[new_name].type = old_cell->ports.at(old_name).type;
    connect_port(ctx, old_cell->ports.at(old_name).net, new_cell, new_name);
}

void copy_bus(Context *ctx, CellInfo *old_cell, IdString old_name, int old_offset, bool old_brackets,
              CellInfo *new_cell, IdString new_name, int new_offset, bool new_brackets, int width)
{
    for (int i = 0; i < width; i++) {
        IdString old_port = ctx->id(stringf(old_brackets ? "%s[%d]" : "%s%d", old_name.c_str(ctx), i + old_offset));
        IdString new_port = ctx->id(stringf(new_brackets ? "%s[%d]" : "%s%d", new_name.c_str(ctx), i + new_offset));
        copy_port(ctx, old_cell, old_port, new_cell, new_port);
    }
}

NEXTPNR_NAMESPACE_END
