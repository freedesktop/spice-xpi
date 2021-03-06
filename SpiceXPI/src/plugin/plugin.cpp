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
 *   Copyright 2009-2013, Red Hat Inc.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <signal.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

extern "C" {
#include <pthread.h>
#include <signal.h>
}

#include <cstring>
#include <fstream>
#include <set>

#if defined(XP_UNIX)
#include "controller-unix.h"
#endif
#if defined(XP_WIN)
#include "controller-win.h"
#endif
#include "plugin.h"
#include "nsScriptablePeer.h"

DECLARE_NPOBJECT_CLASS_WITH_BASE(ScriptablePluginObject,
                                 AllocateScriptablePluginObject);

namespace {
    const std::string ver(PACKAGE_VERSION);
    const std::string PLUGIN_NAME = "Spice Firefox Plugin " + ver;
    const std::string MIME_TYPES_DESCRIPTION = "application/x-spice:qsc:" + PLUGIN_NAME;
    const std::string PLUGIN_DESCRIPTION = PLUGIN_NAME + " Spice Client wrapper for firefox";

    // helper function for string copy
    char *stringCopy(const std::string &src)
    {
        char *dest = static_cast<char *>(NPN_MemAlloc(src.length() + 1));
        if (dest)
            strcpy(dest, src.c_str());

        return dest;
    }
    
    // helper function for tcp/udp range conversion and validation
    static int portToInt(const std::string &port)
    {
        errno = 0;
        char *end;
        const long int min = 0;
        const long int max = 65535;
        long int conv = strtol(port.c_str(), &end, 10);
        return (errno || *end != '\0' || end == port.c_str() || conv < min || conv > max)
            ? -1 : static_cast<int>(conv);
    }
}

const char *NPP_GetMIMEDescription(void)
{
    return const_cast<char *>(MIME_TYPES_DESCRIPTION.c_str());
}

//////////////////////////////////////
//
// general initialization and shutdown
//
NPError NS_PluginInitialize()
{
    return NPERR_NO_ERROR;
}

void NS_PluginShutdown()
{
}

// get values per plugin
NPError NS_PluginGetValue(NPPVariable aVariable, void *aValue)
{
    switch (aVariable)
    {
    case NPPVpluginNameString:
        *(static_cast<char **>(aValue)) = const_cast<char *>(PLUGIN_NAME.c_str());
        break;

    case NPPVpluginDescriptionString:
        *(static_cast<char **>(aValue)) = const_cast<char *>(PLUGIN_DESCRIPTION.c_str());
        break;

    default:
        return NPERR_INVALID_PARAM;
    }

    return NPERR_NO_ERROR;
}


/////////////////////////////////////////////////////////////
//
// construction and destruction of our plugin instance object
//
nsPluginInstanceBase *NS_NewPluginInstance(nsPluginCreateData *aCreateDataStruct)
{
    if(!aCreateDataStruct)
        return NULL;

    nsPluginInstance *plugin = new nsPluginInstance(aCreateDataStruct->instance);

    // now is the time to tell Mozilla that we are windowless
    NPN_SetValue(aCreateDataStruct->instance, NPPVpluginWindowBool, NULL);

    return plugin;
}

void NS_DestroyPluginInstance(nsPluginInstanceBase *aPlugin)
{
    if (aPlugin)
        delete static_cast<nsPluginInstance *>(aPlugin);
}

////////////////////////////////////////
//
// nsPluginInstance class implementation
//
static void glib_log_to_file(const gchar *log_domain,
                             GLogLevelFlags log_level,
                             const gchar *message,
                             gpointer user_data)
{
    FILE *log_file;

    if ((log_level & G_LOG_LEVEL_MASK) > G_LOG_LEVEL_MESSAGE) {
        return;
    }

    log_file = (FILE *)user_data;

    if (log_domain != NULL) {
        fwrite(log_domain, strlen(log_domain), 1, log_file);
        fwrite(": ", 2, 1, log_file);
    }
    if (message != NULL) {
        fwrite(message, strlen(message), 1, log_file);
    }
    fwrite("\r\n", 2, 1, log_file);
    fflush(log_file);
}

