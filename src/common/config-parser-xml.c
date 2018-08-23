/*
 * Copyright (C) 2018 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@jolla.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <pulsecore/log.h>

#include "droid-config.h"

#ifndef HAVE_EXPAT
#include <unistd.h>
pa_droid_config_audio *pa_parse_droid_audio_config_xml(const char *filename) {
    if (access(filename, F_OK) == 0)
        pa_log_warn("Could not parse %s, xml configuration parsing support not compiled in", filename);
    return NULL;
}
#else

#include <stdarg.h>
#include <string.h>
#include <expat.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>

#include "conversion.h"
#include "sllist.h"

#ifdef XML_UNICODE_WCHAR_T
# include <wchar.h>
# define XML_FMT_STR "ls"
#else
# define XML_FMT_STR "s"
#endif

#define POLICY_SUPPORTED_VERSION            "1.0"

#define ELEMENT_audioPolicyConfiguration    "audioPolicyConfiguration"
#define   ELEMENT_globalConfiguration       "globalConfiguration"
#define   ELEMENT_modules                   "modules"
#define     ELEMENT_module                  "module"
#define       ELEMENT_attachedDevices       "attachedDevices"
#define         ELEMENT_item                "item"
#define       ELEMENT_defaultOutputDevice   "defaultOutputDevice"
#define       ELEMENT_mixPorts              "mixPorts"
#define         ELEMENT_mixPort             "mixPort"
#define           ELEMENT_profile           "profile"
#define       ELEMENT_devicePorts           "devicePorts"
#define         ELEMENT_devicePort          "devicePort"
/*                ELEMENT_profile */
#define       ELEMENT_routes                "routes"
#define         ELEMENT_route               "route"

#define ATTRIBUTE_version                   "version"
#define ATTRIBUTE_name                      "name"
#define ATTRIBUTE_halVersion                "halVersion"
#define ATTRIBUTE_format                    "format"
#define ATTRIBUTE_samplingRates             "samplingRates"
#define ATTRIBUTE_channelMasks              "channelMasks"
#define ATTRIBUTE_tagName                   "tagName"
#define ATTRIBUTE_role                      "role"
#define ATTRIBUTE_flags                     "flags"
#define ATTRIBUTE_sink                      "sink"
#define ATTRIBUTE_sources                   "sources"
#define ATTRIBUTE_type                      "type"

#define PORT_TYPE_sink                      "sink"
#define PORT_TYPE_source                    "source"


struct parser_data;

struct element_parser {
    const char *name;
    bool (*attributes)(struct parser_data *data, const char *element_name, const XML_Char **attributes);
    void (*char_data)(struct parser_data *data, const char *str);
    const struct element_parser *next;
    const struct element_parser *child;
};

struct element_parser_stack {
    const struct element_parser *data;
    struct element_parser_stack *next;
};

#define ELEMENT_STACK_PUSH(_stack, _item)       \
    do {                                        \
        struct element_parser_stack *_i;        \
        _i = pa_xmalloc0(sizeof(*_i));          \
        _i->data = _item;                       \
        _i->next = _stack;                      \
        _stack = _i;                            \
    } while(0)

#define ELEMENT_STACK_POP(_stack, _item)        \
    do {                                        \
        if (_stack) {                           \
            struct element_parser_stack *_t;    \
            _t = _stack;                        \
            _item = _stack->data;               \
            _stack = _stack->next;              \
            pa_xfree(_t);                       \
        } else                                  \
            _item = NULL;                       \
    } while(0)

