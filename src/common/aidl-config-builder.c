/* Build dm_config_device from the HAL policy instead of an XML file. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/log.h>

#include <hardware/audio.h>

#include "droid/aidl-config-builder.h"
#include "droid/droid-config.h"
#include "droid/sllist.h"

/* ========================================================================== *
 *  Conversion helpers
 * ========================================================================== */

audio_format_t droid_aidl_format_legacy(int aidl_format) {
    switch (aidl_format) {
        case AIDL_AUDIO_FORMAT_S16LE:     return AUDIO_FORMAT_PCM_16_BIT;
        case AIDL_AUDIO_FORMAT_S32LE:     return AUDIO_FORMAT_PCM_32_BIT;
        case AIDL_AUDIO_FORMAT_FLOAT32LE: return AUDIO_FORMAT_PCM_FLOAT;
        case AIDL_AUDIO_FORMAT_S24LE:     return AUDIO_FORMAT_PCM_24_BIT_PACKED;
        case AIDL_AUDIO_FORMAT_U8:        return AUDIO_FORMAT_PCM_8_BIT;
        default:                          return AUDIO_FORMAT_PCM_16_BIT;
    }
}

audio_channel_mask_t droid_aidl_channel_mask_legacy(int aidl_mask, bool is_input) {
    if (!is_input) {
        /* AIDL AudioChannelLayout output position bits use the same numeric
         * values as legacy AUDIO_CHANNEL_OUT_*; pass through. */
        return (audio_channel_mask_t)aidl_mask;
    }

    /* Inputs: legacy uses a different bit layout for the typical
     * mono/stereo masks, so translate by channel count. */
    int popcount = __builtin_popcount(aidl_mask);
    switch (popcount) {
        case 1:  return AUDIO_CHANNEL_IN_MONO;
        case 2:  return AUDIO_CHANNEL_IN_STEREO;
        case 3:  return AUDIO_CHANNEL_IN_MONO | AUDIO_CHANNEL_IN_STEREO;
        default:
            /* Fall back to mono; richer layouts can be added if a HAL needs them. */
            return AUDIO_CHANNEL_IN_MONO;
    }
}

/* ========================================================================== *
 *  dm_config_port construction
 * ========================================================================== */

static dm_config_profile *make_profile(const struct aidl_audio_profile *src, bool is_input) {
    dm_config_profile *p = pa_xnew0(dm_config_profile, 1);

    p->name = pa_xstrdup("");

    if (src->format >= 0) {
        p->format = droid_aidl_format_legacy(src->format);
    } else {
        /* Non-PCM (e.g. offload). Mark as DEFAULT — droid-card only enables
         * mix ports with known formats. */
        p->format = AUDIO_FORMAT_DEFAULT;
    }

    int n_rates = src->sample_rates_count;
    if (n_rates > AUDIO_MAX_SAMPLING_RATES - 1)
        n_rates = AUDIO_MAX_SAMPLING_RATES - 1;
    for (int i = 0; i < n_rates; i++)
        p->sampling_rates[i] = (uint32_t)src->sample_rates[i];
    p->sampling_rates[n_rates] = 0;  /* 0 terminates the list */

    int n_cm = src->channel_masks_count;
    if (n_cm > AUDIO_MAX_CHANNEL_MASKS - 1)
        n_cm = AUDIO_MAX_CHANNEL_MASKS - 1;
    for (int i = 0; i < n_cm; i++)
        p->channel_masks[i] = droid_aidl_channel_mask_legacy(src->channel_masks[i], is_input);
    p->channel_masks[n_cm] = 0;

    return p;
}

static dm_config_port *make_device_port(dm_config_module *module,
                                        const struct aidl_audio_port *src) {
    dm_config_port *p = pa_xnew0(dm_config_port, 1);
    p->module = module;
    p->port_type = DM_CONFIG_TYPE_DEVICE_PORT;
    p->id = src->id;
    p->name = pa_xstrdup(src->name);
    /* Device port role: OUTPUT device receives audio (sink),
     * INPUT device produces audio (source). */
    p->role = (src->direction == AIDL_AUDIO_DIR_OUTPUT)
              ? DM_CONFIG_ROLE_SINK : DM_CONFIG_ROLE_SOURCE;
    p->type = (audio_devices_t)src->legacy_device_type;
    p->address = pa_xstrdup(src->device_address);
    p->profiles = dm_list_new();

    bool is_input = (src->direction == AIDL_AUDIO_DIR_INPUT);
    for (int i = 0; i < src->profiles_count; i++) {
        dm_config_profile *prof = make_profile(&src->profiles[i], is_input);
        dm_list_push_back(p->profiles, prof);
    }

    /* If no profiles defined (some HAL device ports report none), inject a
     * sensible default so droid-card doesn't reject the port. */
    if (dm_list_size(p->profiles) == 0) {
        dm_config_profile *def = pa_xnew0(dm_config_profile, 1);
        def->name = pa_xstrdup("generated-default");
        def->format = AUDIO_FORMAT_PCM_16_BIT;
        def->sampling_rates[0] = 48000;
        def->sampling_rates[1] = 0;
        def->channel_masks[0] = is_input ? AUDIO_CHANNEL_IN_STEREO : AUDIO_CHANNEL_OUT_STEREO;
        def->channel_masks[1] = 0;
        dm_list_push_back(p->profiles, def);
    }

    return p;
}