static void glib_setup_logging(void)
{
#if defined(XP_WIN)
    FILE *log_file;
    gchar *log_filename;

    if (!g_getenv("SPICE_XPI_LOG_TO_FILE"))
        return;

    log_filename = g_build_filename(g_get_tmp_dir(), "SPICEXPI.LOG", NULL);
    log_file = fopen(log_filename, "w+");
    if (log_file != NULL) {
        g_log_set_default_handler(glib_log_to_file, log_file);
    } else {
        gchar *log_msg;
        log_msg = g_strdup_printf("failed to open %s", log_filename);
        g_free(log_msg);
    }
    g_free(log_filename);
#endif
}

nsPluginInstance::nsPluginInstance(NPP aInstance):
    nsPluginInstanceBase(),
    m_connected_status(-2),
    m_instance(aInstance),
    m_initialized(true),
    m_window(NULL),
    m_fullscreen(false),
    m_smartcard(false),
    m_admin_console(false),
    m_no_taskmgr_execution(false),
    m_send_ctrlaltdel(true),
    m_usb_auto_share(true),
    m_scriptable_peer(NULL)
{
#if !GLIB_CHECK_VERSION(2, 35, 0)
    g_type_init();
#endif
    glib_setup_logging();

#if defined(XP_WIN)
    m_external_controller = new SpiceControllerWin(this);
#elif defined(XP_UNIX)
    m_external_controller = new SpiceControllerUnix(this);
#else
#error "Unknown OS, no controller implementation"
#endif
}

nsPluginInstance::~nsPluginInstance()
{
    // m_scriptable_peer may be also held by the browser
    // so releasing it here does not guarantee that it is over
    // we should take precaution in case it will be called later
    // and zero its m_plugin member
    if (m_scriptable_peer)
        NPN_ReleaseObject(m_scriptable_peer);
    delete(m_external_controller);
}

NPBool nsPluginInstance::init(NPWindow *aWindow)
{
    m_initialized = true;

    m_host_ip.clear();
    m_port.clear();
    m_password.clear();
    m_secure_port.clear();
    m_cipher_suite.clear();
    m_ssl_channels.clear();
    m_trust_store.clear();
    m_host_subject.clear();
    m_title.clear();
    m_dynamic_menu.clear();
    m_number_of_monitors.clear();
    m_guest_host_name.clear();
    m_hot_keys.clear();
    m_usb_filter.clear();
    m_language.clear();
    m_trust_store_file.clear();
    m_color_depth.clear();
    m_disable_effects.clear();
    m_proxy.clear();
    m_external_controller->SetProxy(std::string());

    m_fullscreen = false;
    m_smartcard = false;
    m_admin_console = false;
    m_no_taskmgr_execution = false;
    m_send_ctrlaltdel = true;

    return m_initialized;
}

NPError nsPluginInstance::SetWindow(NPWindow *aWindow)
{
    // keep window parameters
    m_window = aWindow;
    return NPERR_NO_ERROR;
}

void nsPluginInstance::shut()
{
    m_initialized = false;
}

NPBool nsPluginInstance::isInitialized()
{
    return m_initialized;
}

/* attribute string hostIP; */
char *nsPluginInstance::GetHostIP() const
{
    return stringCopy(m_host_ip);
}

void nsPluginInstance::SetHostIP(const char *aHostIP)
{
    m_host_ip = aHostIP;
}

/* attribute string port; */
char *nsPluginInstance::GetPort() const
{
    return stringCopy(m_port);
}

void nsPluginInstance::SetPort(const char *aPort)
{
    m_port = aPort;
}

/* attribute string SecurePort; */
char *nsPluginInstance::GetSecurePort() const
{
    return stringCopy(m_secure_port);
}

void nsPluginInstance::SetSecurePort(const char *aSecurePort)
{
    m_secure_port = aSecurePort;
}