static bool parse_audio_policy_configuration(struct parser_data *data, const char *element_name, const XML_Char **attributes);
static bool parse_route(struct parser_data *data, const char *element_name, const XML_Char **attributes);
static bool parse_profile(struct parser_data *data, const char *element_name, const XML_Char **attributes);
static bool parse_device_port(struct parser_data *data, const char *element_name, const XML_Char **attributes);
static bool parse_mix_port(struct parser_data *data, const char *element_name, const XML_Char **attributes);
static void parse_default_output_device(struct parser_data *data, const char *str);
static void parse_item(struct parser_data *data, const char *str);
static bool parse_module(struct parser_data *data, const char *element_name, const XML_Char **attributes);
static bool parse_global_configuration(struct parser_data *data, const char *element_name, const XML_Char **attributes);

static const struct element_parser element_parse_route = {
    ELEMENT_route,
    parse_route,
    NULL,
    NULL,
    NULL
};

static const struct element_parser element_parse_routes = {
    ELEMENT_routes,
    NULL,
    NULL,
    NULL,
    &element_parse_route
};

static const struct element_parser element_parse_profile = {
    ELEMENT_profile,
    parse_profile,
    NULL,
    NULL,
    NULL
};

static const struct element_parser element_parse_device_port = {
    ELEMENT_devicePort,
    parse_device_port,
    NULL,
    NULL,
    &element_parse_profile
};

static const struct element_parser element_parse_device_ports = {
    ELEMENT_devicePorts,
    NULL,
    NULL,
    &element_parse_routes,
    &element_parse_device_port
};

static const struct element_parser element_parse_mix_port = {
    ELEMENT_mixPort,
    parse_mix_port,
    NULL,
    NULL,
    &element_parse_profile
};

static const struct element_parser element_parse_mix_ports = {
    ELEMENT_mixPorts,
    NULL,
    NULL,
    &element_parse_device_ports,
    &element_parse_mix_port
};

static const struct element_parser element_parse_default_output_device = {
    ELEMENT_defaultOutputDevice,
    NULL,
    parse_default_output_device,
    &element_parse_mix_ports,
    NULL
};

static const struct element_parser element_parse_item = {
    ELEMENT_item,
    NULL,
    parse_item,
    NULL,
    NULL
};

static const struct element_parser element_parse_attached_devices = {
    ELEMENT_attachedDevices,
    NULL,
    NULL,
    &element_parse_default_output_device,
    &element_parse_item
};

static const struct element_parser element_parse_module = {
    ELEMENT_module,
    parse_module,
    NULL,
    NULL,
    &element_parse_attached_devices
};

static const struct element_parser element_parse_modules = {
    ELEMENT_modules,
    NULL,
    NULL,
    NULL,
    &element_parse_module
};

static const struct element_parser element_parse_global_configuration = {
    ELEMENT_globalConfiguration,
    parse_global_configuration,
    NULL,
    &element_parse_modules,
    NULL
};

static const struct element_parser element_parse_audio_policy_configuration = {
    ELEMENT_audioPolicyConfiguration,
    parse_audio_policy_configuration,
    NULL,
    NULL,
    &element_parse_global_configuration
};

static const struct element_parser element_parse_root = {
    NULL,
    NULL,
    NULL,
    NULL,
    &element_parse_audio_policy_configuration
};


struct global_configuration {
    char *key;
    char *value;
    struct global_configuration *next;
};

struct device {
    char *name;
    struct device *next;
};

struct profile {
    char *name;
    audio_format_t format;
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES];
    audio_channel_mask_t channel_masks;
    struct profile *next;
};

struct mix_port {
    char *name;
    char *role;
    uint32_t flags;
    struct profile *profiles;
    struct mix_port *next;
};

struct device_port {
    char *tag_name;
    audio_devices_t type;
    char *role;
    struct profile *profiles;
    struct device_port *next;
};

struct route {
    char *type;
    char *sink;
    struct device *sources;
    struct route *next;
};

struct module {
    char *name;
    uint32_t version;
    struct device *attached_devices;
    struct device *default_output;
    struct mix_port *mix_ports;
    struct device_port *device_ports;
    struct route *routes;

    struct module *next;
};

struct audio_policy_configuration {
    struct global_configuration *global;
    struct module *modules;
};

