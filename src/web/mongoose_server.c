#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <regex.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "web/mongoose_server.h"
#include "web/http_server.h"
#include "core/logger.h"
#include "core/shutdown_coordinator.h"
#include "utils/memory.h"
#include "web/mongoose_server_websocket.h"
#include "web/websocket_manager.h"
#include "web/mongoose_server_multithreading.h"

// Include Mongoose
#include "mongoose.h"
#include "web/mongoose_adapter.h"
#include "web/api_handlers.h"
#include "web/api_handlers_onvif.h"
#include "web/api_handlers_timeline.h"
#include "web/api_handlers_recordings.h"
#include "web/api_handlers_go2rtc_proxy.h"
#include "web/api_handlers_users.h"
#include "web/api_handlers_health.h"

// Forward declarations for timeline API handlers
void mg_handle_get_timeline_segments(struct mg_connection *c, struct mg_http_message *hm);
void mg_handle_timeline_manifest(struct mg_connection *c, struct mg_http_message *hm);
void mg_handle_timeline_playback(struct mg_connection *c, struct mg_http_message *hm);

// Forward declarations for HLS API handlers
void mg_handle_hls_master_playlist(struct mg_connection *c, struct mg_http_message *hm);
void mg_handle_hls_media_playlist(struct mg_connection *c, struct mg_http_message *hm);
void mg_handle_hls_segment(struct mg_connection *c, struct mg_http_message *hm);
void mg_handle_direct_hls_request(struct mg_connection *c, struct mg_http_message *hm);

// Default initial handler capacity
#define INITIAL_HANDLER_CAPACITY 32

// API handler function type
typedef void (*mg_api_handler_t)(struct mg_connection *c, struct mg_http_message *hm);

// API route entry structure
typedef struct {
    const char *method;     // HTTP method (GET, POST, etc.)
    const char *uri;        // URI pattern
    mg_api_handler_t handler; // Handler function
} mg_api_route_t;

// Forward declarations
static void mongoose_event_handler(struct mg_connection *c, int ev, void *ev_data);
static void *mongoose_server_event_loop(void *arg);
static void init_route_table(void);
static void free_route_table(void);
static int match_route(struct mg_http_message *hm);

// Include other mongoose server components
#include "web/mongoose_server_utils.h"
#include "web/mongoose_server_handlers.h"
#include "web/mongoose_server_auth.h"
#include "web/mongoose_server_static.h"
#include "web/http_router.h"

// Forward declarations for WebSocket handlers
void mg_handle_batch_delete_recordings_ws(struct mg_connection *c, struct mg_http_message *hm);
void mg_handle_websocket_message(struct mg_connection *c, struct mg_ws_message *wm);
void mg_handle_websocket_close(struct mg_connection *c);