/* attribute string Password; */
char *nsPluginInstance::GetPassword() const
{
    return stringCopy(m_password);
}

void nsPluginInstance::SetPassword(const char *aPassword)
{
    m_password = aPassword;
}

/* attribute string CipherSuite; */
char *nsPluginInstance::GetCipherSuite() const
{
    return stringCopy(m_cipher_suite);
}

void nsPluginInstance::SetCipherSuite(const char *aCipherSuite)
{
    m_cipher_suite = aCipherSuite;
}

/* attribute string SSLChannels; */
char *nsPluginInstance::GetSSLChannels() const
{
    return stringCopy(m_ssl_channels);
}

void nsPluginInstance::SetSSLChannels(const char *aSSLChannels)
{
    m_ssl_channels = aSSLChannels;

    /*
     * Backward Compatibility: Begin
     * Remove leading 's' from m_ssl_channels, e.g. "main" not "smain"
     * RHEL5 uses 'smain' and 'sinpusts
     * RHEL6 uses 'main'  and 'inputs'
     */
    const char* chan_names[] = {
        "smain", "sdisplay", "sinputs",
        "scursor", "splayback", "srecord",
        "susbredir", "ssmartcard", "stunnel"
    };
    const int nnames = sizeof(chan_names) / sizeof(chan_names[0]);

    for (int i = 0; i < nnames; i++) {
        const char *name = chan_names[i];
        size_t found = 0;
        while ((found = m_ssl_channels.find(name, found)) != std::string::npos)
            m_ssl_channels.replace(found, strlen(name), name + 1);
    }
    /* Backward Compatibility: End */
}

//* attribute string TrustStore; */
char *nsPluginInstance::GetTrustStore() const
{
    return stringCopy(m_trust_store);
}

void nsPluginInstance::SetTrustStore(const char *aTrustStore)
{
    m_trust_store = aTrustStore;
}

/* attribute string HostSubject; */
char *nsPluginInstance::GetHostSubject() const
{
    return stringCopy(m_host_subject);
}

void nsPluginInstance::SetHostSubject(const char *aHostSubject)
{
    m_host_subject = aHostSubject;
}

/* attribute boolean fullScreen; */
bool nsPluginInstance::GetFullScreen() const
{
    return m_fullscreen;
}

void nsPluginInstance::SetFullScreen(bool aFullScreen)
{
    m_fullscreen = aFullScreen;
}

/* attribute boolean Smartcard; */
bool nsPluginInstance::GetSmartcard() const
{
    return m_smartcard;
}

void nsPluginInstance::SetSmartcard(bool aSmartcard)
{
    m_smartcard = aSmartcard;
}

/* attribute string Title; */
char *nsPluginInstance::GetTitle() const
{
    return stringCopy(m_title);
}

void nsPluginInstance::SetTitle(const char *aTitle)
{
    m_title = aTitle;
}

/* attribute string dynamicMenu; */
char *nsPluginInstance::GetDynamicMenu() const
{
    return stringCopy(m_dynamic_menu);
}

void nsPluginInstance::SetDynamicMenu(const char *aDynamicMenu)
{
    m_dynamic_menu = aDynamicMenu;
}

/* attribute string NumberOfMonitors; */
char *nsPluginInstance::GetNumberOfMonitors() const
{
    return stringCopy(m_number_of_monitors);
}

void nsPluginInstance::SetNumberOfMonitors(const char *aNumberOfMonitors)
{
    m_number_of_monitors = aNumberOfMonitors;
}

/* attribute boolean AdminConsole; */
bool nsPluginInstance::GetAdminConsole() const
{
    return m_admin_console;
}

void nsPluginInstance::SetAdminConsole(bool aAdminConsole)
{
    m_admin_console = aAdminConsole;
}

/* attribute string GuestHostName; */
char *nsPluginInstance::GetGuestHostName() const
{
    return stringCopy(m_guest_host_name);
}

