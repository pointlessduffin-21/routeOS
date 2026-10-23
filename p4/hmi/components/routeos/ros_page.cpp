// The setup page: what you open in a browser to join this board to a network and prove the link works.
//
// The page is served from flash (see ros_page_assets.h, generated from web/). The one thing it cannot do itself is the
// link test, because a browser cannot tell you whether the *board* can reach the internet — so POST /api/test runs the
// request here, on the device, and reports the timing it measured.
#include "routeos.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <string>

#include "ros_page_assets.h"

namespace routeos {
namespace {

constexpr char kTag[] = "ros-page";

esp_err_t sendBlob(httpd_req_t* req, const char* type, const char* body, unsigned length, bool cache) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", cache ? "public, max-age=600" : "no-store");
    return httpd_resp_send(req, body, length);
}

esp_err_t page(httpd_req_t* req) { return sendBlob(req, "text/html; charset=utf-8", PAGE_HTML, PAGE_HTML_LEN, false); }
esp_err_t paperCss(httpd_req_t* req) { return sendBlob(req, "text/css; charset=utf-8", PAGE_CSS, PAGE_CSS_LEN, true); }
esp_err_t toolCss(httpd_req_t* req) { return sendBlob(req, "text/css; charset=utf-8", PAGE_TOOL_CSS, PAGE_TOOL_CSS_LEN, true); }
esp_err_t paperJs(httpd_req_t* req) { return sendBlob(req, "text/javascript; charset=utf-8", PAGE_JS, PAGE_JS_LEN, true); }
esp_err_t setupJs(httpd_req_t* req) { return sendBlob(req, "text/javascript; charset=utf-8", PAGE_SETUP_JS, PAGE_SETUP_JS_LEN, false); }

std::string quote(const char* text) {
    std::string out = "\"";
    for (const char* c = text; c && *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if ((unsigned char)*c >= 0x20) out += *c;
    }
    out += '"';
    return out;
}

// One HTTPS GET from the device itself, timed end to end. This is the only honest answer to "can this board use the
// network", because DNS, the radio, the route and TLS all have to work for it to come back.
esp_err_t linkTest(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;

    const char* url = "https://yeems214.xyz/";
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // verified, not "insecure"
    cfg.method = HTTP_METHOD_GET;
    cfg.disable_auto_redirect = false;

    const int64_t started = esp_timer_get_time();
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return fail(req, 500, "Could not create the HTTP client");

    const esp_err_t opened = esp_http_client_open(client, 0);
    int status = 0;
    std::string error;
    if (opened == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) error = "no response headers (TLS or DNS failed)";
        status = esp_http_client_get_status_code(client);
    } else {
        error = esp_err_to_name(opened);
    }
    const int ms = int((esp_timer_get_time() - started) / 1000);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    const bool ok = opened == ESP_OK && error.empty() && status >= 200 && status < 400;
    log(ok ? 'I' : 'W', "link test: HTTP %d in %d ms (%s)", status, ms, ok ? "ok" : error.c_str());

    std::string body = "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"status\":";
    body += std::to_string(status);
    body += ",\"ms\":";
    body += std::to_string(ms);
    body += ",\"url\":\"";
    body += url;
    body += "\",\"tls\":\"verified against the certificate bundle\"";
    if (!ok) {
        body += ",\"error\":";
        body += quote(error.empty() ? "the request did not complete" : error.c_str());
    }
    body += "}";
    return reply(req, 200, body);
}

}  // namespace

void pageAttach(httpd_handle_t server) {
    static const httpd_uri_t root = {"/", HTTP_GET, page, nullptr};
    static const httpd_uri_t css1 = {"/paper.css", HTTP_GET, paperCss, nullptr};
    static const httpd_uri_t css2 = {"/tool.css", HTTP_GET, toolCss, nullptr};
    static const httpd_uri_t js1 = {"/paper.js", HTTP_GET, paperJs, nullptr};
    static const httpd_uri_t js2 = {"/setup.js", HTTP_GET, setupJs, nullptr};
    static const httpd_uri_t test = {"/api/test", HTTP_POST, linkTest, nullptr};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &css1);
    httpd_register_uri_handler(server, &css2);
    httpd_register_uri_handler(server, &js1);
    httpd_register_uri_handler(server, &js2);
    httpd_register_uri_handler(server, &test);
    log('I', "setup page on http://<board>/ (%u KB of assets in flash)", unsigned((PAGE_HTML_LEN + PAGE_CSS_LEN) / 1024));
}

}  // namespace routeos