struct parser_data {
    XML_Parser parser;
    const char *fn;
    unsigned lineno;

    const struct element_parser *root;
    const struct element_parser *current;
    struct element_parser_stack *stack;

    struct audio_policy_configuration *conf;
    struct module *current_module;
    struct mix_port *current_mix_port;
    struct device_port *current_device_port;
};


static char *xml_string_dup(const XML_Char *xml_str, int len)
{
    char *str;

    if (len > 0) {
        str = pa_xmalloc0(len + 1);
        snprintf(str, len + 1, "%" XML_FMT_STR, xml_str);
    } else
        str = pa_sprintf_malloc("%" XML_FMT_STR, xml_str);

    return str;
}

static void device_free(struct device *d) {
    pa_assert(d);
    pa_xfree(d->name);
    pa_xfree(d);
}

static void profile_free(struct profile *p) {
    pa_assert(p);
    pa_xfree(p->name);
    pa_xfree(p);
}

static void mix_port_free(struct mix_port *p) {
    struct profile *profile;

    pa_assert(p);

    while (p->profiles) {
        SLLIST_STEAL_FIRST(profile, p->profiles);
        profile_free(profile);
    };

    pa_xfree(p->name);
    pa_xfree(p->role);
    pa_xfree(p);
}

static void device_port_free(struct device_port *p) {
    pa_assert(p);
    pa_xfree(p->tag_name);
    pa_xfree(p->role);
    pa_xfree(p);
}

static void route_free(struct route *r) {
    struct device *d;

    pa_assert(r);

    while (r->sources) {
        SLLIST_STEAL_FIRST(d, r->sources);
        device_free(d);
    }
    pa_xfree(r->type);
    pa_xfree(r->sink);
    pa_xfree(r);
}

static void module_free(struct module *m) {
    struct device *dev;
    struct mix_port *mix_port;
    struct device_port *device_port;
    struct route *route;

    pa_assert(m);

    while (m->attached_devices) {
        SLLIST_STEAL_FIRST(dev, m->attached_devices);
        device_free(dev);
    };

    while (m->default_output) {
        SLLIST_STEAL_FIRST(dev, m->default_output);
        device_free(dev);
    };

    while (m->mix_ports) {
        SLLIST_STEAL_FIRST(mix_port, m->mix_ports);
        mix_port_free(mix_port);
    };

    while (m->device_ports) {
        SLLIST_STEAL_FIRST(device_port, m->device_ports);
        device_port_free(device_port);
    };

    while (m->routes) {
        SLLIST_STEAL_FIRST(route, m->routes);
        route_free(route);
    };

    pa_xfree(m->name);
    pa_xfree(m);
}

static void audio_policy_configuration_free(struct audio_policy_configuration *xml_config) {
    struct global_configuration *global;
    struct module *m;

    pa_assert(xml_config);

    while (xml_config->global) {
        SLLIST_STEAL_FIRST(global, xml_config->global);
        pa_xfree(global->key);
        pa_xfree(global->value);
        pa_xfree(global);
    }

    while (xml_config->modules) {
        SLLIST_STEAL_FIRST(m, xml_config->modules);
        module_free(m);
    };

    pa_xfree(xml_config);
}

static bool get_element_attr(struct parser_data *data, const XML_Char **attr, bool required,
                             const char *key, char **ret_value) {
    int i;
    bool found = false;

    pa_assert(attr);
    pa_assert(key);
    pa_assert(ret_value);

    for (i = 0; attr[i]; i += 2) {
        if (pa_streq(attr[i], key)) {
            *ret_value = xml_string_dup(attr[i + 1], -1);
            found = true;
            break;
        }
    }

    if (!found && required)
        pa_log_warn("[%s:%u] Could not find element attribute \"%s\"", data->fn, data->lineno, key);

    return found;
}