void nsPluginInstance::SetGuestHostName(const char *aGuestHostName)
{
    m_guest_host_name = aGuestHostName;
}

/* attribute string HotKey; */
char *nsPluginInstance::GetHotKeys() const
{
    return stringCopy(m_hot_keys);
}

void nsPluginInstance::SetHotKeys(const char *aHotKeys)
{
    m_hot_keys = aHotKeys;
}

/* attribute boolean NoTaskMgrExecution; */
bool nsPluginInstance::GetNoTaskMgrExecution() const
{
    return m_no_taskmgr_execution;
}

void nsPluginInstance::SetNoTaskMgrExecution(bool aNoTaskMgrExecution)
{
    m_no_taskmgr_execution = aNoTaskMgrExecution;
}

/* attribute boolean SendCtrlAltDelete; */
bool nsPluginInstance::GetSendCtrlAltDelete() const
{
    return m_send_ctrlaltdel;
}

void nsPluginInstance::SetSendCtrlAltDelete(bool aSendCtrlAltDelete)
{
    m_send_ctrlaltdel = aSendCtrlAltDelete;
}

/* attribute unsigned short UsbListenPort; */
unsigned short nsPluginInstance::GetUsbListenPort() const
{
    // this method exists due to RHEVM 2.2
    // and should be removed some time in future,
    // when fixed in RHEVM
    return 0;
}

void nsPluginInstance::SetUsbListenPort(unsigned short aUsbPort)
{
    // this method exists due to RHEVM 2.2
    // and should be removed some time in future,
    // when fixed in RHEVM
}

/* attribute boolean UsbAutoShare; */
bool nsPluginInstance::GetUsbAutoShare() const
{
    return m_usb_auto_share;
}

void nsPluginInstance::SetUsbAutoShare(bool aUsbAutoShare)
{
    m_usb_auto_share = aUsbAutoShare;
}

/* attribute string ColorDepth; */
char *nsPluginInstance::GetColorDepth() const
{
    return stringCopy(m_color_depth);
}

void nsPluginInstance::SetColorDepth(const char *aColorDepth)
{
    m_color_depth = aColorDepth;
}

/* attribute string DisableEffects; */
char *nsPluginInstance::GetDisableEffects() const
{
    return stringCopy(m_disable_effects);
}

void nsPluginInstance::SetDisableEffects(const char *aDisableEffects)
{
    m_disable_effects = aDisableEffects;
}

/* attribute string Proxy; */
char *nsPluginInstance::GetProxy() const
{
    return stringCopy(m_proxy);
}

void nsPluginInstance::SetProxy(const char *aProxy)
{
    m_proxy = aProxy;
    m_external_controller->SetProxy(m_proxy);
}

void nsPluginInstance::WriteToPipe(const void *data, uint32_t size)
{
    m_external_controller->Write(data, size);
}

void nsPluginInstance::SendInit()
{
    ControllerInit msg = { {CONTROLLER_MAGIC, CONTROLLER_VERSION, sizeof(msg)},
                           0, CONTROLLER_FLAG_EXCLUSIVE };
    WriteToPipe(&msg, sizeof(msg));
}

void nsPluginInstance::SendMsg(uint32_t id)
{
    ControllerMsg msg = {id, sizeof(msg)};
    WriteToPipe(&msg, sizeof(msg));
}

void nsPluginInstance::SendValue(uint32_t id, uint32_t value)
{
    if (!value)
        return;

    ControllerValue msg = { {id, sizeof(msg)}, value };
    WriteToPipe(&msg, sizeof(msg));
}

void nsPluginInstance::SendBool(uint32_t id, bool value)
{
    ControllerValue msg = { {id, sizeof(msg)}, value };
    WriteToPipe(&msg, sizeof(msg));
}

