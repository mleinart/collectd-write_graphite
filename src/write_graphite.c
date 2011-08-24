/**
 * collectd - src/write_graphite.c
 * Copyright (C) 2011  Scott Sanders
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Author:
 *   Scott Sanders <scott@jssjr.com>
 *
 *   based on the excellent write_http plugin
 **/

 /* write_graphite plugin configuation example
  *
  * <Plugin write_graphite>
  *   <Carbon "local-agent">
  *     Host "localhost"
  *     Port 2003
  *     Prefix "collectd"
  *   </Carbon>
  * </Plugin>
  */

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include "utils_cache.h"
#include "utils_parse_option.h"

/* Folks without pthread will need to disable this plugin. */
#include <pthread.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netdb.h>

#ifndef WG_FORMAT_NAME
#define WG_FORMAT_NAME(ret, ret_len, vl, prefix, name) \
        wg_format_name (ret, ret_len, (vl)->host, (vl)->plugin, (vl)->plugin_instance, \
                        (vl)->type, (vl)->type_instance, prefix, name)
#endif

/*
 * Private variables
 */
struct wg_callback
{
    char    *name;

    int      sock_fd;
    struct hostent *server;

    char    *host;
    int      port;
    char    *prefix;

    char     send_buf[4096];
    size_t   send_buf_free;
    size_t   send_buf_fill;
    time_t send_buf_init_time;

    pthread_mutex_t send_lock;
};


/*
 * Functions
 */
static void wg_reset_buffer (struct wg_callback *cb) /* {{{ */
{
    memset (cb->send_buf, 0, sizeof (cb->send_buf));
    cb->send_buf_free = sizeof (cb->send_buf);
    cb->send_buf_fill = 0;
    cb->send_buf_init_time = time (NULL);
} /* }}} wg_reset_buffer */

static int wg_send_buffer (struct wg_callback *cb) /* {{{ */
{
    int status = 0;

    status = write (cb->sock_fd, cb->send_buf, strlen (cb->send_buf));
    if (status < 0)
    {
        ERROR ("write_graphite plugin: send failed with "
                "status %i (%s)",
                status,
                strerror (errno));

        pthread_mutex_lock (&cb->send_lock);

        DEBUG ("write_graphite plugin: closing socket and restting fd "
                "so reinit will occur");
        close (cb->sock_fd);
        cb->sock_fd = -1;

        pthread_mutex_unlock (&cb->send_lock);

        return (-1);
    }
    return (0);
} /* }}} wg_send_buffer */

static int wg_flush_nolock (time_t timeout, struct wg_callback *cb) /* {{{ */
{
    int status;

    DEBUG ("write_graphite plugin: wg_flush_nolock: timeout = %.3f; "
            "send_buf_fill = %zu;",
            (double)timeout,
            cb->send_buf_fill);

    /* timeout == 0  => flush unconditionally */
    if (timeout > 0)
    {
        time_t now;

        now = time (NULL);
        if ((cb->send_buf_init_time + timeout) > now)
            return (0);
    }

    if (cb->send_buf_fill <= 0)
    {
        cb->send_buf_init_time = time (NULL);
        return (0);
    }

    status = wg_send_buffer (cb);
    wg_reset_buffer (cb);

    return (status);
} /* }}} wg_flush_nolock */

