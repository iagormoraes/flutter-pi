#include "flutter_webrtc.h"

#include <stdlib.h>
#include <string.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "util/logging.h"

FLUTTERPI_PLUGIN("flutter_webrtc", flutter_webrtc_plugin, flutter_webrtc_init, flutter_webrtc_deinit)

struct flutter_webrtc_plugin {
    // Plugin state
};

static int on_method_call(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    LOG_DEBUG("flutter_webrtc: method call on channel %s\n", channel);

    // Parse method and arguments from object
    if (object->type != kMap) {
        platch_respond_illegal_arg(responsehandle, "Expected map");
        return 0;
    }

    // Extract method name
    struct platch_obj *method_obj = platch_obj_get_map_value(object, "method");
    if (method_obj == NULL || method_obj->type != kString) {
        platch_respond_illegal_arg(responsehandle, "Missing or invalid method");
        return 0;
    }

    const char *method = method_obj->string_value;

    LOG_DEBUG("flutter_webrtc: handling method %s\n", method);

#ifdef HAVE_LIBWEBRTC
    // TODO: Implement full WebRTC functionality
    platch_respond_not_implemented(responsehandle);
#else
    // Stub implementation
    if (strcmp(method, "initialize") == 0) {
        // Respond with success for initialize
        platch_respond_success(responsehandle, NULL);
    } else {
        LOG_INFO("flutter_webrtc: stub - method %s not implemented\n", method);
        platch_respond_not_implemented(responsehandle);
    }
#endif

    return 0;
}

enum plugin_init_result flutter_webrtc_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct flutter_webrtc_plugin *plugin;

    plugin = calloc(1, sizeof(*plugin));
    if (plugin == NULL) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    // Register method channel handler
    int ok = plugin_registry_set_receiver_v2(
        flutterpi_get_plugin_registry(flutterpi),
        "FlutterWebRTC.Method",
        on_method_call,
        plugin
    );
    if (ok != 0) {
        LOG_ERROR("flutter_webrtc: failed to register method channel\n");
        free(plugin);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = plugin;
    LOG_INFO("flutter_webrtc: plugin initialized\n");
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void flutter_webrtc_deinit(struct flutterpi *flutterpi, void *userdata) {
    struct flutter_webrtc_plugin *plugin = userdata;

    // Unregister channel
    plugin_registry_remove_receiver_v2(
        flutterpi_get_plugin_registry(flutterpi),
        "FlutterWebRTC.Method"
    );

    free(plugin);
    LOG_INFO("flutter_webrtc: plugin deinitialized\n");
}