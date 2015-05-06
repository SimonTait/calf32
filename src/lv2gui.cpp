/* Calf DSP Library utility application.
 * DSSI GUI application.
 * Copyright (C) 2007 Krzysztof Foltman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, 
 * Boston, MA  02110-1301  USA
 */
#include <sys/wait.h>
#include <unistd.h>
#include "config.h"
#if USE_LV2_GTK_GUI
#include <calf/gui.h>
#endif
#include <calf/giface.h>
#include <calf/lv2_data_access.h>
#include <calf/lv2_instance_access.h>
#include <calf/lv2_ui.h>
#include <calf/lv2_uri_map.h>
#include <calf/lv2_external_ui.h>
#include <calf/lv2_programs.h>
#include <calf/lv2helpers.h>
#include <calf/osctlnet.h>
#include <calf/utils.h>
#include <glib.h>

using namespace std;
using namespace calf_plugins;
using namespace calf_utils;
using namespace osctl;

struct LV2_Calf_Descriptor {
    plugin_ctl_iface *(*get_pci)(LV2_Handle Instance);
};

/// Temporary assignment to a slot in vector<bool>
typedef scope_assign<bool, vector<bool>::reference> TempSendSetter;

/// Common data and functions for GTK+ GUI and External GUI
struct plugin_proxy_base
{
    const plugin_metadata_iface *plugin_metadata;
    LV2UI_Write_Function write_function;
    LV2UI_Controller controller;

    // Values extracted from the Features array from the host
    
    /// Handle to the plugin instance
    LV2_Handle instance_handle;
    /// Data access feature instance
    LV2_Extension_Data_Feature *data_access;
    /// URI map feature
    LV2_URI_Map_Feature *uri_map;
    /// External UI host feature (must be set when instantiating external UI plugins)
    LV2_External_UI_Host *ext_ui_host;
    
    /// Instance pointer - usually NULL unless the host supports instance-access extension
    plugin_ctl_iface *instance;
    /// If true, a given parameter (not port) may be sent to host - it is blocked when the parameter is written to by the host
    vector<bool> sends;
    /// Map of parameter name to parameter index (used for mapping configure values to string ports)
    map<string, int> params_by_name;
    /// Values of parameters (float control ports)
    vector<float> params;
    /// Number of parameters (non-audio ports)
    int param_count;
    /// Number of the first parameter port
    int param_offset;
    
    plugin_proxy_base(const plugin_metadata_iface *metadata, LV2UI_Write_Function wf, LV2UI_Controller c, const LV2_Feature* const* features);

    /// Send a float value to a control port in the host
    void send_float_to_host(int param_no, float value);
    
    /// Send a string value to a string port in the host, by name (configure-like mechanism)
    char *configure(const char *key, const char *value);
    
    /// Enable sending to host for all ports
    void enable_all_sends();
    
    /// Obtain instance pointers
    void resolve_instance();

    /// Obtain line graph interface if available
    const line_graph_iface *get_line_graph_iface() const;
    
    /// Obtain phase graph interface if available
    const phase_graph_iface *get_phase_graph_iface() const;
    
    /// Map an URI to an integer value using a given URI map
    uint32_t map_uri(const char *mapURI, const char *keyURI);

};

plugin_proxy_base::plugin_proxy_base(const plugin_metadata_iface *metadata, LV2UI_Write_Function wf, LV2UI_Controller c, const LV2_Feature* const* features)
{
    plugin_metadata = metadata;
    
    write_function = wf;
    controller = c;

    instance = NULL;
    instance_handle = NULL;
    data_access = NULL;
    ext_ui_host = NULL;
    
    param_count = metadata->get_param_count();
    param_offset = metadata->get_param_port_offset();
    
    /// Block all updates until GUI is ready
    sends.resize(param_count, false);
    params.resize(param_count);
    for (int i = 0; i < param_count; i++)
    {
        const parameter_properties *pp = metadata->get_param_props(i);
        params_by_name[pp->short_name] = i;
        params[i] = pp->def_value;
    }
    for (int i = 0; features[i]; i++)
    {
        if (!strcmp(features[i]->URI, LV2_INSTANCE_ACCESS_URI))
        {
            instance_handle = features[i]->data;
        }
        else if (!strcmp(features[i]->URI, LV2_DATA_ACCESS_URI))
        {
            data_access = (LV2_Extension_Data_Feature *)features[i]->data;
        }
        else if (!strcmp(features[i]->URI, LV2_EXTERNAL_UI__Host))
        {
            ext_ui_host = (LV2_External_UI_Host *)features[i]->data;
        }
    }
    resolve_instance();
}