static int wg_callback_init (struct wg_callback *cb) /* {{{ */
{
    int status;

    struct sockaddr_in serv_addr;

    if (cb->sock_fd > 0)
        return (0);

    cb->sock_fd = socket (AF_INET, SOCK_STREAM, 0);
    if (cb->sock_fd < 0)
    {
        ERROR ("write_graphite plugin: socket failed: %s", strerror (errno));
        return (-1);
    }
    cb->server = gethostbyname(cb->host);
    if (cb->server == NULL)
    {
        ERROR ("write_graphite plugin: no such host");
        return (-1);
    }
    memset (&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy (&serv_addr.sin_addr.s_addr,
                cb->server->h_addr,
                cb->server->h_length);
    serv_addr.sin_port = htons(cb->port);

    status = connect(cb->sock_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
    if (status < 0)
    {
        char errbuf[1024];
        sstrerror (errno, errbuf, sizeof (errbuf));
        ERROR ("write_graphite plugin: connect failed: %s", errbuf);
        close (cb->sock_fd);
        cb->sock_fd = -1;
        return (-1);
    }

    wg_reset_buffer (cb);

    return (0);
} /* }}} int wg_callback_init */

static void wg_callback_free (void *data) /* {{{ */
{
    struct wg_callback *cb;

    if (data == NULL)
        return;

    cb = data;

    wg_flush_nolock (/* timeout = */ 0, cb);

    close(cb->sock_fd);
    sfree(cb->name);
    sfree(cb->host);
    sfree(cb->prefix);

    sfree(cb);
} /* }}} void wg_callback_free */

static int wg_flush (int timeout, /* {{{ */
        const char *identifier __attribute__((unused)),
        user_data_t *user_data)
{
    struct wg_callback *cb;
    int status;

    if (user_data == NULL)
        return (-EINVAL);

    cb = user_data->data;

    pthread_mutex_lock (&cb->send_lock);

    if (cb->sock_fd < 0)
    {
        status = wg_callback_init (cb);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: wg_callback_init failed.");
            pthread_mutex_unlock (&cb->send_lock);
            return (-1);
        }
    }

    status = wg_flush_nolock (timeout, cb);
    pthread_mutex_unlock (&cb->send_lock);

    return (status);
} /* }}} int wg_flush */

static int wg_format_values (char *ret, size_t ret_len, /* {{{ */
        int ds_num, const data_set_t *ds, const value_list_t *vl,
        bool store_rates)
{
    size_t offset = 0;
    int status;
    gauge_t *rates = NULL;

    assert (0 == strcmp (ds->type, vl->type));

    memset (ret, 0, ret_len);

#define BUFFER_ADD(...) do { \
    status = ssnprintf (ret + offset, ret_len - offset, \
            __VA_ARGS__); \
    if (status < 1) \
    { \
        sfree (rates); \
        return (-1); \
    } \
    else if (((size_t) status) >= (ret_len - offset)) \
    { \
        sfree (rates); \
        return (-1); \
    } \
    else \
    offset += ((size_t) status); \
} while (0)

    if (ds->ds[ds_num].type == DS_TYPE_GAUGE)
        BUFFER_ADD ("%f", vl->values[ds_num].gauge);
    else if (store_rates)
    {
        if (rates == NULL)
            rates = uc_get_rate (ds, vl);
        if (rates == NULL)
        {
            WARNING ("format_values: "
                    "uc_get_rate failed.");
            return (-1);
        }
        BUFFER_ADD ("%g", rates[ds_num]);
    }
    else if (ds->ds[ds_num].type == DS_TYPE_COUNTER)
        BUFFER_ADD ("%llu", vl->values[ds_num].counter);
    else if (ds->ds[ds_num].type == DS_TYPE_DERIVE)
        BUFFER_ADD ("%"PRIi64, vl->values[ds_num].derive);
    else if (ds->ds[ds_num].type == DS_TYPE_ABSOLUTE)
        BUFFER_ADD ("%"PRIu64, vl->values[ds_num].absolute);
    else
    {
        ERROR ("format_values plugin: Unknown data source type: %i",
                ds->ds[ds_num].type);
        sfree (rates);
        return (-1);
    }

#undef BUFFER_ADD

    sfree (rates);
    return (0);
} /* }}} int wg_format_values */

static int normalize_hostname (char *dst, const char *src) /* {{{ */
{
    size_t i;

    int reps = 0;

    for (i = 0; i < strlen(src) ; i++)
    {
        if (src[i] == '.')
        {
            dst[i] = '_';
            ++reps;
        }
        else
            dst[i] = src[i];
    }
    dst[i] = '\0';

    return reps;
} /* }}} int normalize_hostname */