static bool get_element_attrs(struct parser_data *data, const XML_Char **attr, ...) {
    const char *key;
    char **ret_value;
    va_list ap;
    uint32_t keys = 0;
    uint32_t found = 0;

    va_start(ap, attr);
    for (;;) {
        key = va_arg(ap, const char *);
        if (!key)
            break;
        keys++;

        ret_value = va_arg(ap, char **);
        if (get_element_attr(data, attr, true, key, ret_value))
            found++;
    }
    va_end(ap);

    return keys == found;
}

static void XMLCALL xml_start_element(void *userdata, const XML_Char *element_name, const XML_Char **attributes) {
    struct parser_data *data = userdata;
    char *element = NULL;
    const struct element_parser *node;

    element = xml_string_dup(element_name, -1);
    data->lineno = (unsigned) XML_GetCurrentLineNumber(data->parser);

    SLLIST_FOREACH(node, data->current->child) {
        if (pa_streq(node->name, element)) {
            if (node->attributes) {
                if (!node->attributes(data, element, attributes))
                    goto fail;
            }

            ELEMENT_STACK_PUSH(data->stack, data->current);
            data->current = node;
            break;
        }
    }

    pa_xfree(element);

    return;

fail:
    while (data->stack)
        ELEMENT_STACK_POP(data->stack, node);

    pa_xfree(element);

    XML_StopParser(data->parser, 0);
}

static void XMLCALL xml_end_element(void *userdata, const XML_Char *element_name) {
    struct parser_data *data = userdata;
    char *element = NULL;

    element = xml_string_dup(element_name, -1);

    if (pa_streq(data->current->name, element)) {
        ELEMENT_STACK_POP(data->stack, data->current);

        if (pa_streq(element, ELEMENT_mixPort))
            data->current_mix_port = NULL;
        else if (pa_streq(element, ELEMENT_devicePort))
            data->current_device_port = NULL;
        else if (pa_streq(element, ELEMENT_module))
            data->current_module = NULL;
    }

    pa_xfree(element);
}

static void XMLCALL xml_character_data_handler(void *userdata, const XML_Char *s, int len) {
    struct parser_data *data = userdata;
    int whitespace = 0;
    char *str = NULL;

    if (len <= 0)
        goto done;

    str = xml_string_dup(s, len);
    whitespace = strspn(str, "\r\n\t ");

    if (whitespace == len)
        goto done;

    if (data->current->char_data)
        data->current->char_data(data, str);

done:
    pa_xfree(str);
}

static bool parse_audio_policy_configuration(struct parser_data *data,
                                             const char *element_name,
                                             const XML_Char **attributes) {
    char *version = NULL;

    if (!get_element_attr(data, attributes, true, ATTRIBUTE_version, &version))
        return false;

    if (!pa_streq(version, POLICY_SUPPORTED_VERSION)) {
        pa_log_warn("[%s:%u] We only support " ELEMENT_audioPolicyConfiguration
                    " version " POLICY_SUPPORTED_VERSION ". Expect problems.",
                    data->fn, data->lineno);
    }

    pa_xfree(version);

    return true;
}

static bool parse_module(struct parser_data *data, const char *element_name, const XML_Char **attributes) {
    struct module *m;
    bool parsed = false;
    char *halVersion = NULL;

    m = pa_xmalloc0(sizeof(*m));

    if (!get_element_attrs(data, attributes,
                           ATTRIBUTE_name, &m->name,
                           ATTRIBUTE_halVersion, &halVersion,
                           NULL))
        goto done;

    if (!pa_conversion_parse_version(data->fn, data->lineno, halVersion, &m->version))
        goto done;

    parsed = true;
done:
    pa_xfree(halVersion);

    if (parsed) {
        SLLIST_APPEND(struct module, data->conf->modules, m);
        data->current_module = m;
        pa_log_debug("New " ELEMENT_module ": \"%s\"", m->name);
    } else {
        pa_log("[%s:%u] Failed to parse element <" ELEMENT_module ">", data->fn, data->lineno);
        module_free(m);
    }

    return parsed;
}

