/* ***** BEGIN LICENSE BLOCK *****
 *   Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 *   The contents of this file are subject to the Mozilla Public License Version
 *   1.1 (the "License"); you may not use this file except in compliance with
 *   the License. You may obtain a copy of the License at
 *   http://www.mozilla.org/MPL/
 *
 *   Software distributed under the License is distributed on an "AS IS" basis,
 *   WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 *   for the specific language governing rights and limitations under the
 *   License.
 *
 *   Copyright 2009-2011, Red Hat Inc.
 *   Copyright 2013, Red Hat Inc.
 *   Based on mozilla.org's scriptable plugin example
 *
 *   The Original Code is mozilla.org code.
 *
 *   The Initial Developer of the Original Code is
 *   Netscape Communications Corporation.
 *   Portions created by the Initial Developer are Copyright (C) 1998
 *   the Initial Developer. All Rights Reserved.
 *
 *   Contributor(s):
 *   Uri Lublin
 *   Martin Stransky
 *   Peter Hatina
 *   Christophe Fergeau
 *
 *   Alternatively, the contents of this file may be used under the terms of
 *   either the GNU General Public License Version 2 or later (the "GPL"), or
 *   the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 *   in which case the provisions of the GPL or the LGPL are applicable instead
 *   of those above. If you wish to allow use of your version of this file only
 *   under the terms of either the GPL or the LGPL, and not to allow others to
 *   use your version of this file under the terms of the MPL, indicate your
 *   decision by deleting the provisions above and replace them with the notice
 *   and other provisions required by the GPL or the LGPL. If you do not delete
 *   the provisions above, a recipient may use your version of this file under
 *   the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <glib.h>

#include "rederrorcodes.h"
#include "controller.h"
#include "plugin.h"

SpiceController::SpiceController(nsPluginInstance *aPlugin):
    m_pid_controller(0),
    m_pipe(NULL),
    m_plugin(aPlugin),
    m_child_watch_mainloop(NULL)
{
}

SpiceController::~SpiceController()
{
    g_debug("%s", G_STRFUNC);
    Disconnect();
}

void SpiceController::SetFilename(const std::string &name)
{
    m_name = name;
}

void SpiceController::SetProxy(const std::string &proxy)
{
    m_proxy = proxy;
}

#define FACILITY_SPICEX             50
#define FACILITY_CREATE_RED_PROCESS 51
#define FACILITY_STRING_OPERATION   52
#define FACILITY_CREATE_RED_EVENT   53
#define FACILITY_CREATE_RED_PIPE    54
#define FACILITY_PIPE_OPERATION     55

int SpiceController::Connect(const int nRetries)
{
    int rc = -1;
    int sleep_time = 1;

    // try to connect for specified count
    for (int i = 0; rc != 0 && i < nRetries; ++i)
    {
        rc = Connect();
        if (rc == 1)
            break;
        g_usleep(sleep_time * G_USEC_PER_SEC);
    }
    if (rc != 0) {
        g_warning("error connecting");
        g_assert(m_pipe == NULL);
    }
    if (!CheckPipe()) {
        g_warning("Pipe validation failure");
        g_warn_if_fail(m_pipe == NULL);
    }
    if (m_pipe == NULL) {
        g_warning("failed to create pipe");
#ifdef XP_WIN
        rc = MAKE_HRESULT(1, FACILITY_CREATE_RED_PIPE, GetLastError());
#endif
        this->StopClient();
    }

    return rc;
}

void SpiceController::Disconnect()
{
}

void SpiceController::ChildExited(GPid pid, gint status, gpointer user_data)
{
    SpiceController *fake_this = (SpiceController *)user_data;

    g_message("Client with pid %p exited", pid);

    g_main_loop_quit(fake_this->m_child_watch_mainloop);
    /* FIXME: we are not in the main thread!! */
    fake_this->m_plugin->OnSpiceClientExit(status);
}

void SpiceController::WaitForPid(GPid pid)
{
    GMainContext *context;
    GSource *source;

    context = g_main_context_new();

    m_child_watch_mainloop = g_main_loop_new(context, FALSE);
    source = g_child_watch_source_new(pid);
    g_source_set_callback(source, (GSourceFunc)ChildExited, this, NULL);
    g_source_attach(source, context);

    g_main_loop_run(m_child_watch_mainloop);

    g_main_loop_unref(m_child_watch_mainloop);
    g_main_context_unref(context);

    g_spawn_close_pid(pid);
    if (pid == m_pid_controller)
        m_pid_controller = 0;
}