static int wg_format_name (char *ret, int ret_len, /* {{{ */
                const char *hostname,
                const char *plugin, const char *plugin_instance,
                const char *type, const char *type_instance,
                const char *prefix, const char *ds_name)
{
    int  status;
    char *n_hostname;

    assert (plugin != NULL);
    assert (type != NULL);

    if ((n_hostname = malloc(strlen(hostname)+1)) == NULL)
    {
        ERROR ("Unable to allocate memory for normalized hostname buffer");
        return (-1);
    }

    if (normalize_hostname(n_hostname, hostname) == -1)
    {
        ERROR ("Unable to normalize hostname");
        return (-1);
    }

    if ((plugin_instance == NULL) || (strlen (plugin_instance) == 0))
    {
        if ((type_instance == NULL) || (strlen (type_instance) == 0))
        {
            if ((ds_name == NULL) || (strlen (ds_name) == 0))
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s",
                        prefix, n_hostname, plugin, type);
            else
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s.%s",
                        prefix, n_hostname, plugin, type, ds_name);
        }
        else
        {
            if ((ds_name == NULL) || (strlen (ds_name) == 0))
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s-%s",
                        prefix, n_hostname, plugin, type,
                        type_instance);
            else
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s-%s.%s",
                        prefix, n_hostname, plugin, type,
                        type_instance, ds_name);
        }
    }
    else
    {
        if ((type_instance == NULL) || (strlen (type_instance) == 0))
        {
            if ((ds_name == NULL) || (strlen (ds_name) == 0))
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s.%s",
                        prefix, n_hostname, plugin,
                        plugin_instance, type);
            else
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s.%s.%s",
                        prefix, n_hostname, plugin,
                        plugin_instance, type, ds_name);
        }
        else
        {
            if ((ds_name == NULL) || (strlen (ds_name) == 0))
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s.%s-%s",
                        prefix, n_hostname, plugin,
                        plugin_instance, type, type_instance);
            else
                status = ssnprintf (ret, ret_len, "%s.%s.%s.%s.%s-%s.%s",
                        prefix, n_hostname, plugin,
                        plugin_instance, type, type_instance, ds_name);
        }
    }

    sfree(n_hostname);

    if ((status < 1) || (status >= ret_len))
        return (-1);
    return (0);
} /* }}} int wg_format_name */

static int wg_send_message (const char* key, const char* value, time_t time, struct wg_callback *cb) /* {{{ */
{
    int status;
    size_t message_len;
    char message[1024];

    message_len = (size_t) ssnprintf (message, sizeof (message),
            "%s %s %u\n",
            key,
            value,
            (unsigned int) time);
    if (message_len >= sizeof (message)) {
        ERROR ("write_graphite plugin: message buffer too small: "
                "Need %zu bytes.", message_len + 1);
        return (-1);
    }


    pthread_mutex_lock (&cb->send_lock);

    if (cb->sock_fd < 0)
    {
        status = wg_callback_init (cb);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: wg_callback_init failed.");
            pthread_mutex_unlock (&cb->send_lock);
            return (-1);
        }
    }

    if (message_len >= cb->send_buf_free)
    {
        status = wg_flush_nolock (/* timeout = */ 0, cb);
        if (status != 0)
        {
            pthread_mutex_unlock (&cb->send_lock);
            return (status);
        }
    }
    assert (message_len < cb->send_buf_free);

    /* `message_len + 1' because `message_len' does not include the
     * trailing null byte. Neither does `send_buffer_fill'. */
    memcpy (cb->send_buf + cb->send_buf_fill,
            message, message_len + 1);
    cb->send_buf_fill += message_len;
    cb->send_buf_free -= message_len;

    DEBUG ("write_graphite plugin: <%s:%d> buf %zu/%zu (%g%%) \"%s\"",
            cb->host,
            cb->port,
            cb->send_buf_fill, sizeof (cb->send_buf),
            100.0 * ((double) cb->send_buf_fill) / ((double) sizeof (cb->send_buf)),
            message);

    /* Check if we have enough space for this message. */
    pthread_mutex_unlock (&cb->send_lock);

    return (0);
} /* }}} int wg_send_message */

static int wg_write_messages (const data_set_t *ds, const value_list_t *vl, /* {{{ */
                        struct wg_callback *cb)
{
    char key[10*DATA_MAX_NAME_LEN];
    char values[512];

    int status, i;

    if (0 != strcmp (ds->type, vl->type))
    {
        ERROR ("write_graphite plugin: DS type does not match "
                "value list type");
        return -1;
    }

    if (ds->ds_num > 1)
    {
        for (i = 0; i < ds->ds_num; i++)
        {
            /* Copy the identifier to `key' and escape it. */
            status = WG_FORMAT_NAME (key, sizeof (key), vl, cb->prefix, ds->ds[i].name);
            if (status != 0)
            {
                ERROR ("write_graphite plugin: error with format_name");
                return (status);
            }

            escape_string (key, sizeof (key));
            /* Convert the values to an ASCII representation and put that into
             * `values'. */
            status = wg_format_values (values, sizeof (values), i, ds, vl, 0);
            if (status != 0)
            {
                ERROR ("write_graphite plugin: error with "
                        "wg_format_values");
                return (status);
            }

            /* Send the message to graphite */
            status = wg_send_message (key, values, vl->time, cb);
            if (status != 0)
            {
                ERROR ("write_graphite plugin: error with "
                        "wg_send_message");
                return (status);
            }
        }
    }
    else
    {
        /* Copy the identifier to `key' and escape it. */
        status = WG_FORMAT_NAME (key, sizeof (key), vl, cb->prefix, NULL);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: error with format_name");
            return (status);
        }