static bool parse_global_configuration(struct parser_data *data, const char *element_name, const XML_Char **attributes) {
    int i;

    for (i = 0; attributes[i]; i += 2) {
        struct global_configuration *c;
        c = pa_xmalloc0(sizeof(*c));
        c->key = xml_string_dup(attributes[i], -1);
        c->value = xml_string_dup(attributes[i + 1], -1);
        SLLIST_APPEND(struct global_configuration, data->conf->global, c);
    }

    return true;
}

static void parse_item(struct parser_data *data, const char *str) {
    struct device *d;

    d = pa_xmalloc0(sizeof(*d));
    d->name = pa_xstrdup(str);
    SLLIST_APPEND(struct device, data->current_module->attached_devices, d);
}

static void parse_default_output_device(struct parser_data *data, const char *str) {
    struct device *d;

    d = pa_xmalloc0(sizeof(*d));
    d->name = pa_xstrdup(str);
    SLLIST_APPEND(struct device, data->current_module->default_output, d);
}

static bool parse_mix_port(struct parser_data *data, const char *element_name, const XML_Char **attributes) {
    struct mix_port *p;
    bool parsed = false;
    char *flags = NULL;

    p = pa_xmalloc0(sizeof(*p));

    if (!get_element_attrs(data, attributes,
                           ATTRIBUTE_name, &p->name,
                           ATTRIBUTE_role, &p->role,
                           NULL))
        goto done;

    /* flags is not mandatory element attribute */
    if (get_element_attr(data, attributes, false, ATTRIBUTE_flags, &flags)) {
        if (!(pa_streq(p->role, PORT_TYPE_source) ?
                pa_conversion_parse_output_flags(data->fn, data->lineno, flags, &p->flags)
              : pa_conversion_parse_input_flags(data->fn, data->lineno, flags, &p->flags)))
            goto done;
    }

    parsed = true;
done:
    pa_xfree(flags);

    if (parsed) {
        SLLIST_APPEND(struct mix_port, data->current_module->mix_ports, p);
        data->current_mix_port = p;
    } else {
        pa_log("[%s:%u] Failed to parse element <" ELEMENT_mixPort ">", data->fn, data->lineno);
        mix_port_free(p);
    }

    return parsed;
}

static void replace_in_place(char **string, const char *a, const char *b) {
    char *tmp;

    pa_assert(*string);
    pa_assert(a);
    pa_assert(b);

    tmp = pa_replace(*string, a, b);
    pa_xfree(*string);
    *string = tmp;
}