void nsPluginInstance::SendStr(uint32_t id, std::string str)
{
    if (str.empty())
        return;

    size_t size = sizeof(ControllerData) + str.size() + 1;
    ControllerData *msg = static_cast<ControllerData *>(malloc(size));
    msg->base.id = id;
    msg->base.size = size;
    strcpy(reinterpret_cast<char *>(msg->data), str.c_str());
    WriteToPipe(msg, size);
    free(msg);
}

bool nsPluginInstance::CreateTrustStoreFile(const std::string &trust_store)
{
    GFile *tmp_file;
    GFileIOStream *iostream;
    GOutputStream *stream;

    tmp_file = g_file_new_tmp("trustore.pem-XXXXXX", &iostream, NULL);
    if (tmp_file == NULL) {
        g_message("Couldn't create truststore");
        return false;
    }

    stream = g_io_stream_get_output_stream(G_IO_STREAM(iostream));
    if (!g_output_stream_write_all(stream,
                                   trust_store.c_str(),
                                   trust_store.length(),
                                   NULL, NULL, NULL)) {
        g_critical("Couldn't write truststore");
        return false;
    }
    m_trust_store_file = g_file_get_path(tmp_file);
    g_object_unref(tmp_file);
    g_object_unref(iostream);

    return true;
}

bool nsPluginInstance::RemoveTrustStoreFile()
{
    if (g_unlink(m_trust_store_file.c_str()) != 0)
        return false;;

    m_trust_store_file.clear();

    return true;
}

void nsPluginInstance::Connect()
{
    const int port = portToInt(m_port);
    const int sport = portToInt(m_secure_port);
    if (port < 0)
        g_warning("invalid port: '%s'", m_port.c_str());
    if (sport < 0)
        g_warning("invalid secure port: '%s'", m_secure_port.c_str());
    if (port <= 0 && sport <= 0)
    {
        m_connected_status = 1;
        CallOnDisconnected(m_connected_status);
        return;
    }

    if (!m_external_controller->StartClient()) {
        g_critical("failed to start SPICE client");
        return;
    }

    if (m_external_controller->Connect(10) != 0)
    {
        g_critical("could not connect to spice client controller");
        return;
    }

    if (!this->CreateTrustStoreFile(m_trust_store)) {
        g_critical("failed to create trust store");
        return;
    }

    SendInit();
    SendStr(CONTROLLER_HOST, m_host_ip);
    if (port > 0)
        SendValue(CONTROLLER_PORT, port);
    if (sport > 0)
        SendValue(CONTROLLER_SPORT, sport);
    SendValue(CONTROLLER_FULL_SCREEN,
            (m_fullscreen == true ? CONTROLLER_SET_FULL_SCREEN : 0) |
            (m_admin_console == false ? CONTROLLER_AUTO_DISPLAY_RES : 0));
    SendBool(CONTROLLER_ENABLE_SMARTCARD, m_smartcard);
    SendStr(CONTROLLER_PASSWORD, m_password);
    SendStr(CONTROLLER_TLS_CIPHERS, m_cipher_suite);
    SendStr(CONTROLLER_SET_TITLE, m_title);
    SendBool(CONTROLLER_SEND_CAD, m_send_ctrlaltdel);
    SendBool(CONTROLLER_ENABLE_USB_AUTOSHARE, m_usb_auto_share);
    SendStr(CONTROLLER_USB_FILTER, m_usb_filter);
    SendStr(CONTROLLER_SECURE_CHANNELS, m_ssl_channels);
    SendStr(CONTROLLER_CA_FILE, m_trust_store_file);
    SendStr(CONTROLLER_HOST_SUBJECT, m_host_subject);
    SendStr(CONTROLLER_HOTKEYS, m_hot_keys);
    SendValue(CONTROLLER_COLOR_DEPTH, atoi(m_color_depth.c_str()));
    SendStr(CONTROLLER_DISABLE_EFFECTS, m_disable_effects);
    SendMsg(CONTROLLER_CONNECT);
    SendMsg(CONTROLLER_SHOW);

    // set connected status
    m_connected_status = -1;
}