static dm_config_port *make_mix_port(dm_config_module *module,
                                     const struct aidl_audio_port *src) {
    dm_config_port *p = pa_xnew0(dm_config_port, 1);
    p->module = module;
    p->port_type = DM_CONFIG_TYPE_MIX_PORT;
    p->id = src->id;
    p->name = pa_xstrdup(src->name);
    /* Mix port role: OUTPUT mix is a source (audio originates here and flows
     * out to the device), INPUT mix is a sink (audio arrives here from the
     * device). Opposite of device port role. */
    p->role = (src->direction == AIDL_AUDIO_DIR_OUTPUT)
              ? DM_CONFIG_ROLE_SOURCE : DM_CONFIG_ROLE_SINK;
    p->flags = (uint32_t)src->io_flags;
    p->max_open_count = src->max_open_count;
    p->max_active_count = src->max_active_count;
    p->profiles = dm_list_new();

    bool is_input = (src->direction == AIDL_AUDIO_DIR_INPUT);
    for (int i = 0; i < src->profiles_count; i++) {
        dm_config_profile *prof = make_profile(&src->profiles[i], is_input);
        dm_list_push_back(p->profiles, prof);
    }

    return p;
}

static bool aidl_audio_port_is_attached_device(const struct aidl_audio_port *src) {
    if (src->port_type != AIDL_AUDIO_PORT_TYPE_DEVICE)
        return false;

    if (src->device_connection[0] != '\0')
        return false;

    if (src->legacy_device_type == AUDIO_DEVICE_OUT_DEFAULT ||
        src->legacy_device_type == AUDIO_DEVICE_IN_DEFAULT)
        return false;

    return true;
}

static bool aidl_audio_port_is_default_device(const struct aidl_audio_port *src) {
    return (src->device_flags & AIDL_AUDIO_PORT_DEVICE_FLAG_DEFAULT) != 0;
}

/* ========================================================================== *
 *  Helper: find dm_config_port by AIDL port id (via the id-map)
 * ========================================================================== */

struct port_id_entry {
    int aidl_id;
};

static dm_config_port *find_port_by_aidl_id(dm_config_module *module,
                                            pa_hashmap *aidl_port_ids,
                                            int aidl_id) {
    dm_config_port *p;
    void *state;
    DM_LIST_FOREACH_DATA(p, module->ports, state) {
        struct port_id_entry *e = pa_hashmap_get(aidl_port_ids, p->name);
        if (e && e->aidl_id == aidl_id)
            return p;
    }
    return NULL;
}

/* ========================================================================== *
 *  Top-level builder
 * ========================================================================== */