static bool parse_profile(struct parser_data *data, const char *element_name, const XML_Char **attributes) {
    struct profile *p;
    bool parsed = false, unknown_format = false, output = true;
    char *samplingRates = NULL, *channelMasks = NULL, *format = NULL;

    /* if the parsing of parent devicePort or mixPort failed
     * we skip child profiles as well. */
    if (!data->current_mix_port && !data->current_device_port)
        return true;

    p = pa_xmalloc0(sizeof(*p));

    if (!get_element_attrs(data, attributes,
                           ATTRIBUTE_name, &p->name,
                           ATTRIBUTE_format, &format,
                           ATTRIBUTE_samplingRates, &samplingRates,
                           NULL))
        goto done;

    if (data->current_mix_port)
        output = pa_streq(data->current_mix_port->role, PORT_TYPE_source);
    else if (data->current_device_port)
        output = pa_streq(data->current_device_port->role, PORT_TYPE_sink);
    else
        pa_assert_not_reached();

    /* some devicePorts do not have channel masks */
    get_element_attr(data, attributes, false, ATTRIBUTE_channelMasks, &channelMasks);

    /* Hard-coded workaround for incorrect audio policy configuration. */
    if (channelMasks && data->current_device_port) {
        if (output && pa_startswith(channelMasks, "AUDIO_CHANNEL_IN_")) {
            pa_log_info("[%s:%u] Output has wrong direction channel mask (%s), reversing.",
                        data->fn, data->lineno, channelMasks);
            replace_in_place(&channelMasks, "AUDIO_CHANNEL_IN_", "AUDIO_CHANNEL_OUT_");
        }
        else if (!output && pa_startswith(channelMasks, "AUDIO_CHANNEL_OUT_")) {
            pa_log_info("[%s:%u] Input has wrong direction channel mask (%s), reversing.",
                        data->fn, data->lineno, channelMasks);
            replace_in_place(&channelMasks, "AUDIO_CHANNEL_OUT_", "AUDIO_CHANNEL_IN_");
        }
    }

    if (!pa_conversion_parse_sampling_rates(data->fn, data->lineno, samplingRates, false, p->sampling_rates))
        goto done;

    if (!pa_conversion_parse_formats(data->fn, data->lineno, format, false, &p->format))
        unknown_format = true;

    if (!unknown_format && channelMasks && !(output ?
            pa_conversion_parse_output_channels(data->fn, data->lineno, channelMasks, false, &p->channel_masks)
          : pa_conversion_parse_input_channels(data->fn, data->lineno, channelMasks, false, &p->channel_masks)))
        goto done;

    parsed = true;
done:
    pa_xfree(samplingRates);
    pa_xfree(channelMasks);
    pa_xfree(format);

    if (!parsed) {
        pa_log_error("[%s:%u] Failed to parse element <" ELEMENT_profile ">", data->fn, data->lineno);
        profile_free(p);
    } else if (unknown_format) {
        pa_log_info("[%s:%u] Ignore profile with unknown format.", data->fn, data->lineno);
        profile_free(p);
    } else {
        if (data->current_mix_port)
            SLLIST_APPEND(struct profile, data->current_module->mix_ports->profiles, p);
        else if (data->current_device_port)
            SLLIST_APPEND(struct profile, data->current_module->device_ports->profiles, p);
        else
            pa_assert_not_reached();
    }

    return parsed;
}

static bool parse_device_port(struct parser_data *data, const char *element_name, const XML_Char **attributes) {
    struct device_port *d;
    bool parsed = false, unknown_device = false;
    char *type = NULL;

    d = pa_xmalloc0(sizeof(*d));

    if (!get_element_attrs(data, attributes,
                           ATTRIBUTE_tagName, &d->tag_name,
                           ATTRIBUTE_role, &d->role,
                           NULL))
        goto done;

    if (!get_element_attr(data, attributes, true, ATTRIBUTE_type, &type))
        goto done;

    if (!(pa_streq(d->role, ATTRIBUTE_sink) ?
            pa_conversion_parse_output_devices(data->fn, data->lineno, type, false, false, &d->type)
          : pa_conversion_parse_input_devices(data->fn, data->lineno, type, false, false, &d->type)))
        unknown_device = true;

    parsed = true;
done:
    pa_xfree(type);

    if (!parsed) {
        pa_log("[%s:%u] Failed to parse element <" ELEMENT_devicePort ">", data->fn, data->lineno);
        device_port_free(d);
    } else if (unknown_device) {
        pa_log_info("[%s:%u] Ignore <" ELEMENT_devicePort "> with unknown device.", data->fn, data->lineno);
        device_port_free(d);
    } else {
        SLLIST_APPEND(struct device_port, data->current_module->device_ports, d);
        data->current_device_port = d;
    }

    return parsed;
}