gpointer SpiceController::ClientThread(gpointer data)
{
    SpiceController *fake_this = (SpiceController *)data;
    gchar **env = g_get_environ();
    GPid pid;
    gboolean spawned = FALSE;
    GError *error = NULL;
    GStrv client_argv;

    // Setup client environment
    fake_this->SetupControllerPipe(env);
    if (!fake_this->m_proxy.empty())
        env = g_environ_setenv(env, "SPICE_PROXY", fake_this->m_proxy.c_str(), TRUE);

    // Work around bug in firefox gtk3 builds, see
    // https://bugzilla.redhat.com/show_bug.cgi?id=1217076
    // http://emilio.pozuelo.org/posts/75
    //
    // This LD_PRELOAD'ed library causes gtk3 applications to crash
    // FIXME: remove this hack if this ever gets improved in firefox-gtk3
    env = g_environ_unsetenv(env, "LD_PRELOAD");

    // Try to spawn main client
    client_argv = fake_this->GetClientPath();
    if (client_argv != NULL) {
        char *argv_str = g_strjoinv(" ", client_argv);
        g_warning("main client cmdline: %s", argv_str);
        g_free(argv_str);

        spawned = g_spawn_async(NULL,
                                client_argv, env,
                                G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, /* child_func, child_arg */
                                &pid, &error);
        if (error != NULL) {
            g_warning("failed to start %s: %s", client_argv[0], error->message);
            g_warn_if_fail(spawned == FALSE);
            g_clear_error(&error);
        }
        g_strfreev(client_argv);
    }

    if (!spawned) {
        // Fallback client for backward compatibility
        GStrv fallback_argv;
        char *argv_str;
        fallback_argv = fake_this->GetFallbackClientPath();
        if (fallback_argv == NULL) {
            goto out;
        }

        argv_str = g_strjoinv(" ", fallback_argv);
        g_warning("fallback client cmdline: %s", argv_str);
        g_free(argv_str);

        g_message("failed to run preferred client, running fallback client instead");
        spawned = g_spawn_async(NULL, fallback_argv, env,
                                G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, /* child_func, child_arg */
                                &pid, &error);
        if (error != NULL) {
            g_warning("failed to start %s: %s", fallback_argv[0], error->message);
            g_warn_if_fail(spawned == FALSE);
            g_clear_error(&error);
        }
    }

    out:
        g_strfreev(env);

    if (!spawned) {
        g_critical("ERROR failed to run spicec fallback");
        return NULL;
    }

#ifdef XP_UNIX
    fake_this->m_pid_controller = pid;
#endif
    fake_this->WaitForPid(pid);

    return NULL;
}

bool SpiceController::StartClient()
{
    GThread *thread;

    thread = g_thread_new("spice-xpi client thread", ClientThread, this);

    return (thread != NULL);
}

int SpiceController::TranslateRC(int nRC)
{
    switch (nRC)
    {
    case SPICEC_ERROR_CODE_SUCCESS:
        return 0;

    case SPICEC_ERROR_CODE_GETHOSTBYNAME_FAILED:
        return RDP_ERROR_CODE_HOST_NOT_FOUND;

    case SPICEC_ERROR_CODE_CONNECT_FAILED:
        return RDP_ERROR_CODE_WINSOCK_CONNECT_FAILED;

    case SPICEC_ERROR_CODE_ERROR:
    case SPICEC_ERROR_CODE_SOCKET_FAILED:
        return RDP_ERROR_CODE_INTERNAL_ERROR;

    case SPICEC_ERROR_CODE_RECV_FAILED:
        return RDP_ERROR_RECV_WINSOCK_FAILED;

    case SPICEC_ERROR_CODE_SEND_FAILED:
        return RDP_ERROR_SEND_WINSOCK_FAILED;

    case SPICEC_ERROR_CODE_NOT_ENOUGH_MEMORY:
        return RDP_ERROR_CODE_OUT_OF_MEMORY;

    case SPICEC_ERROR_CODE_AGENT_TIMEOUT:
        return RDP_ERROR_CODE_TIMEOUT;

    case SPICEC_ERROR_CODE_AGENT_ERROR:
        return RDP_ERROR_CODE_INTERNAL_ERROR;

    default:
        return RDP_ERROR_CODE_INTERNAL_ERROR;
    }
}
