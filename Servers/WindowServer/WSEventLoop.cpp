/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "WSClipboard.h"
#include <Kernel/KeyCode.h>
#include <Kernel/MousePacket.h>
#include <LibCore/CLocalSocket.h>
#include <LibCore/CObject.h>
#include <WindowServer/WSClientConnection.h>
#include <WindowServer/WSCursor.h>
#include <WindowServer/WSEvent.h>
#include <WindowServer/WSEventLoop.h>
#include <WindowServer/WSScreen.h>
#include <WindowServer/WSWindowManager.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

//#define WSMESSAGELOOP_DEBUG

WSEventLoop::WSEventLoop()
    : m_server(Core::LocalServer::construct())
{
    m_keyboard_fd = open("/dev/keyboard", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    m_mouse_fd = open("/dev/psaux", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    bool ok = m_server->take_over_from_system_server();
    ASSERT(ok);

    m_server->on_ready_to_accept = [this] {
        auto client_socket = m_server->accept();
        if (!client_socket) {
            dbg() << "WindowServer: accept failed.";
            return;
        }
        static int s_next_client_id = 0;
        int client_id = ++s_next_client_id;
        new_client_connection<WSClientConnection>(*client_socket, client_id);
    };

    ASSERT(m_keyboard_fd >= 0);
    ASSERT(m_mouse_fd >= 0);

    m_keyboard_notifier = Core::Notifier::construct(m_keyboard_fd, Core::Notifier::Read);
    m_keyboard_notifier->on_ready_to_read = [this] { drain_keyboard(); };

    m_mouse_notifier = Core::Notifier::construct(m_mouse_fd, Core::Notifier::Read);
    m_mouse_notifier->on_ready_to_read = [this] { drain_mouse(); };

    WSClipboard::the().on_content_change = [&] {
        WSClientConnection::for_each_client([&](auto& client) {
            client.notify_about_clipboard_contents_changed();
        });
    };
}

WSEventLoop::~WSEventLoop()
{
}

void WSEventLoop::drain_mouse()
{
    auto& screen = WSScreen::the();
    unsigned prev_buttons = screen.mouse_button_state();
    int dx = 0;
    int dy = 0;
    int dz = 0;
    unsigned buttons = prev_buttons;
    for (;;) {
        MousePacket packet;
        ssize_t nread = read(m_mouse_fd, &packet, sizeof(MousePacket));
        if (nread == 0)
            break;
        ASSERT(nread == sizeof(packet));
        buttons = packet.buttons;

        dx += packet.dx;
        dy += -packet.dy;
        dz += packet.dz;
        if (buttons != prev_buttons) {
            screen.on_receive_mouse_data(dx, dy, dz, buttons);
            dx = 0;
            dy = 0;
            dz = 0;
            prev_buttons = buttons;
        }
    }
    if (dx || dy || dz)
        screen.on_receive_mouse_data(dx, dy, dz, buttons);
}

void WSEventLoop::drain_keyboard()
{
    auto& screen = WSScreen::the();
    for (;;) {
        KeyEvent event;
        ssize_t nread = read(m_keyboard_fd, (u8*)&event, sizeof(KeyEvent));
        if (nread == 0)
            break;
        ASSERT(nread == sizeof(KeyEvent));
        screen.on_receive_keyboard_data(event);
    }
}
