#ifndef _FLUTTERPI_SRC_PLUGINS_FLUTTER_WEBRTC_H
#define _FLUTTERPI_SRC_PLUGINS_FLUTTER_WEBRTC_H

#include <glib.h>
#include <stdint.h>

#include "pluginregistry.h"

struct flutterpi;

enum plugin_init_result flutter_webrtc_init(struct flutterpi *flutterpi, void **userdata_out);

void flutter_webrtc_deinit(struct flutterpi *flutterpi, void *userdata);

#endif // _FLUTTERPI_SRC_PLUGINS_FLUTTER_WEBRTC_H