void plugin_proxy_base::send_float_to_host(int param_no, float value)
{
    params[param_no] = value;
    if (sends[param_no]) {
        TempSendSetter _a_(sends[param_no], false);
        write_function(controller, param_no + param_offset, sizeof(float), 0, &params[param_no]);
    }
}

void plugin_proxy_base::resolve_instance()
{
    fprintf(stderr, "CALF DEBUG: instance %p data %p\n", instance_handle, data_access);
    if (instance_handle && data_access)
    {
        LV2_Calf_Descriptor *calf = (LV2_Calf_Descriptor *)(*data_access->data_access)("http://foltman.com/ns/calf-plugin-instance");
        fprintf(stderr, "CALF DEBUG: calf %p cpi %p\n", calf, calf ? calf->get_pci : NULL);
        if (calf && calf->get_pci)
            instance = calf->get_pci(instance_handle);
    }
}

uint32_t plugin_proxy_base::map_uri(const char *mapURI, const char *keyURI)
{
    if (!uri_map)
        return 0;
    return uri_map->uri_to_id(uri_map->callback_data, mapURI, keyURI);
}

const line_graph_iface *plugin_proxy_base::get_line_graph_iface() const
{
    if (instance)
        return instance->get_line_graph_iface();
    return NULL;
}

const phase_graph_iface *plugin_proxy_base::get_phase_graph_iface() const
{
    if (instance)
        return instance->get_phase_graph_iface();
    return NULL;
}

char *plugin_proxy_base::configure(const char *key, const char *value)
{
    if (instance)
        return instance->configure(key, value);
    else
        return strdup("Configuration not available because of lack of instance-access/data-access");
}