// API routes table
static const mg_api_route_t s_api_routes[] = {
    // Auth API
    {"POST", "/api/auth/login", mg_handle_auth_login},
    {"POST", "/api/auth/logout", mg_handle_auth_logout},
    {"GET", "/api/auth/verify", mg_handle_auth_verify},
    
    // User Management API
    {"GET", "/api/auth/users", mg_handle_users_list},
    {"GET", "/api/auth/users/#", mg_handle_users_get},
    {"POST", "/api/auth/users", mg_handle_users_create},
    {"PUT", "/api/auth/users/#", mg_handle_users_update},
    {"DELETE", "/api/auth/users/#", mg_handle_users_delete},
    {"POST", "/api/auth/users/#/api-key", mg_handle_users_generate_api_key},
    
    // Streams API
    {"GET", "/api/streams", mg_handle_get_streams},
    {"POST", "/api/streams", mg_handle_post_stream},
    {"POST", "/api/streams/test", mg_handle_test_stream},
    {"GET", "/api/streams/#", mg_handle_get_stream},
    {"PUT", "/api/streams/#", mg_handle_put_stream},
    {"DELETE", "/api/streams/#", mg_handle_delete_stream},
    
    // Settings API
    {"GET", "/api/settings", mg_handle_get_settings},
    {"POST", "/api/settings", mg_handle_post_settings},
    
    // System API
    {"GET", "/api/system", mg_handle_get_system_info},
    {"GET", "/api/system/info", mg_handle_get_system_info}, // Keep for backward compatibility
    {"GET", "/api/system/logs", mg_handle_get_system_logs},
    {"POST", "/api/system/restart", mg_handle_post_system_restart},
    {"POST", "/api/system/shutdown", mg_handle_post_system_shutdown},
    {"POST", "/api/system/logs/clear", mg_handle_post_system_logs_clear},
    {"POST", "/api/system/backup", mg_handle_post_system_backup},
    {"GET", "/api/system/status", mg_handle_get_system_status},
    {"GET", "/api/health", mg_handle_get_health},
    
    // Recordings API
    {"GET", "/api/recordings", mg_handle_get_recordings},
    {"GET", "/api/recordings/play/#", mg_handle_play_recording},
    {"GET", "/api/recordings/download/#", mg_handle_download_recording},
    {"GET", "/api/recordings/files/check", mg_handle_check_recording_file},
    {"DELETE", "/api/recordings/files", mg_handle_delete_recording_file},
    {"GET", "/api/recordings/#", mg_handle_get_recording},
    {"DELETE", "/api/recordings/#", mg_handle_delete_recording},
    {"POST", "/api/recordings/batch-delete", mg_handle_batch_delete_recordings},
    {"POST", "/api/recordings/batch-delete-ws", mg_handle_batch_delete_recordings_ws},
    {"GET", "/api/ws", mg_handle_websocket_upgrade},
    
    // Streaming API - HLS
    {"GET", "/api/streaming/#/hls/index.m3u8", mg_handle_hls_master_playlist},
    {"GET", "/api/streaming/#/hls/stream.m3u8", mg_handle_hls_media_playlist},
    {"GET", "/api/streaming/#/hls/segment_#.ts", mg_handle_hls_segment},
    {"GET", "/api/streaming/#/hls/segment_#.m4s", mg_handle_hls_segment},
    {"GET", "/api/streaming/#/hls/init.mp4", mg_handle_hls_segment},
    
    // No direct HLS access handler - handled by static file handler

    // go2rtc WebRTC API
    {"POST", "/api/webrtc", mg_handle_go2rtc_webrtc_offer},
    {"POST", "/api/webrtc/ice", mg_handle_go2rtc_webrtc_ice},
    {"OPTIONS", "/api/webrtc", mg_handle_go2rtc_webrtc_options},
    {"OPTIONS", "/api/webrtc/ice", mg_handle_go2rtc_webrtc_ice_options},
    
    // Detection API
    {"GET", "/api/detection/results/#", mg_handle_get_detection_results},
    {"GET", "/api/detection/models", mg_handle_get_detection_models},
    
    // ONVIF API
    {"GET", "/api/onvif/discovery/status", mg_handle_get_onvif_discovery_status},
    {"GET", "/api/onvif/devices", mg_handle_get_discovered_onvif_devices},
    {"GET", "/api/onvif/device/profiles", mg_handle_get_onvif_device_profiles},
    {"POST", "/api/onvif/discovery/discover", mg_handle_post_discover_onvif_devices},
    {"POST", "/api/onvif/device/add", mg_handle_post_add_onvif_device_as_stream},
    {"POST", "/api/onvif/device/test", mg_handle_post_test_onvif_connection},
    
    // Timeline API
    {"GET", "/api/timeline/segments", mg_handle_get_timeline_segments},
    {"GET", "/api/timeline/manifest", mg_handle_timeline_manifest},
    {"GET", "/api/timeline/play", mg_handle_timeline_playback},
    
    // End of table marker
    {NULL, NULL, NULL}
};

/**
 * @brief Handle API request using the routes table
 * 
 * @param c Mongoose connection
 * @param hm Mongoose HTTP message
 * @param use_threading Whether to use threading for this request
 * @return true if request was handled, false otherwise
 */
static bool handle_api_request(struct mg_connection *c, struct mg_http_message *hm, bool use_threading) {
    // Extract URI for logging
    char uri_buf[MAX_PATH_LENGTH] = {0};
    size_t uri_len = hm->uri.len < sizeof(uri_buf) - 1 ? hm->uri.len : sizeof(uri_buf) - 1;
    memcpy(uri_buf, hm->uri.buf, uri_len);
    uri_buf[uri_len] = '\0';
    
    // Extract method for logging
    char method_buf[16] = {0};
    size_t method_len = hm->method.len < sizeof(method_buf) - 1 ? hm->method.len : sizeof(method_buf) - 1;
    memcpy(method_buf, hm->method.buf, method_len);
    method_buf[method_len] = '\0';
    
    log_info("API request received: %s %s", method_buf, uri_buf);
    
    // Find matching route
    int route_index = match_route(hm);
    if (route_index >= 0) {
        // Route matched
        log_info("API route matched: %s %s", method_buf, uri_buf);

        // Call handler directly
        log_info("Handling API request directly: %s %s", method_buf, uri_buf);
        s_api_routes[route_index].handler(c, hm);
        return true;
    }
    
    // No route matched
    log_warn("No API route matched for: %s %s", method_buf, uri_buf);
    return false;
}

/**
 * @brief Initialize the route table
 */
static void init_route_table(void) {
    // Nothing to initialize since we're using the static s_api_routes table
    log_info("Route table initialized using API routes table");
}

/**
 * @brief Free the route table
 */
static void free_route_table(void) {
    // Nothing to free since we're using a static table
    log_info("Route table reference cleared");
}

/**
 * @brief Match a route in the route table
 * 
 * @param hm HTTP message
 * @return int Index of matching route or -1 if no match
 */
