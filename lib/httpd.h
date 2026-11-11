/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#ifndef HTTPD_H
#define HTTPD_H

#include "logger.h"
#include "http_request.h"
#include "http_response.h"

typedef struct httpd_s httpd_t;

typedef enum connectype_type_e {
    CONNECTION_TYPE_UNKNOWN,
    CONNECTION_TYPE_RAOP
} connection_type_t;

struct httpd_callbacks_s {
    void* opaque;
    void* (*conn_init)(void *opaque, unsigned char *local, int locallen, unsigned char *remote,
                       int remotelen, unsigned int zone_id);
    void  (*conn_request)(void *ptr, http_request_t *request, http_response_t **response);
    void  (*conn_destroy)(void *ptr);
};
typedef struct httpd_callbacks_s httpd_callbacks_t;
bool httpd_nohold(httpd_t *httpd);
void httpd_remove_known_connections(httpd_t *httpd);
void httpd_remove_connections_by_type(httpd_t *httpd, connection_type_t type);

int httpd_set_connection_type (httpd_t *http, void *user_data, connection_type_t type);
int httpd_count_connection_type (httpd_t *http, connection_type_t type);
int httpd_get_connection_socket (httpd_t *httpd, void *user_data);
int httpd_get_connection_socket_by_type (httpd_t *httpd, connection_type_t type, int instance);
const char *httpd_get_connection_typename (connection_type_t type);
void *httpd_get_connection_by_type (httpd_t *httpd, connection_type_t type, int instance);
/* seconds since data was last received on the connection of user_data, -1 if it has none */
int httpd_get_connection_idle_time (httpd_t *httpd, void *user_data);
httpd_t *httpd_init(logger_t *logger, httpd_callbacks_t *callbacks, int  nohold);

/* 0 when the server thread is not serving: not started, stopped, or ended by
 * itself after an error (then httpd_stop() followed by httpd_start() restarts it) */
int httpd_is_running(httpd_t *httpd);

/* 1: started; 0: already running; < 0: failed (the socket error is logged) */
int httpd_start(httpd_t *httpd, unsigned short *port);
void httpd_stop(httpd_t *httpd);

void httpd_destroy(httpd_t *httpd);


#endif