void plugin_proxy_base::enable_all_sends()
{
    sends.clear();
    sends.resize(param_count, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if USE_LV2_GTK_GUI
/// Plugin controller that uses LV2 host with help of instance/data access to remotely
/// control a plugin from the GUI
struct lv2_plugin_proxy: public plugin_ctl_iface, public plugin_proxy_base, public gui_environment
{
    /// Plugin GTK+ GUI object pointer
    plugin_gui *gui;
    /// Glib source ID for update timer
    int source_id;
    
    lv2_plugin_proxy(const plugin_metadata_iface *md, LV2UI_Write_Function wf, LV2UI_Controller c, const LV2_Feature* const* f)
    : plugin_proxy_base(md, wf, c, f)
    {
        gui = NULL;
        if (instance)
        {
            conditions.insert("directlink");
            conditions.insert("configure");
        }
        conditions.insert("lv2gui");    
    }
    
    virtual bool activate_preset(int, int) {
        return false;
    }

    virtual float get_param_value(int param_no) {
        if (param_no < 0 || param_no >= param_count)
            return 0;
        return params[param_no];
    }
    
    virtual void set_param_value(int param_no, float value) {
        if (param_no < 0 || param_no >= param_count)
            return;
        send_float_to_host(param_no, value);
    }
    
    /// Override for a method in plugin_ctl_iface - trivial delegation to base class
    virtual char *configure(const char *key, const char *value) { return plugin_proxy_base::configure(key, value); }
    
    virtual float get_level(unsigned int port) { return 0.f; }
    virtual void execute(int command_no) { assert(0); }
    virtual void send_configures(send_configure_iface *sci)
    {    
        if (instance)
        {
            fprintf(stderr, "Send configures...\n");
            instance->send_configures(sci);
        }
        else
            fprintf(stderr, "Configuration not available because of lack of instance-access/data-access\n");
    }
    virtual int send_status_updates(send_updates_iface *sui, int last_serial)
    { 
        if (instance)
            return instance->send_status_updates(sui, last_serial);
        else // no status updates because of lack of instance-access/data-access
            return 0;
    }
    virtual const plugin_metadata_iface *get_metadata_iface() const { return plugin_metadata; }
    /// Override for a method in plugin_ctl_iface - trivial delegation to base class
    virtual const line_graph_iface *get_line_graph_iface() const { return plugin_proxy_base::get_line_graph_iface(); }
    
    /// Override for a method in plugin_ctl_iface - trivial delegation to base class
    virtual const phase_graph_iface *get_phase_graph_iface() const { return plugin_proxy_base::get_phase_graph_iface(); }
};

static gboolean plugin_on_idle(void *data)
{
    plugin_gui *self = (plugin_gui *)data;
    self->on_idle();
    return TRUE;
}

LV2UI_Handle gui_instantiate(const struct _LV2UI_Descriptor* descriptor,
                          const char*                     plugin_uri,
                          const char*                     bundle_path,
                          LV2UI_Write_Function            write_function,
                          LV2UI_Controller                controller,
                          LV2UI_Widget*                   widget,
                          const LV2_Feature* const*       features)
{
    const plugin_metadata_iface *md = plugin_registry::instance().get_by_uri(plugin_uri);
    if (!md)
        return NULL;
    lv2_plugin_proxy *proxy = new lv2_plugin_proxy(md, write_function, controller, features);
    if (!proxy)
        return NULL;
    
    gtk_rc_parse(PKGLIBDIR "calf.rc");
    
    plugin_gui_window *window = new plugin_gui_window(proxy, NULL);
    plugin_gui *gui = new plugin_gui(window);
    const char *xml = proxy->plugin_metadata->get_gui_xml();
    assert(xml);
    *(GtkWidget **)(widget) = gui->create_from_xml(proxy, xml);
    proxy->enable_all_sends();
    proxy->send_configures(gui);
    if (*(GtkWidget **)(widget))
        proxy->source_id = g_timeout_add_full(G_PRIORITY_LOW, 1000/30, plugin_on_idle, gui, NULL); // 30 fps should be enough for everybody    
    gui->show_rack_ears(proxy->get_config()->rack_ears);
    
    return (LV2UI_Handle)gui;
}

void gui_cleanup(LV2UI_Handle handle)
{
    plugin_gui *gui = (plugin_gui *)handle;
    lv2_plugin_proxy *proxy = dynamic_cast<lv2_plugin_proxy *>(gui->plugin);
    if (proxy->source_id)
        g_source_remove(proxy->source_id);
    delete gui;
}

void gui_port_event(LV2UI_Handle handle, uint32_t port, uint32_t buffer_size, uint32_t format, const void *buffer)
{
    plugin_gui *gui = (plugin_gui *)handle;
    lv2_plugin_proxy *proxy = dynamic_cast<lv2_plugin_proxy *>(gui->plugin);
    assert(proxy);
    float v = *(float *)buffer;
    int param = port - proxy->plugin_metadata->get_param_port_offset();
    if (param >= proxy->plugin_metadata->get_param_count())
        return;
    if (!proxy->sends[param])
        return;
    if (fabs(gui->plugin->get_param_value(param) - v) < 0.00001)
        return;
    {
        TempSendSetter _a_(proxy->sends[param], false);
        gui->set_param_value(param, v);
    }
}

const void *gui_extension(const char *uri)
{
    printf("gui_extension %s\n", uri);
    return NULL;
}

#endif

///////////////////////////////////////////////////////////////////////////////////////

class ext_plugin_gui: public LV2_External_UI_Widget, public plugin_proxy_base, public osc_message_sink<osc_strstream>, public send_updates_iface
{
public:
    GPid child_pid;
    osc_server srv;
    osc_client cli;
    bool confirmed;
    string prefix;
    dssi_feedback_sender *feedback_sender;
    bool enable_graph_updates;
    osc_inline_typed_strstream status_data;

    ext_plugin_gui(const plugin_metadata_iface *metadata, LV2UI_Write_Function wf, LV2UI_Controller c, const LV2_Feature* const* f);

    bool initialise();

    void show_impl();
    void hide_impl();
    void run_impl();
    void port_event_impl(uint32_t port, uint32_t buffer_size, uint32_t format, const void *buffer);

    bool activate_preset(int bank, int program)
    {
        if (confirmed) {
            osc_inline_typed_strstream str;
            str << (uint32_t)(bank) << (uint32_t)(program);
            cli.send("/program", str);
            return false;
        }
        return false;
    }

    virtual void send_status(const char *key, const char *value);
    virtual void receive_osc_message(std::string address, std::string args, osc_strstream &buffer);
    virtual ~ext_plugin_gui();
        
private:
    static void show_(LV2_External_UI_Widget *h) { ((ext_plugin_gui *)(h))->show_impl(); }
    static void hide_(LV2_External_UI_Widget *h) { ((ext_plugin_gui *)(h))->hide_impl(); }
    static void run_(LV2_External_UI_Widget *h) { ((ext_plugin_gui *)(h))->run_impl(); }
};

ext_plugin_gui::ext_plugin_gui(const plugin_metadata_iface *metadata, LV2UI_Write_Function wf, LV2UI_Controller c, const LV2_Feature* const* f)
: plugin_proxy_base(metadata, wf, c, f)
{
    confirmed = false;
    feedback_sender = NULL;
    
    show = show_;
    hide = hide_;
    run = run_;
}

bool ext_plugin_gui::initialise()
{
    if (ext_ui_host == NULL)
        return false;
    
    srv.sink = this;
    srv.bind("127.0.0.1");
    
    return true;
}

void ext_plugin_gui::show_impl()
{
    struct osc_configure_sender: public send_configure_iface
    {
        osc_client &cli;
        
        osc_configure_sender(osc_client &c)
        : cli(c)
        {
        }
        
        virtual void send_configure(const char *key, const char *value)
        {
            osc_inline_typed_strstream data;
            data << key;
            data << value;
            cli.send("/configure", data);
        }
    };

    osc_configure_sender sender(cli);
    
    if (instance)
        instance->send_configures(&sender);
    
    cli.send("/show");
}

void ext_plugin_gui::hide_impl()
{
    cli.send("/hide");
}

void ext_plugin_gui::send_status(const char *key, const char *value)
{
    status_data << key << value;
}

void ext_plugin_gui::port_event_impl(uint32_t port, uint32_t buffer_size, uint32_t format, const void *buffer)
{
    assert(confirmed);
    assert(port >= (uint32_t)param_offset);
    if (port >= (uint32_t)param_offset)
    {
        int param = port - param_offset;
        if (!sends[param])
            return;
        TempSendSetter _a_(sends[param], false);
        if (format == 0)
        {
            osc_inline_typed_strstream data;
            data << port; 
            data << *(float *)buffer;
            cli.send("/control", data);
        }
    }
}

void ext_plugin_gui::run_impl()
{
    srv.read_from_socket();
    if (waitpid(child_pid, NULL, WNOHANG) != 0)
    {
        ext_ui_host->ui_closed(controller);
        return;
    }
    if (feedback_sender && enable_graph_updates)
        feedback_sender->update();
}

void ext_plugin_gui::receive_osc_message(std::string address, std::string args, osc_strstream &buffer)
{
    if (address == "/bridge/update" && args == "s")
    {
        if (confirmed)
        {
            g_warning("Update message already received, ignoring");
            return;
        }
        string url;
        buffer >> url;
        cli.bind();
        cli.set_url(url.c_str());
        if (get_line_graph_iface())
        {
            feedback_sender = new dssi_feedback_sender(&cli, get_line_graph_iface());
            feedback_sender->add_graphs(plugin_metadata->get_param_props(0), param_count);
        }
        confirmed = true;
    }
    else
    if (address == "/bridge/control" && args == "if")
    {
        int port;
        float value;
        buffer >> port >> value;
        assert(port >= param_offset);
        send_float_to_host(port - param_offset, value);
    }
    else
    if (address == "/bridge/enable_updates" && args == "i")
    {
        int updates;
        buffer >> updates;
        enable_graph_updates = updates != 0;
        if (enable_graph_updates && feedback_sender)
            feedback_sender->update();
    }
    else
    if (address == "/bridge/configure" && (args == "s" || args == "ss"))
    {
        string key, value;
        buffer >> key;
        if (args == "ss")
        {
            buffer >> value;
            plugin_proxy_base::configure(key.c_str(), value.c_str());
        }
        else
            plugin_proxy_base::configure(key.c_str(), NULL);
    }
    else
    if (address == "/bridge/send_status" && args == "i")
    {
        if (instance)
        {
            int serial;
            buffer >> serial;

            status_data.clear();
            status_data << (uint32_t)instance->send_status_updates(this, serial);
            cli.send("/status_data", status_data);
        }
    }
    else
        srv.dump.receive_osc_message(address, args, buffer);
}

ext_plugin_gui::~ext_plugin_gui()
{
    if (confirmed)
    {
        cli.send("/quit");
    }
    if (feedback_sender)
        delete feedback_sender;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////

LV2UI_Handle extgui_instantiate(const struct _LV2UI_Descriptor* descriptor,
                          const char*                     plugin_uri,
                          const char*                     bundle_path,
                          LV2UI_Write_Function            write_function,
                          LV2UI_Controller                controller,
                          LV2UI_Widget*                   widget,
                          const LV2_Feature* const*       features)
{
    const plugin_metadata_iface *plugin_metadata = plugin_registry::instance().get_by_uri(plugin_uri);
    if (!plugin_metadata)
        return false;
    
    ext_plugin_gui *ui = new ext_plugin_gui(plugin_metadata, write_function, controller, features);
    if (!ui->initialise())
        return NULL;
    
    string url = ui->srv.get_url() + "/bridge";
    const gchar *argv[] = { "./calf_gtk", url.c_str(), "calf.so", plugin_uri, (ui->ext_ui_host->plugin_human_id ? ui->ext_ui_host->plugin_human_id : "Unknown"), NULL };
    GError *error = NULL;
    if (g_spawn_async(bundle_path, (gchar **)argv, NULL, (GSpawnFlags)G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &ui->child_pid, &error))
    {
        // wait for the sign of life from the GUI
        while(!ui->confirmed && waitpid(ui->child_pid, NULL, WNOHANG) == 0)
        {
            printf("Waiting for the GUI to open\n");
            ui->srv.read_from_socket();
            usleep(500000);
        }
        
        if (ui->confirmed)
        {
            *(LV2_External_UI_Widget **)widget = ui;
            ui->enable_all_sends();
            return (LV2UI_Handle)ui;
        }
        else
        {
            g_warning("The GUI exited before establishing contact with the host");
            return NULL;
        }
    }
    else
    {
        g_warning("%s", error->message);
        return NULL;
    }
}

void extgui_cleanup(LV2UI_Handle handle)
{
    ext_plugin_gui *gui = (ext_plugin_gui *)handle;
    delete gui;
}

void extgui_port_event(LV2UI_Handle handle, uint32_t port, uint32_t buffer_size, uint32_t format, const void *buffer)
{
    ((ext_plugin_gui *)handle)->port_event_impl(port, buffer_size, format, buffer);;
}

void extgui_select_program(LV2UI_Handle handle, uint32_t bank, uint32_t program)
{
    ((ext_plugin_gui *)handle)->activate_preset(bank, program);
}

const void *extgui_extension(const char *uri)
{
    printf("extgui_extension %s\n", uri);
    static LV2_Programs_UI_Interface uiProgramsExt = { extgui_select_program };
    if (strcmp(uri, LV2_PROGRAMS__UIInterface) == 0)
        return &uiProgramsExt;
    return NULL;
}

const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
#if USE_LV2_GTK_GUI
    static LV2UI_Descriptor gtkgui;
    gtkgui.URI = "http://calf.sourceforge.net/plugins/gui/gtk2-gui";
    gtkgui.instantiate = gui_instantiate;
    gtkgui.cleanup = gui_cleanup;
    gtkgui.port_event = gui_port_event;
    gtkgui.extension_data = gui_extension;
    if (!index--)
        return &gtkgui;
#endif
    static LV2UI_Descriptor extgui;
    extgui.URI = "http://calf.sourceforge.net/plugins/gui/ext-gui";
    extgui.instantiate = extgui_instantiate;
    extgui.cleanup = extgui_cleanup;
    extgui.port_event = extgui_port_event;
    extgui.extension_data = extgui_extension;
    if (!index--)
        return &extgui;

    return NULL;
}