static int match_route(struct mg_http_message *hm) {
    if (!hm) {
        return -1;
    }
    
    // Try to match each route
    for (int i = 0; s_api_routes[i].method != NULL; i++) {
        // Check method
        if (!mg_match(hm->method, mg_str(s_api_routes[i].method), NULL)) {
            continue;
        }
        
        // Check if URI matches the pattern
        if (mg_match(hm->uri, mg_str(s_api_routes[i].uri), NULL)) {
            // Route matched
            log_debug("Route matched: method=%.*s, pattern=%s, uri=%.*s", 
                     (int)hm->method.len, hm->method.buf,
                     s_api_routes[i].uri, 
                     (int)hm->uri.len, hm->uri.buf);
            return i;
        }
    }
    
    // No route matched
    log_debug("No route matched for: %.*s %.*s", 
             (int)hm->method.len, hm->method.buf,
             (int)hm->uri.len, hm->uri.buf);
    return -1;
}

/**
 * @brief Initialize HTTP server
 */
http_server_handle_t http_server_init(const http_server_config_t *config) {
    // Initialize router
    if (mongoose_server_init_router() != 0) {
        log_error("Failed to initialize router");
        return NULL;
    }
    
    // Initialize route table
    init_route_table();
    
    // Initialize health check system
    init_health_check_system();
    
    // Register WebSocket handlers
    log_info("Registering WebSocket handlers");
    websocket_register_handlers();
    
    // We now use the multithreading pattern instead of the API thread pool
    log_info("Using multithreading pattern for all requests");
    
    http_server_handle_t server = mongoose_server_init(config);
    if (!server) {
        log_error("Failed to initialize Mongoose server");
        return NULL;
    }
    
    return server;
}

/**
 * @brief Initialize HTTP server using Mongoose
 */
http_server_handle_t mongoose_server_init(const http_server_config_t *config) {
    if (!config) {
        log_error("Invalid server configuration");
        return NULL;
    }

    // Allocate server structure
    http_server_t *server = calloc(1, sizeof(http_server_t));
    if (!server) {
        log_error("Failed to allocate memory for server");
        return NULL;
    }

    // Copy configuration
    memcpy(&server->config, config, sizeof(http_server_config_t));

    // Allocate Mongoose event manager
    server->mgr = calloc(1, sizeof(struct mg_mgr));
    if (!server->mgr) {
        log_error("Failed to allocate memory for Mongoose event manager");
        free(server);
        return NULL;
    }

    // Initialize Mongoose event manager
    mg_mgr_init(server->mgr);
    
    // Initialize wakeup functionality for multithreading
    mg_wakeup_init(server->mgr);

    // Allocate handlers array
    server->handlers = calloc(INITIAL_HANDLER_CAPACITY, sizeof(*server->handlers));
    if (!server->handlers) {
        log_error("Failed to allocate memory for handlers");
        mg_mgr_free(server->mgr);
        free(server->mgr);
        free(server);
        return NULL;
    }
    
    // No mutex needed as we're not tracking statistics
    
    log_info("Using per-request threading for all requests");

    server->handler_capacity = INITIAL_HANDLER_CAPACITY;
    server->handler_count = 0;
    server->running = false;

    log_info("HTTP server initialized");
    return server;
}

// External function to set the web server socket for signal handling
extern void set_web_server_socket(int socket_fd);

/**
 * @brief Start HTTP server
 */
int http_server_start(http_server_handle_t server) {
    if (!server || !server->mgr) {
        log_error("Invalid server handle");
        return -1;
    }

    if (server->running) {
        log_warn("Server is already running");
        return 0;
    }

    // Construct listen URL
    char listen_url[128];
    if (server->config.ssl_enabled) {
        snprintf(listen_url, sizeof(listen_url), "https://0.0.0.0:%d", server->config.port);
    } else {
        snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", server->config.port);
    }

    // Start listening
    struct mg_connection *c = mg_http_listen(server->mgr, listen_url, mongoose_event_handler, server);
    if (c == NULL) {
        log_error("Failed to start server on %s", listen_url);
        return -1;
    }
    
    // Store the socket file descriptor for signal handling
    if (c->fd != NULL) {
        int socket_fd = (int)(size_t)c->fd;
        set_web_server_socket(socket_fd);
        log_debug("Stored web server socket: %d", socket_fd);
        
        // Set SO_REUSEADDR to allow immediate reuse of the port after shutdown
        int reuse = 1;
        if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            log_warn("Failed to set SO_REUSEADDR on listening socket: %s", strerror(errno));
        } else {
            log_info("Set SO_REUSEADDR on listening socket to allow immediate port reuse");
        }
    }

    // Configure SSL if enabled
    if (server->config.ssl_enabled) {
        struct mg_tls_opts opts = {
            .cert = server->config.cert_path,
            .key = server->config.key_path,
        };
        mg_tls_init(c, &opts);
    }

    server->running = true;
    log_info("HTTP server started on port %d", server->config.port);

    // Create a thread that runs the event loop
    pthread_t thread;
    if (pthread_create(&thread, NULL, (void *(*)(void *))mongoose_server_event_loop, server) != 0) {
        log_error("Failed to create server thread");
        server->running = false;
        return -1;
    }

    // Detach thread to let it run independently
    pthread_detach(thread);

    return 0;
}