void nsPluginInstance::Show()
{
    g_debug("sending show message");
    SendMsg(CONTROLLER_SHOW);
}

void nsPluginInstance::Disconnect()
{
    m_external_controller->StopClient();
}

void nsPluginInstance::ConnectedStatus(int32_t *retval)
{
    *retval = m_connected_status;
}

void nsPluginInstance::SetLanguageStrings(const char *aSection, const char *aLanguage)
{
    if (aSection != NULL && aLanguage != NULL)
    {
        if (strlen(aSection) > 0 && strlen(aLanguage) > 0)
            m_language[aSection] = aLanguage;

    }
}

void nsPluginInstance::SetUsbFilter(const char *aUsbFilter)
{
    if (aUsbFilter != NULL)
        m_usb_filter = aUsbFilter;
}

void nsPluginInstance::CallOnDisconnected(int code)
{
    NPObject *window = NULL;
    if (NPN_GetValue(m_instance, NPNVWindowNPObject, &window) != NPERR_NO_ERROR)
    {
        g_critical("could not get browser window, when trying to call OnDisconnected");
        return;
    }

    // get OnDisconnected callback
    NPIdentifier id_on_disconnected = NPN_GetStringIdentifier("OnDisconnected");
    if (!id_on_disconnected)
    {
        g_critical("could not find OnDisconnected identifier");
        return;
    }

    NPVariant var_on_disconnected;
    if (!NPN_GetProperty(m_instance, window, id_on_disconnected, &var_on_disconnected))
    {
        g_critical("could not get OnDisconnected function");
        return;
    }

    if (!NPVARIANT_IS_OBJECT(var_on_disconnected))
    {
        g_critical("OnDisconnected is not object");
        return;
    }

    NPObject *call_on_disconnected = NPVARIANT_TO_OBJECT(var_on_disconnected);

    // call OnDisconnected
    NPVariant arg;
    NPVariant void_result;
    INT32_TO_NPVARIANT(code, arg);
    NPVariant args[] = { arg };

    if (NPN_InvokeDefault(m_instance, call_on_disconnected, args, sizeof(args) / sizeof(args[0]), &void_result))
        g_debug("OnDisconnected successfuly called");
    else
        g_critical("could not call OnDisconnected");

    // cleanup
    NPN_ReleaseObject(window);
    NPN_ReleaseVariantValue(&var_on_disconnected);
}

void nsPluginInstance::OnSpiceClientExit(int exit_code)
{
    m_connected_status = m_external_controller->TranslateRC(exit_code);
    if (!getenv("SPICE_XPI_DEBUG"))
    {
        CallOnDisconnected(exit_code);
        m_external_controller->Disconnect();
    }

    RemoveTrustStoreFile();
}

// ==============================
// ! Scriptability related code !
// ==============================
//
// here the plugin is asked by Mozilla to tell if it is scriptable
// we should return a valid interface id and a pointer to
// nsScriptablePeer interface which we should have implemented
// and which should be defined in the corressponding *.xpt file
// in the bin/components folder
NPError nsPluginInstance::GetValue(NPPVariable aVariable, void *aValue)
{
    // Here we indicate that the plugin is scriptable. See this page for details:
    // https://developer.mozilla.org/en/Gecko_Plugin_API_Reference/Scripting_plugins
    if (aVariable == NPPVpluginScriptableNPObject)
    {
        // addref happens in getter, so we don't addref here
        *(static_cast<NPObject **>(aValue)) = GetScriptablePeer();
    }
    return NPERR_NO_ERROR;
}

// ==============================
// ! Scriptability related code !
// ==============================
//
// this method will return the scriptable object (and create it if necessary)
NPObject *nsPluginInstance::GetScriptablePeer()
{
    if (!m_scriptable_peer)
        m_scriptable_peer = NPN_CreateObject(m_instance, GET_NPOBJECT_CLASS(ScriptablePluginObject));

    if (m_scriptable_peer)
        NPN_RetainObject(m_scriptable_peer);

    return m_scriptable_peer;
}
