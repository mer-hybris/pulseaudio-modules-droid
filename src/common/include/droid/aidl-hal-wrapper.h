/* Adapt the AIDL client to the legacy audio_hw_device_t interface. */

#ifndef DROID_AIDL_HAL_WRAPPER_H
#define DROID_AIDL_HAL_WRAPPER_H

#include <hardware/audio.h>
#include <pulsecore/hashmap.h>

#include "aidl-binder.h"
#include "droid-config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Takes ownership of aidl_module and aidl_port_ids on success; config is borrowed.
 * Close with audio_hw_device_close() before freeing config. Returns NULL on failure. */
struct audio_hw_device *aidl_hal_wrapper_open(aidl_audio_module *aidl_module,
                                              dm_config_device *config,
                                              pa_hashmap *aidl_port_ids);

#ifdef __cplusplus
}
#endif

#endif /* DROID_AIDL_HAL_WRAPPER_H */
