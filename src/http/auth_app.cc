// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "http/auth_app.hpp"

#include <map>
#include <string>

#include "clustering/administration/auth/authentication_error.hpp"
#include "clustering/administration/auth/plaintext_authenticator.hpp"
#include "clustering/administration/auth/username.hpp"
#include "rdb_protocol/base64.hpp"

// ---------------------------------------------------------------------------
// Embedded login page (served at GET /login)
// ---------------------------------------------------------------------------

static const char LOGIN_HTML_HEAD[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>RethinkDB &#8212; Sign in</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: #f4f4f4;
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 100vh;
    }
    .card {
      background: #fff;
      border-radius: 6px;
      box-shadow: 0 2px 14px rgba(0,0,0,.13);
      padding: 2.5rem 2rem;
      width: 100%;
      max-width: 340px;
    }
    .logo {
      font-size: 1.4rem;
      font-weight: 700;
      color: #c23b22;
      margin-bottom: 1.5rem;
      letter-spacing: -.5px;
    }
    h1 { font-size: 1.1rem; font-weight: 600; color: #222; margin-bottom: 1.25rem; }
    label {
      display: block;
      font-size: .8rem;
      font-weight: 500;
      color: #555;
      margin-bottom: .3rem;
      margin-top: .75rem;
    }
    label:first-of-type { margin-top: 0; }
    input[type=text], input[type=password] {
      display: block;
      width: 100%;
      padding: .5rem .7rem;
      border: 1px solid #ccc;
      border-radius: 4px;
      font-size: .95rem;
    }
    input:focus { outline: none; border-color: #c23b22; box-shadow: 0 0 0 2px rgba(194,59,34,.15); }
    button {
      display: block;
      width: 100%;
      margin-top: 1.25rem;
      padding: .6rem;
      background: #c23b22;
      border: none;
      border-radius: 4px;
      color: #fff;
      font-size: .95rem;
      font-weight: 600;
      cursor: pointer;
    }
    button:hover { background: #a83020; }
    .err {
      background: #fdf0ee;
      border: 1px solid #f5b5ab;
      border-radius: 4px;
      color: #a83020;
      font-size: .85rem;
      padding: .5rem .75rem;
      margin-bottom: 1rem;
    }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">RethinkDB</div>
    <h1>Sign in</h1>
)html";

static const char LOGIN_HTML_ERROR[] =
    "    <div class=\"err\">Invalid username or password.</div>\n";

static const char LOGIN_HTML_TAIL[] = R"html(    <form method="post" action="/login">
      <label for="u">Username</label>
      <input id="u" name="username" type="text" value="admin" autocomplete="username" autofocus>
      <label for="p">Password</label>
      <input id="p" name="password" type="password" autocomplete="current-password">
      <button type="submit">Sign in</button>
    </form>
  </div>
</body>
</html>
)html";

static std::string build_login_page(bool show_error) {
    std::string page;
    page.reserve(sizeof(LOGIN_HTML_HEAD) + sizeof(LOGIN_HTML_ERROR)
                 + sizeof(LOGIN_HTML_TAIL));
    page += LOGIN_HTML_HEAD;
    if (show_error) {
        page += LOGIN_HTML_ERROR;
    }
    page += LOGIN_HTML_TAIL;
    return page;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Parse application/x-www-form-urlencoded body (key=value&key2=value2).
static std::map<std::string, std::string> parse_form(const std::string &body) {
    std::map<std::string, std::string> result;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find('&', pos);
        if (next == std::string::npos) {
            next = body.size();
        }
        size_t eq = body.find('=', pos);
        if (eq != std::string::npos && eq < next) {
            std::string key = body.substr(pos, eq - pos);
            std::string raw = body.substr(eq + 1, next - eq - 1);
            // Replace '+' with space, then percent-decode.
            for (char &c : raw) {
                if (c == '+') c = ' ';
            }
            std::string decoded;
            if (!percent_unescape_string(raw, &decoded)) {
                decoded = raw;
            }
            result[key] = decoded;
        }
        pos = next + 1;
    }
    return result;
}

// Find a named cookie in the Cookie request header.
static optional<std::string> get_cookie(const http_req_t &req,
                                        const std::string &name) {
    optional<std::string> hdr = req.find_header_line("Cookie");
    if (!hdr) {
        return r_nullopt;
    }
    const std::string &cookies = *hdr;
    const std::string search = name + "=";
    size_t pos = 0;
    while (pos < cookies.size()) {
        // Skip leading spaces (after a semicolon).
        while (pos < cookies.size() && cookies[pos] == ' ') ++pos;
        if (cookies.compare(pos, search.size(), search) == 0) {
            size_t val_start = pos + search.size();
            size_t val_end = cookies.find(';', val_start);
            if (val_end == std::string::npos) {
                val_end = cookies.size();
            }
            return optional<std::string>(cookies.substr(val_start, val_end - val_start));
        }
        size_t semi = cookies.find(';', pos);
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return r_nullopt;
}

// Decode a Base64 credential string (base64(username:password)).
// Returns false if decoding fails.
static bool decode_credential(const std::string &encoded,
                               std::string *username_out,
                               std::string *password_out) {
    std::string decoded;
    try {
        decoded = decode_base64(encoded.data(), encoded.size());
    } catch (...) {
        return false;
    }
    size_t colon = decoded.find(':');
    *username_out = (colon == std::string::npos)
        ? decoded : decoded.substr(0, colon);
    *password_out = (colon == std::string::npos)
        ? std::string() : decoded.substr(colon + 1);
    return true;
}

// Verify credentials against the auth store. Returns true on success.
static bool verify_credentials(
        clone_ptr_t<watchable_t<auth_semilattice_metadata_t>> watchable,
        const std::string &username,
        const std::string &password) {
    try {
        auth::plaintext_authenticator_t authenticator(
            watchable, auth::username_t(username));
        authenticator.next_message(password);
        return true;
    } catch (const auth::authentication_error_t &) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// auth_http_app_t
// ---------------------------------------------------------------------------

auth_http_app_t::auth_http_app_t(
        http_app_t *inner,
        clone_ptr_t<watchable_t<auth_semilattice_metadata_t>> auth_watchable)
    : m_inner(inner), m_auth_watchable(std::move(auth_watchable)) { }

void auth_http_app_t::handle(const http_req_t &req,
                             http_res_t *result,
                             signal_t *interruptor) {
    const std::string path = req.resource.as_string();

    // ── /login: serve the login form and process form submissions ────────────
    if (path == "/login" || path == "/login/") {
        if (req.method == http_method_t::GET) {
            bool show_error = static_cast<bool>(req.find_query_param("error"));
            result->set_body("text/html", build_login_page(show_error));
            result->code = http_status_code_t::OK;
            return;
        }
        if (req.method == http_method_t::POST) {
            auto fields = parse_form(req.body);
            const std::string &username = fields["username"];
            const std::string &password = fields["password"];
            if (verify_credentials(m_auth_watchable, username, password)) {
                const std::string credential = encode_base64(
                    (username + ":" + password).data(),
                    (username + ":" + password).size());
                *result = http_res_t(http_status_code_t::SEE_OTHER);
                result->add_header_line("Location", "/");
                result->add_header_line("Set-Cookie",
                    "rethinkdb_auth=" + credential + "; HttpOnly; Path=/");
                return;
            }
            *result = http_res_t(http_status_code_t::SEE_OTHER);
            result->add_header_line("Location", "/login?error=1");
            return;
        }
        // Other methods on /login fall through to the inner app (returns 404 or 405).
    }

    // ── All other paths: require authentication ───────────────────────────────
    std::string username, password;
    bool has_credentials = false;

    // 1. Check Authorization: Basic … header (API clients / curl -u).
    optional<std::string> auth_hdr = req.find_header_line("Authorization");
    if (auth_hdr && auth_hdr->size() > 6 && auth_hdr->substr(0, 6) == "Basic ") {
        if (decode_credential(auth_hdr->substr(6), &username, &password)) {
            has_credentials = true;
        }
    }

    // 2. Check rethinkdb_auth session cookie (browser sessions).
    if (!has_credentials) {
        optional<std::string> cookie = get_cookie(req, "rethinkdb_auth");
        if (cookie) {
            if (decode_credential(*cookie, &username, &password)) {
                has_credentials = true;
            }
        }
    }

    if (!has_credentials) {
        // No credentials supplied: redirect to login form.
        *result = http_res_t(http_status_code_t::SEE_OTHER);
        result->add_header_line("Location", "/login");
        return;
    }

    if (!verify_credentials(m_auth_watchable, username, password)) {
        // Credentials present but invalid: redirect to login with error flag.
        *result = http_res_t(http_status_code_t::SEE_OTHER);
        result->add_header_line("Location", "/login?error=1");
        return;
    }

    // Authenticated: attach username and forward to the wrapped app.
    http_req_t authenticated_req = req;
    authenticated_req.authenticated_user = username;
    m_inner->handle(authenticated_req, result, interruptor);
}
