// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef HTTP_AUTH_APP_HPP_
#define HTTP_AUTH_APP_HPP_

#include "clustering/administration/metadata.hpp"
#include "http/http.hpp"
#include "rpc/semilattice/view.hpp"

// auth_http_app_t is an HTTP middleware that enforces HTTP Basic authentication
// on every request by verifying credentials against the existing auth user store.
class auth_http_app_t : public http_app_t {
public:
    auth_http_app_t(
        http_app_t *inner,
        clone_ptr_t<watchable_t<auth_semilattice_metadata_t>> auth_watchable);

    void handle(const http_req_t &request,
                http_res_t *result,
                signal_t *interruptor) override;

private:
    http_app_t *m_inner;
    clone_ptr_t<watchable_t<auth_semilattice_metadata_t>> m_auth_watchable;
};

#endif  // HTTP_AUTH_APP_HPP_