/**
 * @brief Stop HTTP server
 */
void http_server_stop(http_server_handle_t server) {
    if (!server || !server->mgr) {
        return;
    }

    if (!server->running) {
        return;
    }

    server->running = false;
    log_info("Stopping HTTP server");

    // Give WebSocket connections time to send close frames
    usleep(250000); // 250ms for WebSocket connections to close

    // Store the listening socket FD before closing connections
    int listening_socket_fd = -1;
    for (struct mg_connection *c = server->mgr->conns; c != NULL; c = c->next) {
        if (c->is_listening && c->fd != NULL) {
            listening_socket_fd = (int)(size_t)c->fd;
            log_info("Found listening socket: %d", listening_socket_fd);
            break;
        }
    }

    // Now signal all remaining connections to close
    int connection_count = 0;
    struct mg_connection *next = NULL;
    
    // First pass: Mark all connections for closing and send close frames
    for (struct mg_connection *c = server->mgr->conns; c != NULL; c = c->next) {
        connection_count++;
        
        // Mark all connections for closing
        c->is_closing = 1;
        
        // Send proper close frame for WebSocket connections
        if (c->is_websocket) {
            mg_ws_send(c, "", 0, WEBSOCKET_OP_CLOSE);
            log_debug("Sent WebSocket close frame to connection");
        }
    }
    
    log_info("Marked %d remaining connections for closing", connection_count);
    
    // Give WebSocket connections a moment to send close frames
    usleep(100000); // 100ms
    
    // Second pass: Forcibly close all sockets
    for (struct mg_connection *c = server->mgr->conns; c != NULL; c = next) {
        // Save next pointer before potentially invalidating the current connection
        next = c->next;
        
        // Close the socket explicitly to ensure it's released
        if (c->fd != NULL && !c->is_listening) { // Don't close listening socket yet
            int socket_fd = (int)(size_t)c->fd;
            log_debug("Forcibly closing socket: %d", socket_fd);
            
            // Set SO_LINGER to force immediate socket closure
            struct linger so_linger;
            so_linger.l_onoff = 1;
            so_linger.l_linger = 0;
            setsockopt(socket_fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
            
            // Set socket to non-blocking mode to avoid hang on close
            int flags = fcntl(socket_fd, F_GETFL, 0);
            fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
            
            // Shutdown both directions of the socket
            shutdown(socket_fd, SHUT_RDWR);
            
            // Now close the socket
            close(socket_fd);
            c->fd = NULL;  // Mark as closed
            
            // Force Mongoose to drop this connection
            c->is_draining = 1;
            c->is_closing = 1;
            c->is_readable = 0;
            c->is_writable = 0;
        }
    }
    
    // Give a short time for the manager to process the closed connections
    usleep(250000); // 250ms

    // Explicitly poll the manager one more time to process closed connections
    mg_mgr_poll(server->mgr, 0);
    
    // Free Mongoose event manager
    mg_mgr_free(server->mgr);

    // Reset the web server socket
    set_web_server_socket(-1);
    
    // Log the final state
    log_info("All Mongoose connections closed and manager freed");

    // Now explicitly close the listening socket if we found it
    if (listening_socket_fd >= 0) {
        log_info("Explicitly closing listening socket: %d", listening_socket_fd);
        
        // Force immediate closure with SO_LINGER
        struct linger so_linger;
        so_linger.l_onoff = 1;
        so_linger.l_linger = 0;
        setsockopt(listening_socket_fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
        
        // Also set SO_REUSEADDR to allow immediate reuse of the port
        int reuse = 1;
        setsockopt(listening_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        // Close the socket
        close(listening_socket_fd);
        
        // Double-check that the port is released by trying to bind to it
        int test_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (test_socket >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(server->config.port);
            
            // Set SO_REUSEADDR on test socket
            int reuse = 1;
            setsockopt(test_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            
            if (bind(test_socket, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                log_info("Successfully verified port %d is released", server->config.port);
                close(test_socket);
            } else {
                log_warn("Port %d still in use after closing listening socket: %s", 
                        server->config.port, strerror(errno));
                close(test_socket);
            }
        }
    } else {
        log_warn("Could not find listening socket to close explicitly");
    }

    log_info("HTTP server stopped");
}

/**
 * @brief Destroy HTTP server
 */
void http_server_destroy(http_server_handle_t server) {
    if (!server) {
        return;
    }

    // Stop server if running
    if (server->running) {
        http_server_stop(server);
    }

    // IMPORTANT: Improved shutdown order to prevent memory corruption
    
    // First, mark all connections as closing to prevent new operations
    log_info("Marking all connections for closing");
    for (struct mg_connection *c = server->mgr->conns; c != NULL; c = c->next) {
        c->is_closing = 1;
    }
    
    // First shutdown WebSocket manager to prevent any new WebSocket operations
    log_info("Shutting down WebSocket manager");
    websocket_manager_shutdown();
    
    // Wait a moment for WebSocket connections to finish closing
    usleep(250000);  // 250ms - increased from 100ms for better safety
    
    // Wait longer for connections to finish closing
    usleep(250000);  // 250ms - increased from 100ms for better safety
    
    log_info("Multithreading cleanup complete");

    // No mutex to destroy

    // Free resources
    if (server->mgr) {
        free(server->mgr);
        server->mgr = NULL;  // Avoid double-free
    }

    if (server->handlers) {
        free(server->handlers);
        server->handlers = NULL;  // Avoid double-free
    }

    // Free route table
    free_route_table();
    
    // Finally free the server structure
    free(server);
    log_info("HTTP server destroyed");
}

/**
 * @brief Register request handler
 */
int http_server_register_handler(http_server_handle_t server, const char *path, 
                                const char *method, request_handler_t handler) {
    if (!server || !path || !handler) {
        log_error("Invalid parameters for register_handler");
        return -1;
    }

    // Check if we need to resize the handlers array
    if (server->handler_count >= server->handler_capacity) {
        int new_capacity = server->handler_capacity * 2;
        void *new_handlers = realloc(server->handlers, new_capacity * sizeof(*server->handlers));
        if (!new_handlers) {
            log_error("Failed to resize handlers array");
            return -1;
        }

        server->handlers = new_handlers;
        server->handler_capacity = new_capacity;
    }

    // Add handler
    strncpy(server->handlers[server->handler_count].path, path, sizeof(server->handlers[0].path) - 1);
    server->handlers[server->handler_count].path[sizeof(server->handlers[0].path) - 1] = '\0';

    if (method) {
        strncpy(server->handlers[server->handler_count].method, method, sizeof(server->handlers[0].method) - 1);
        server->handlers[server->handler_count].method[sizeof(server->handlers[0].method) - 1] = '\0';
    } else {
        server->handlers[server->handler_count].method[0] = '\0';
    }

    server->handlers[server->handler_count].handler = handler;
    server->handler_count++;

    log_debug("Registered handler for path: %s, method: %s", 
             path, method ? method : "ANY");

    return 0;
}

/**
 * @brief Get server statistics
 * 
 * Note: This function is kept for API compatibility but no longer tracks statistics
 */
int http_server_get_stats(http_server_handle_t server, int *active_connections, 
                         double *requests_per_second, uint64_t *bytes_sent, 
                         uint64_t *bytes_received) {
    if (!server) {
        return -1;
    }

    // Set all statistics to zero or default values
    if (active_connections) {
        *active_connections = 0;
    }

    if (requests_per_second) {
        *requests_per_second = 0.0;
    }

    if (bytes_sent) {
        *bytes_sent = 0;
    }

    if (bytes_received) {
        *bytes_received = 0;
    }

    return 0;
}

/**
 * @brief Mongoose event handler
 */
static void mongoose_event_handler(struct mg_connection *c, int ev, void *ev_data) {
    http_server_t *server = (http_server_t *)c->fn_data;

    if (ev == MG_EV_ACCEPT) {
        // New connection accepted
        log_debug("New connection accepted");
        
        // No statistics tracking
        
        // Set Connection: close header for all responses to prevent connection reuse
        c->data[1] = 'C';  // Mark connection to add "Connection: close" header
        
} else if (ev == MG_EV_WS_OPEN) {
    // WebSocket connection opened
    log_info("WebSocket connection opened");
    
    // Mark the connection as a WebSocket client
    c->data[0] = 'W';  // Mark as WebSocket client
    
    // Note: The WebSocket connection is now handled directly in mg_handle_websocket_upgrade
    
} else if (ev == MG_EV_WS_MSG) {
    // WebSocket message received
    struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
    
    // Log the message for debugging
    log_debug("WebSocket message received: %.*s", (int)wm->data.len, wm->data.buf);
    
    // Call the WebSocket message handler directly
    mg_handle_websocket_message(c, wm);
        
    } else if (ev == MG_EV_WAKEUP) {
        // Wakeup event from worker thread
        log_debug("Received wakeup event for connection ID %lu", c->id);
        
        // Handle the wakeup event
        mg_handle_wakeup_event(c, ev_data);
        
    } else if (ev == MG_EV_HTTP_MSG) {
        // HTTP request received
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        // No statistics tracking

        // Extract URI
        char uri[MAX_PATH_LENGTH];
        size_t uri_len = hm->uri.len < sizeof(uri) - 1 ? hm->uri.len : sizeof(uri) - 1;
        memcpy(uri, hm->uri.buf, uri_len);
        uri[uri_len] = '\0';

        // Log request details with more information
        log_info("Received request: uri=%s, method=%.*s", uri, 
                (int)hm->method.len, hm->method.buf);
        
        // Special handling for root path
        if (strcmp(uri, "/") == 0) {
            log_info("Root path detected, web_root=%s", server->config.web_root);
        }

        // Check if this is a static asset that should bypass authentication
        bool is_static_asset = false;
        if (strncmp(uri, "/js/", 4) == 0 || 
            strncmp(uri, "/css/", 5) == 0 || 
            strncmp(uri, "/img/", 5) == 0 || 
            strncmp(uri, "/fonts/", 7) == 0 ||
            strstr(uri, ".js") != NULL ||
            strstr(uri, ".css") != NULL ||
            strstr(uri, ".map") != NULL ||
            strstr(uri, ".ico") != NULL ||
            strstr(uri, ".png") != NULL ||
            strstr(uri, ".jpg") != NULL ||
            strstr(uri, ".jpeg") != NULL ||
            strstr(uri, ".gif") != NULL ||
            strstr(uri, ".svg") != NULL ||
            strstr(uri, ".woff") != NULL ||
            strstr(uri, ".woff2") != NULL ||
            strstr(uri, ".ttf") != NULL ||
            strstr(uri, ".eot") != NULL) {
            is_static_asset = true;
        }
        
        // Check if this is an HLS request
        bool is_hls_request = (strncmp(uri, "/hls/", 5) == 0);
        
        // Check if this is an auth verification request
        bool is_auth_verify = (strcmp(uri, "/api/auth/verify") == 0);
        
        // Skip authentication for static assets and HTML pages
        if (is_static_asset || strstr(uri, ".html") != NULL) {
            log_debug("Bypassing authentication for asset: %s", uri);
            // Continue processing without authentication check
        }
        // Process HLS requests with authentication
        else if (is_hls_request) {
            log_info("Processing HLS request with authentication: %s", uri);
            
            // Log all headers for debugging
            for (int i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
                if (hm->headers[i].name.len == 0) break;
                log_info("HLS request header: %.*s: %.*s", 
                        (int)hm->headers[i].name.len, hm->headers[i].name.buf,
                        (int)hm->headers[i].value.len, hm->headers[i].value.buf);
            }
            
            // Check for auth header or cookie
            struct mg_str *auth_header = mg_http_get_header(hm, "Authorization");
            const bool has_auth_header = (auth_header != NULL);
            
            // Check for auth cookie
            struct mg_str *cookie_header = mg_http_get_header(hm, "Cookie");
            bool has_auth_cookie = false;
            bool has_session_cookie = false;
            
            if (cookie_header != NULL) {
                // Parse cookie to check for auth
                char cookie_str[1024] = {0};
                if (cookie_header->len < sizeof(cookie_str) - 1) {
                    memcpy(cookie_str, cookie_header->buf, cookie_header->len);
                    cookie_str[cookie_header->len] = '\0';
                    
                    // Check if auth cookie exists
                    has_auth_cookie = (strstr(cookie_str, "auth=") != NULL);
                    has_session_cookie = (strstr(cookie_str, "session=") != NULL);
                }
            }
            
            log_info("HLS request auth status: header=%d, cookie=%d, session=%d", 
                    has_auth_header, has_auth_cookie, has_session_cookie);
            
            // If authentication is enabled and we have neither auth header nor cookie, return 401
            if (server->config.auth_enabled && !has_auth_header && !has_auth_cookie && !has_session_cookie) {
                log_info("Authentication required for HLS request but no auth provided");
                mg_printf(c, "HTTP/1.1 401 Unauthorized\r\n");
                mg_printf(c, "Content-Type: application/json\r\n");
                mg_printf(c, "Content-Length: 29\r\n");
                mg_printf(c, "\r\n");
                mg_printf(c, "{\"error\": \"Unauthorized\"}\n");
                return;
            }
        }
        // For non-static assets and non-HLS requests, check authentication if enabled
        else if (!is_hls_request && server->config.auth_enabled && mongoose_server_basic_auth_check(hm, server) != 0) {
            // Authentication failed
            log_info("Authentication failed for request: %s", uri);
            
            // For API requests, return 401 Unauthorized but don't prompt for basic auth
            if (strncmp(uri, "/api/", 5) == 0) {
                mg_printf(c, "HTTP/1.1 401 Unauthorized\r\n");
                mg_printf(c, "Content-Type: application/json\r\n");
                mg_printf(c, "Content-Length: 29\r\n");
                mg_printf(c, "\r\n");
                mg_printf(c, "{\"error\": \"Unauthorized\"}\n");
                return;
            } else {
                // Check if this is the root path or login page - both should be accessible without auth
                if (strcmp(uri, "/") == 0 || 
                    strcmp(uri, "/login.html") == 0 || 
                    strncmp(uri, "/login.html?", 12) == 0) {
                    // Root path or login page, serve it without authentication
                    log_info("Serving %s without authentication", uri);
                    // Continue processing without redirecting
                } else {
                    // For other requests, redirect to login page
                    mg_printf(c, "HTTP/1.1 302 Found\r\n");
                    mg_printf(c, "Location: /login.html\r\n");
                    mg_printf(c, "Content-Length: 0\r\n");
                    mg_printf(c, "\r\n");
                    return;
                }
            }
        }

        // Handle CORS preflight request
        if (server->config.cors_enabled && mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            log_info("Handling CORS preflight request: %s", uri);
            mongoose_server_handle_cors_preflight(c, hm, server);
            return;
        }
        
        // Check if this is a static asset, HTML file, or HLS request
        is_static_asset = is_static_asset || strstr(uri, ".html") != NULL;
        bool is_api_request = strncasecmp(uri, "/api/", 5) == 0;
        bool is_direct_hls = strncasecmp(uri, "/hls/", 5) == 0;
        bool handled = false;
        
        // Special handling for root path
        if (strcmp(uri, "/") == 0) {
            log_info("Root path detected in main handler, redirecting to index.html");
            // Directly serve index.html for root path
            char index_path[MAX_PATH_LENGTH * 2];
            snprintf(index_path, sizeof(index_path), "%s/index.html", server->config.web_root);
            
            // Check if index.html exists
            struct stat st;
            if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
                // Use Mongoose's built-in file serving capabilities
                struct mg_http_serve_opts opts = {
                    .root_dir = server->config.web_root,
                    .mime_types = "html=text/html,htm=text/html,css=text/css,js=application/javascript,"
                                "json=application/json,jpg=image/jpeg,jpeg=image/jpeg,png=image/png,"
                                "gif=image/gif,svg=image/svg+xml,ico=image/x-icon,mp4=video/mp4,"
                                "webm=video/webm,ogg=video/ogg,mp3=audio/mpeg,wav=audio/wav,"
                                "txt=text/plain,xml=application/xml,pdf=application/pdf",
                    .extra_headers = "Connection: close\r\n"
                };
                
                log_info("Serving index file for root path using mg_http_serve_file: %s", index_path);
                mg_http_serve_file(c, hm, index_path, &opts);
            } else {
                // If index.html doesn't exist, redirect to /index.html with query parameters preserved
                char redirect_url[MAX_PATH_LENGTH * 2] = "/index.html";
                
                // Extract query string if present
                if (hm->query.len > 0) {
                    strncat(redirect_url, "?", sizeof(redirect_url) - strlen(redirect_url) - 1);
                    strncat(redirect_url, hm->query.buf, 
                           MIN(hm->query.len, sizeof(redirect_url) - strlen(redirect_url) - 1));
                }
                
                log_info("Index file not found, redirecting to %s", redirect_url);
                mg_printf(c, "HTTP/1.1 302 Found\r\n");
                mg_printf(c, "Location: %s\r\n", redirect_url);
                mg_printf(c, "Content-Length: 0\r\n");
                mg_printf(c, "\r\n");
            }
            handled = true;
        }
        else if (is_api_request) {
            // For API requests, use the API request handler - always handle directly
            handled = handle_api_request(c, hm, false);
        } else if (is_direct_hls) {
            // For direct HLS requests, use the HLS handler
            log_debug("Handling direct HLS request: %s", uri);
            mg_handle_direct_hls_request(c, hm);
            handled = true;
        } else if (is_static_asset) {
            // For static assets, serve directly
            log_debug("Serving static asset directly: %s", uri);
            mongoose_server_handle_static_file(c, hm, server);
            handled = true;
        } else {
            // For other requests, handle directly
            log_debug("Handling non-API request directly: %s", uri);
            handled = mg_handle_request_with_threading(c, hm, false);
        }
        
    // If not handled by API handlers or multithreading, serve static file or return 404
    if (!handled) {
        // Extract URI for logging
        char uri_buf[MAX_PATH_LENGTH] = {0};
        size_t uri_len = hm->uri.len < sizeof(uri_buf) - 1 ? hm->uri.len : sizeof(uri_buf) - 1;
        memcpy(uri_buf, hm->uri.buf, uri_len);
        uri_buf[uri_len] = '\0';
        
        log_info("Request not handled by API or multithreading, passing to static file handler: %s", uri_buf);
        
        // Try to serve static file
        mongoose_server_handle_static_file(c, hm, server);
    }
    } else if (ev == MG_EV_CLOSE) {
        // Connection closed
        log_debug("Connection closed");
        
        // If this was a WebSocket connection, handle cleanup
        if (c->is_websocket) {
            log_info("WebSocket connection closed");
            
            // Call the WebSocket close handler directly
            mg_handle_websocket_close(c);
            
            // Find the component ID for this connection
            char conn_name[64];
            snprintf(conn_name, sizeof(conn_name), "websocket_%p", (void*)c);
            
            // Only update if shutdown coordinator is initialized
            if (get_shutdown_coordinator() != NULL) {
                // Mark the component as stopped in the shutdown coordinator
                // We don't have the component ID stored, but we can search for it by name
                shutdown_coordinator_t *coordinator = get_shutdown_coordinator();
                
                pthread_mutex_lock(&coordinator->mutex);
                for (int i = 0; i < atomic_load(&coordinator->component_count); i++) {
                    if (strcmp(coordinator->components[i].name, conn_name) == 0) {
                        // Found the component, update its state
                        atomic_store(&coordinator->components[i].state, COMPONENT_STOPPED);
                        log_debug("Updated WebSocket connection %s state to STOPPED in shutdown coordinator", 
                                 conn_name);
                        break;
                    }
                }
                pthread_mutex_unlock(&coordinator->mutex);
            }
        }
    } else if (ev == MG_EV_ERROR) {
        // Connection error
        log_error("Connection error: %s", (char *)ev_data);
    } else if (ev == MG_EV_POLL) {
        // Poll event - do nothing
    } else if (ev == MG_EV_READ || ev == MG_EV_WRITE) {
        // Read/write events - normal socket operations
        // No need to log these high-frequency events
    } else if (ev == 7) {
        // Event 7 appears to be related to WebSocket data frame start
        // Handle silently to avoid log spam
    } else if (ev == 8) {
        // Event 8 appears to be related to WebSocket data frame continuation
        // Handle silently to avoid log spam
    } else if (ev == 10) {
        // Event 10 appears to be related to connection activity
        // Handle silently to avoid log spam
    } else {
        // Other events
        log_debug("Unhandled event: %d", ev);
    }
}

/**
 * @brief Event loop for Mongoose server
 * This function runs in a separate thread and continuously calls mg_mgr_poll
 */
static void *mongoose_server_event_loop(void *arg) {
    http_server_t *server = (http_server_t *)arg;
    
    log_info("Mongoose event loop started");
    
    // Run event loop until server is stopped
    int poll_count = 0;
    while (server->running) {
        // Check if shutdown has been initiated
        if (is_shutdown_initiated()) {
            log_info("Shutdown initiated, stopping Mongoose event loop");
            server->running = false;
            break;
        }
        
        // Check if server needs restart due to health check failures
        if (check_server_restart_needed()) {
            log_info("Attempting to restart server due to health check failures");
            
            // Stop the server
            server->running = false;
            
            // Wait a moment for connections to close
            usleep(500000); // 500ms
            
            // Reset restart flag
            reset_server_restart_flag();
            
            // Reset health metrics
            reset_health_metrics();
            
            // Restart the server by recreating it
            http_server_stop(server);
            
            // Wait a moment before restarting
            usleep(1000000); // 1 second
            
            // Start the server again
            if (http_server_start(server) == 0) {
                log_info("Server successfully restarted");
                server->running = true;
            } else {
                log_error("Failed to restart server");
                break;
            }
        }
        
        // Poll for events with a shorter timeout to be more responsive
        mg_mgr_poll(server->mgr, 10);
        
        poll_count++;
        
        // Log every 1000 polls (approximately every 10 seconds with 10ms timeout)
        if (poll_count % 1000 == 0) {
            // Count active connections for debugging
            int active_count = 0;
            for (struct mg_connection *c = server->mgr->conns; c != NULL; c = c->next) {
                active_count++;
            }
            
            log_debug("Mongoose event loop poll count: %d, active connections: %d", 
                     poll_count, active_count);
        }
    }
    
    log_info("Mongoose event loop stopped");
    
    // Immediately close all connections when the event loop stops
    log_info("Forcibly closing all remaining connections");
    for (struct mg_connection *c = server->mgr->conns; c != NULL; c = c->next) {
        // Mark all connections for closing
        c->is_closing = 1;
        
        // Close the socket explicitly
        if (c->fd != NULL) {
            int socket_fd = (int)(size_t)c->fd;
            
            // Set SO_LINGER to force immediate socket closure
            struct linger so_linger;
            so_linger.l_onoff = 1;
            so_linger.l_linger = 0;
            setsockopt(socket_fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
            
            // Close the socket
            close(socket_fd);
            c->fd = NULL;  // Mark as closed
        }
    }
    
    // Poll one more time to process closed connections
    mg_mgr_poll(server->mgr, 0);
    
    return NULL;
}