dm_config_device *droid_aidl_build_config(aidl_audio_module *aidl_module,
                                          const char *module_name,
                                          pa_hashmap **aidl_port_ids_out) {
    struct aidl_audio_port *ports = NULL;
    int ports_count = 0;
    struct aidl_audio_route *routes = NULL;
    int routes_count = 0;
    dm_config_device *config = NULL;
    dm_config_module *module = NULL;
    dm_config_port *first_output_device = NULL;
    dm_config_port *flagged_default_output = NULL;
    dm_config_port *speaker_output = NULL;
    pa_hashmap *port_ids = NULL;
    int rc;

    pa_assert(aidl_module);
    pa_assert(module_name);
    pa_assert(aidl_port_ids_out);

    rc = aidl_audio_module_get_ports(aidl_module, &ports, &ports_count);
    if (rc < 0) {
        pa_log("aidl-config-builder: failed to enumerate ports: %d", rc);
        goto fail;
    }

    rc = aidl_audio_module_get_routes(aidl_module, &routes, &routes_count);
    if (rc < 0) {
        pa_log("aidl-config-builder: failed to enumerate routes: %d", rc);
        goto fail;
    }

    config = pa_xnew0(dm_config_device, 1);
    config->global_config = dm_list_new();
    config->modules = dm_list_new();

    module = pa_xnew0(dm_config_module, 1);
    module->config = config;
    module->name = pa_xstrdup(module_name);
    module->version_major = 0;
    module->version_minor = 0;
    module->attached_devices = dm_list_new();
    module->mix_ports = dm_list_new();
    module->device_ports = dm_list_new();
    module->ports = dm_list_new();
    module->routes = dm_list_new();

    dm_list_push_back(config->modules, module);

    port_ids = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                   pa_idxset_string_compare_func,
                                   NULL, pa_xfree);

    /* Pass 1: create dm_config_port for every device port. */
    for (int i = 0; i < ports_count; i++) {
        if (ports[i].port_type != AIDL_AUDIO_PORT_TYPE_DEVICE) continue;

        dm_config_port *cp = make_device_port(module, &ports[i]);
        dm_list_push_back(module->ports, cp);
        dm_list_push_back(module->device_ports, cp);

        if (aidl_audio_port_is_attached_device(&ports[i]))
            dm_list_push_back(module->attached_devices, cp);

        struct port_id_entry *e = pa_xnew0(struct port_id_entry, 1);
        e->aidl_id = ports[i].id;
        pa_hashmap_put(port_ids, cp->name, e);

        if (cp->role == DM_CONFIG_ROLE_SINK) {
            if (!first_output_device)
                first_output_device = cp;

            if (!flagged_default_output &&
                aidl_audio_port_is_default_device(&ports[i]) &&
                ports[i].legacy_device_type != AUDIO_DEVICE_OUT_DEFAULT)
                flagged_default_output = cp;

            if (!speaker_output &&
                ports[i].legacy_device_type == AUDIO_DEVICE_OUT_SPEAKER)
                speaker_output = cp;
        }
    }

    module->default_output_device = flagged_default_output
                                  ? flagged_default_output
                                  : speaker_output ? speaker_output : first_output_device;

    /* Pass 2: mix ports. */
    for (int i = 0; i < ports_count; i++) {
        if (ports[i].port_type != AIDL_AUDIO_PORT_TYPE_MIX) continue;

        dm_config_port *cp = make_mix_port(module, &ports[i]);
        dm_list_push_back(module->ports, cp);
        dm_list_push_back(module->mix_ports, cp);

        struct port_id_entry *e = pa_xnew0(struct port_id_entry, 1);
        e->aidl_id = ports[i].id;
        pa_hashmap_put(port_ids, cp->name, e);
    }

    /* Pass 3: routes. */
    for (int i = 0; i < routes_count; i++) {
        dm_config_route *cr = pa_xnew0(dm_config_route, 1);
        cr->sources = dm_list_new();

        dm_config_port *sink = find_port_by_aidl_id(module, port_ids,
                                                    routes[i].sink_port_id);
        if (!sink) {
            pa_log_warn("aidl-config-builder: route sink port id %d not found",
                        routes[i].sink_port_id);
            dm_list_free(cr->sources, NULL);
            pa_xfree(cr);
            continue;
        }
        cr->sink = sink;
        if (sink->port_type == DM_CONFIG_TYPE_MIX_PORT)
            sink->role = DM_CONFIG_ROLE_SINK;
        cr->type = sink->port_type;

        for (int j = 0; j < routes[i].sources_count; j++) {
            dm_config_port *src = find_port_by_aidl_id(module, port_ids,
                                                       routes[i].source_port_ids[j]);
            if (src) {
                if (src->port_type == DM_CONFIG_TYPE_MIX_PORT)
                    src->role = DM_CONFIG_ROLE_SOURCE;
                dm_list_push_back(cr->sources, src);
            }
        }

        if (dm_list_size(cr->sources) == 0) {
            dm_list_free(cr->sources, NULL);
            pa_xfree(cr);
            continue;
        }
        dm_list_push_back(module->routes, cr);
    }

    pa_log_info("aidl-config-builder: built config: %zd device ports, %zd mix ports, %zd routes",
                dm_list_size(module->device_ports),
                dm_list_size(module->mix_ports),
                dm_list_size(module->routes));

    pa_xfree(ports);
    pa_xfree(routes);

    *aidl_port_ids_out = port_ids;
    return config;

fail:
    if (ports) pa_xfree(ports);
    if (routes) pa_xfree(routes);
    if (port_ids) pa_hashmap_free(port_ids);
    if (config) {
        /* dm_config_free will walk and clean up modules/ports/profiles. */
        dm_config_free(config);
    }
    return NULL;
}
