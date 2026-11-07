/* Build the policy from AIDL port and route enumeration. */

#ifndef DROID_AIDL_CONFIG_BUILDER_H
#define DROID_AIDL_CONFIG_BUILDER_H

#include <pulsecore/hashmap.h>

#include "aidl-binder.h"
#include "droid-config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The caller owns the returned config and port-id map. Map keys borrow port names;
 * values belong to the map. The module remains borrowed. Returns NULL on failure. */
dm_config_device *droid_aidl_build_config(aidl_audio_module *aidl_module,
                                          const char *module_name,
                                          pa_hashmap **aidl_port_ids_out);

/* Translate AIDL layoutMask (channel mask value from
 * aidl_audio_profile.channel_masks[]) into legacy audio_channel_mask_t,
 * direction-aware. */
audio_channel_mask_t droid_aidl_channel_mask_legacy(int aidl_mask, bool is_input);

/* Translate AIDL_AUDIO_FORMAT_* into legacy audio_format_t. */
audio_format_t droid_aidl_format_legacy(int aidl_format);

#ifdef __cplusplus
}
#endif

#endif /* DROID_AIDL_CONFIG_BUILDER_H */