static bool parse_route(struct parser_data *data, const char *element_name, const XML_Char **attributes) {
    struct route *r;
    bool parsed = false;
    char *sources = NULL;
    const char *state = NULL;
    char *source;

    r = pa_xmalloc0(sizeof(*r));

    if (!get_element_attrs(data, attributes,
                           ATTRIBUTE_type, &r->type,
                           ATTRIBUTE_sink, &r->sink,
                           ATTRIBUTE_sources, &sources,
                           NULL))
        goto done;

    while ((source = pa_split(sources, ",", &state))) {
        struct device *d = pa_xmalloc0(sizeof(*d));
        d->name = source;
        SLLIST_APPEND(struct device, r->sources, d);
    }

    parsed = true;
done:
    pa_xfree(sources);

    if (parsed) {
        SLLIST_APPEND(struct route, data->current_module->routes, r);
    } else {
        pa_log("[%s:%u] Failed to parse element <" ELEMENT_route ">", data->fn, data->lineno);
        route_free(r);
    }

    return parsed;
}

static bool parse_file(struct parser_data *data, const char *filename) {
    char buf[BUFSIZ];
    FILE *f = NULL;
    XML_Parser parser = NULL;
    bool done;
    bool ret = true;

    pa_assert(data);
    pa_assert(filename);

    f = fopen(filename, "r");

    if (!f) {
        pa_log_info("Failed to open file (%s): %s", filename, pa_cstrerror(errno));
        ret = false;
        goto done;
    }

    parser = XML_ParserCreate(NULL);
    data->parser = parser;
    data->fn = filename;
    if (!data->conf)
        data->conf = pa_xnew0(struct audio_policy_configuration, 1);

    XML_SetUserData(parser, data);
    XML_SetElementHandler(parser, xml_start_element, xml_end_element);
    XML_SetCharacterDataHandler(parser, xml_character_data_handler);

    pa_log_debug("Read %s ...", data->fn);

    do {
        size_t len = fread(buf, 1, sizeof(buf), f);
        done = len < sizeof(buf);
        if (XML_Parse(parser, buf, (int) len, done) == XML_STATUS_ERROR) {
            unsigned long lineno = XML_GetCurrentLineNumber(parser);
            pa_log("%" XML_FMT_STR " at line %lu\n",
                   XML_ErrorString(XML_GetErrorCode(parser)),
                   lineno);
            ret = false;
            goto done;
        }
    } while (!done);

done:
    if (parser)
        XML_ParserFree(parser);

    if (f)
        fclose(f);

    return ret;
}

static struct device_port *find_device_port(struct module *module, const char *name) {
    struct device_port *port;

    SLLIST_FOREACH(port, module->device_ports) {
        if (pa_streq(port->tag_name, name))
            return port;
    }

    return NULL;
}

static bool device_in_list(struct device *list, const char *name) {
    struct device *dev;

    pa_assert(name);

    SLLIST_FOREACH(dev, list) {
        if (pa_streq(name, dev->name))
            return true;
    }

    return false;
}

static void add_output(struct module *module, struct mix_port *mix_port, pa_droid_config_hw_module *hw_module) {
    pa_droid_config_device *output;
    struct profile *profile;
    struct route *route;
    struct device_port *device_port;

    output = pa_droid_config_device_new(hw_module, PA_DIRECTION_OUTPUT, mix_port->name);
    output->flags = mix_port->flags;
    SLLIST_FOREACH(profile, mix_port->profiles) {
        memcpy(output->sampling_rates, profile->sampling_rates, sizeof(output->sampling_rates));
        output->channel_masks |= profile->channel_masks;
        output->formats |= profile->format;
    }

    SLLIST_FOREACH(route, module->routes) {
        struct device *source;
        SLLIST_FOREACH(source, route->sources) {
            if (pa_streq(source->name, mix_port->name)) {
                if ((device_port = find_device_port(module, route->sink))) {
                    output->devices |= device_port->type;
                    if (device_in_list(module->attached_devices, device_port->tag_name))
                        hw_module->global_config->attached_output_devices |= device_port->type;
                    if (device_in_list(module->default_output, device_port->tag_name))
                        hw_module->global_config->default_output_device |= device_port->type;
                    break;
                } else
                    pa_log_info("Couldn't find matching <" ELEMENT_devicePort " tagName=%s> for <" ELEMENT_mixPort " name=%s>",
                                route->sink, source->name);
            }
        }
    }

    pa_log_debug("config: %s: New output: %s", hw_module->name, output->name);
    SLLIST_APPEND(pa_droid_config_device, hw_module->outputs, output);
}