        escape_string (key, sizeof (key));
        /* Convert the values to an ASCII representation and put that into
         * `values'. */
        status = wg_format_values (values, sizeof (values), 0, ds, vl, 0);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: error with "
                    "wg_format_values");
            return (status);
        }

        /* Send the message to graphite */
        status = wg_send_message (key, values, vl->time, cb);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: error with "
                    "wg_send_message");
            return (status);
        }
    }

    return (0);
} /* }}} int wg_write_messages */

static int wg_write (const data_set_t *ds, const value_list_t *vl, /* {{{ */
        user_data_t *user_data)
{
    struct wg_callback *cb;
    int status;

    if (user_data == NULL)
        return (-EINVAL);

    cb = user_data->data;

    status = wg_write_messages (ds, vl, cb);

    return (status);
} /* }}} int wg_write */

static int config_set_number (int *dest, /* {{{ */
        oconfig_item_t *ci)
{
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_NUMBER))
    {
        WARNING ("write_graphite plugin: The `%s' config option "
                "needs exactly one numeric argument.", ci->key);
        return (-1);
    }

    *dest = ci->values[0].value.number;

    return (0);
} /* }}} int config_set_number */

static int config_set_string (char **ret_string, /* {{{ */
        oconfig_item_t *ci)
{
    char *string;

    if ((ci->values_num != 1)
            || (ci->values[0].type != OCONFIG_TYPE_STRING))
    {
        WARNING ("write_graphite plugin: The `%s' config option "
                "needs exactly one string argument.", ci->key);
        return (-1);
    }

    string = strdup (ci->values[0].value.string);
    if (string == NULL)
    {
        ERROR ("write_graphite plugin: strdup failed.");
        return (-1);
    }

    if (*ret_string != NULL)
        sfree (*ret_string);
    *ret_string = string;

    return (0);
} /* }}} int config_set_string */

static int wg_config_carbon (oconfig_item_t *ci) /* {{{ */
{
    struct wg_callback *cb;
    user_data_t user_data;
    int i;

    cb = malloc (sizeof (*cb));
    if (cb == NULL)
    {
        ERROR ("write_graphite plugin: malloc failed.");
        return (-1);
    }
    memset (cb, 0, sizeof (*cb));
    cb->sock_fd = -1;
    cb->host = NULL;
    cb->name = NULL;
    cb->port = 2003;
    cb->prefix = NULL;
    cb->server = NULL;

    pthread_mutex_init (&cb->send_lock, /* attr = */ NULL);

    config_set_string (&cb->name, ci);
    if (cb->name == NULL)
        return (-1);

    for (i = 0; i < ci->children_num; i++)
    {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp ("Host", child->key) == 0)
            config_set_string (&cb->host, child);
        else if (strcasecmp ("Port", child->key) == 0)
            config_set_number (&cb->port, child);
        else if (strcasecmp ("Prefix", child->key) == 0)
            config_set_string (&cb->prefix, child);
        else
        {
            ERROR ("write_graphite plugin: Invalid configuration "
                        "option: %s.", child->key);
        }
    }

    DEBUG ("write_graphite: Registering write callback to carbon agent "
            "%s:%d", cb->host, cb->port);

    memset (&user_data, 0, sizeof (user_data));
    user_data.data = cb;
    user_data.free_func = NULL;
    plugin_register_flush ("write_graphite", wg_flush, &user_data);

    user_data.free_func = wg_callback_free;
    plugin_register_write ("write_graphite", wg_write, &user_data);

    return (0);
} /* }}} int wg_config_carbon */

static int wg_config (oconfig_item_t *ci) /* {{{ */
{
    int i;

    for (i = 0; i < ci->children_num; i++)
    {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp ("Carbon", child->key) == 0)
            wg_config_carbon (child);
        else
        {
            ERROR ("write_graphite plugin: Invalid configuration "
                        "option: %s.", child->key);
        }
    }

    return (0);
} /* }}} int wg_config */

void module_register (void) /* {{{ */
{
    plugin_register_complex_config ("write_graphite", wg_config);
} /* }}} void module_register */

/* vim: set fdm=marker sw=4 ts=4 sts=4 tw=78 et : */