static void add_input(struct module *module, struct mix_port *mix_port, pa_droid_config_hw_module *hw_module) {
    pa_droid_config_device *input;
    struct profile *profile;
    struct route *route;
    struct device_port *device_port;

    input = pa_droid_config_device_new(hw_module, PA_DIRECTION_INPUT, mix_port->name);
    input->flags = mix_port->flags;
    SLLIST_FOREACH(profile, mix_port->profiles) {
        memcpy(input->sampling_rates, profile->sampling_rates, sizeof(input->sampling_rates));
        input->channel_masks |= profile->channel_masks;
        input->formats |= profile->format;
    }

    SLLIST_FOREACH(route, module->routes) {
        if (pa_streq(route->sink, mix_port->name)) {
            struct device *source;
            SLLIST_FOREACH(source, route->sources) {
                if ((device_port = find_device_port(module, source->name))) {
                    input->devices |= device_port->type;
                    if (device_in_list(module->attached_devices, device_port->tag_name))
                        hw_module->global_config->attached_input_devices |= device_port->type;
                } else
                    pa_log_info("Couldn't find matching <" ELEMENT_mixPort " name=%s> for <" ELEMENT_devicePort " tagName=%s>",
                                source->name, route->sink);

            }
        }
    }

    pa_log_debug("config: %s: New input: %s", hw_module->name, input->name);
    SLLIST_APPEND(pa_droid_config_device, hw_module->inputs, input);
}

static void generate_config_for_module(struct module *module, pa_droid_config_audio *config) {
    pa_droid_config_hw_module *hw_module;
    struct mix_port *mix_port;

    pa_assert(module);
    pa_assert(config);
    pa_assert(config->global_config);

    hw_module = pa_droid_config_hw_module_new(config, module->name);
    if (module->attached_devices || module->default_output)
        hw_module->global_config = pa_xnew0(pa_droid_config_global, 1);
    SLLIST_APPEND(pa_droid_config_hw_module, config->hw_modules, hw_module);

    SLLIST_FOREACH(mix_port, module->mix_ports) {
        if (pa_streq(mix_port->role, PORT_TYPE_source))
            add_output(module, mix_port, hw_module);
        else if (pa_streq(mix_port->role, ATTRIBUTE_sink))
            add_input(module, mix_port, hw_module);
        else
            pa_log_warn("Unknown <" ELEMENT_mixPort "> role \"%s\"", mix_port->role);
    }
}

static pa_droid_config_audio *convert_config(struct audio_policy_configuration *source) {
    pa_droid_config_audio *config = NULL;
    struct module *module;

    pa_assert(source);

    config = pa_xnew0(pa_droid_config_audio, 1);
    config->global_config = pa_xnew0(pa_droid_config_global, 1);

    pa_log_debug("Convert configuration ...");
    SLLIST_FOREACH(module, source->modules)
        generate_config_for_module(module, config);

    return config;
}

pa_droid_config_audio *pa_parse_droid_audio_config_xml(const char *filename) {
    pa_droid_config_audio *config = NULL;
    struct parser_data data;
    bool ret = true;

    pa_assert(filename);

    memset(&data, 0, sizeof(data));

    data.root = &element_parse_root;
    data.current = data.root;

    if (!(ret = parse_file(&data, filename)))
        goto done;

    config = convert_config(data.conf);

done:
    if (data.conf)
        audio_policy_configuration_free(data.conf);

    return config;
}

#endif
