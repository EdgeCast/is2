//: ----------------------------------------------------------------------------
//: Copyright (C) 2018 Verizon.  All Rights Reserved.
//: All Rights Reserved
//:
//: \file:    t_srvr.cc
//: \details: TODO
//: \author:  Reed P. Morrison
//: \date:    10/05/2015
//:
//:   Licensed under the Apache License, Version 2.0 (the "License");
//:   you may not use this file except in compliance with the License.
//:   You may obtain a copy of the License at
//:
//:       http://www.apache.org/licenses/LICENSE-2.0
//:
//:   Unless required by applicable law or agreed to in writing, software
//:   distributed under the License is distributed on an "AS IS" BASIS,
//:   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//:   See the License for the specific language governing permissions and
//:   limitations under the License.
//:
//: ----------------------------------------------------------------------------
//: ----------------------------------------------------------------------------
//: includes
//: ----------------------------------------------------------------------------
#include "srvr/t_srvr.h"
#include "srvr/ups_session.h"
#include "http_parser/http_parser.h"
#include "nconn/nconn_tls.h"
#include "is2/support/ndebug.h"
#include "is2/support/nbq.h"
#include "is2/status.h"
#include "is2/support/trace.h"
#include "is2/srvr/resp.h"
#include "is2/srvr/lsnr.h"
#include "is2/srvr/session.h"
//: ----------------------------------------------------------------------------
//: macros
//: ----------------------------------------------------------------------------
#define _SET_NCONN_OPT(_conn, _opt, _buf, _len) \
        do { \
                int _status = 0; \
                _status = _conn.set_opt((_opt), (_buf), (_len)); \
                if (_status != nconn::NC_STATUS_OK) { \
                        TRC_ERROR("set_opt %d.  Status: %d.\n", \
                                   _opt, _status); \
                        delete &_conn;\
                        return STATUS_ERROR;\
                } \
        } while(0)

namespace ns_is2 {
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
t_srvr::t_srvr(const t_conf *a_t_conf):
        m_t_run_thread(),
        m_orphan_in_q(NULL),
        m_orphan_out_q(NULL),
        m_stat(),
        m_stat_cache(),
        m_stat_cache_mutex(),
        m_t_conf(a_t_conf),
        // *************************************************
        // -------------------------------------------------
        // subrequest support
        // -------------------------------------------------
        // *************************************************
        m_subr_list(),
        m_subr_list_size(),
        m_nconn_proxy_pool(a_t_conf->m_num_parallel,4096),
#ifdef ASYNC_DNS_SUPPORT
        m_adns_ctx(NULL),
#endif
        m_stopped(true),
        m_start_time_s(0),
        m_evr_loop(NULL),
        m_scheme(SCHEME_TCP),
        m_listening_nconn_list(),
        m_is_initd(false),
        m_srvr(NULL)
{
        pthread_mutex_init(&m_stat_cache_mutex, NULL);
        m_orphan_in_q = get_nbq(NULL);
        m_orphan_out_q = get_nbq(NULL);
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
t_srvr::~t_srvr()
{
        m_nconn_proxy_pool.evict_all_idle();
        for(listening_nconn_list_t::iterator i_conn = m_listening_nconn_list.begin();
            i_conn != m_listening_nconn_list.end();
            ++i_conn)
        {
                if(*i_conn)
                {
                        delete *i_conn;
                        *i_conn = NULL;
                }
        }
#ifdef ASYNC_DNS_SUPPORT
        if(m_t_conf &&
           m_t_conf->m_nresolver)
        {
                m_t_conf->m_nresolver->destroy_async(m_adns_ctx);
                m_adns_ctx = NULL;
        }
#endif
        if(m_evr_loop)
        {
                delete m_evr_loop;
                m_evr_loop = NULL;
        }
        if(m_orphan_in_q)
        {
                delete m_orphan_in_q;
                m_orphan_in_q = NULL;
        }
        if(m_orphan_out_q)
        {
                delete m_orphan_out_q;
                m_orphan_out_q = NULL;
        }
        pthread_mutex_destroy(&m_stat_cache_mutex);
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::init(void)
{
        if(m_is_initd)
        {
                return STATUS_OK;
        }
        // Create loop
        // TODO Need to make epoll vector resizeable...
        m_evr_loop = new evr_loop(m_t_conf->m_evr_loop_type,
                                           512);
        if(!m_evr_loop)
        {
                TRC_ERROR("m_evr_loop == NULL");
                return STATUS_ERROR;
        }
        // *************************************************
        // -------------------------------------------------
        // proxy support
        // -------------------------------------------------
        // *************************************************
#ifdef ASYNC_DNS_SUPPORT
        if(!m_adns_ctx)
        {
                nresolver *l_nresolver = m_t_conf->m_nresolver;
                if(!l_nresolver)
                {
                        TRC_ERROR("l_nresolver == NULL\n");
                        return STATUS_ERROR;
                }
                m_adns_ctx = l_nresolver->get_new_adns_ctx(m_evr_loop, adns_resolved_cb);
                if(!m_adns_ctx)
                {
                        TRC_ERROR("performing get_new_adns_ctx\n");
                        return STATUS_ERROR;
                }
        }
#endif
        m_is_initd = true;
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::add_lsnr(lsnr &a_lsnr)
{
        int32_t l_status;
        l_status = init();
        if(l_status != STATUS_OK)
        {
                TRC_ERROR("performing init.\n");
                return STATUS_ERROR;
        }
        nconn *l_nconn = NULL;
        if(a_lsnr.get_scheme() == SCHEME_TCP)
        {
                l_nconn = new nconn_tcp();
        }
        else if(a_lsnr.get_scheme() == SCHEME_TLS)
        {
                l_nconn = new nconn_tls();
        }
        else
        {
                TRC_ERROR("unsupported scheme.\n");
                return STATUS_ERROR;
        }
        l_nconn->set_ctx(this);
        l_nconn->set_num_reqs_per_conn(m_t_conf->m_num_reqs_per_conn);
        //l_nconn->set_collect_stats(m_t_conf->m_collect_stats);
        l_nconn->setup_evr_fd(lsnr::evr_fd_readable_cb,
                              NULL,
                              NULL);
        if(l_nconn->get_scheme() == SCHEME_TLS)
        {
                _SET_NCONN_OPT((*l_nconn),nconn_tls::OPT_TLS_CIPHER_STR,m_t_conf->m_tls_server_ctx_cipher_list.c_str(),m_t_conf->m_tls_server_ctx_cipher_list.length());
                _SET_NCONN_OPT((*l_nconn),nconn_tls::OPT_TLS_CTX,m_t_conf->m_tls_server_ctx,sizeof(m_t_conf->m_tls_server_ctx));
                if(!m_t_conf->m_tls_server_ctx_crt.empty())
                {
                        _SET_NCONN_OPT((*l_nconn),nconn_tls::OPT_TLS_TLS_CRT,m_t_conf->m_tls_server_ctx_crt.c_str(),m_t_conf->m_tls_server_ctx_crt.length());
                }
                if(!m_t_conf->m_tls_server_ctx_key.empty())
                {
                        _SET_NCONN_OPT((*l_nconn),nconn_tls::OPT_TLS_TLS_KEY,m_t_conf->m_tls_server_ctx_key.c_str(),m_t_conf->m_tls_server_ctx_key.length());
                }
                _SET_NCONN_OPT((*l_nconn),nconn_tls::OPT_TLS_OPTIONS,&(m_t_conf->m_tls_server_ctx_options),sizeof(m_t_conf->m_tls_server_ctx_options));
        }
        a_lsnr.set_t_srvr(this);
        l_nconn->set_data(&a_lsnr);
        l_nconn->set_evr_loop(m_evr_loop);
        l_status = l_nconn->nc_set_listening(a_lsnr.get_fd());
        if(l_status != STATUS_OK)
        {
                if(l_nconn)
                {
                        delete l_nconn;
                        l_nconn = NULL;
                }
                TRC_ERROR("Error performing nc_set_listening.\n");
                return STATUS_ERROR;
        }
        m_listening_nconn_list.push_back(l_nconn);
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::queue_event(evr_event **ao_event,
                            evr_event_cb_t a_cb,
                            void *a_data)
{
        if(!m_evr_loop)
        {
                return STATUS_ERROR;
        }
        // TODO -make 0 a define like EVR_EVENT_QUEUE_NOW
        int32_t l_s;
        l_s = m_evr_loop->add_event(0,
                                    a_cb,
                                    a_data,
                                    ao_event);
        // TODO CHECK STATUS!!!
        (void)l_s;
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int t_srvr::run(void)
{
        int32_t l_pthread_error = 0;
        l_pthread_error = pthread_create(&m_t_run_thread,
                                         NULL,
                                         t_run_static,
                                         this);
        if (l_pthread_error != 0)
        {
                // failed to create thread
                //NDBG_PRINT("Error: creating thread.  Reason: %s\n.",
                //           strerror(l_pthread_error));
                return STATUS_ERROR;
        }
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
void t_srvr::signal(void)
{
        int32_t l_status;
        l_status = m_evr_loop->signal();
        if(l_status != STATUS_OK)
        {
                // TODO ???
        }
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
void t_srvr::stop(void)
{
        m_stopped = true;
        signal();
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::run_loop(void)
{
        // -------------------------------------------------
        // run event loop
        // -------------------------------------------------
        int32_t l_s;
        //++m_stat.m_total_run;
        l_s = m_evr_loop->run();
        if(l_s != STATUS_OK)
        {
                // TODO log error
        }
        return l_s;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
void *t_srvr::t_run(void *a_nothing)
{
        // -------------------------------------------------
        // init
        // -------------------------------------------------
        int32_t l_s;
        l_s = init();
        if(l_s != STATUS_OK)
        {
                TRC_ERROR("Error performing init.\n");
                return NULL;
        }
        m_stopped = false;
        // -------------------------------------------------
        // start stats
        // -------------------------------------------------
        m_stat.clear();
        if(m_t_conf->m_stat_update_ms)
        {
                // Add timers...
                void *l_timer = NULL;
                add_timer(m_t_conf->m_stat_update_ms,
                          s_stat_update,
                          this,
                          &l_timer);
        }
        // -------------------------------------------------
        // run
        // -------------------------------------------------
        while(!m_stopped)
        {
                l_s = run_loop();
                UNUSED(l_s);
                // TODO -do less frequently???
                m_nconn_proxy_pool.reap();
        }
        //NDBG_PRINT("stopped\n");
        m_stopped = true;
        return NULL;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
nbq *t_srvr::get_nbq(nbq *a_nbq)
{
        UNUSED(a_nbq);
        nbq *l_nbq = NULL;
        l_nbq = new nbq(4*1024);
        return l_nbq;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::add_timer(uint32_t a_time_ms,
                          evr_event_cb_t a_cb,
                          void *a_data,
                          void **ao_event)
{
        if(!m_evr_loop)
        {
                return STATUS_ERROR;
        }
        evr_event_t *l_e = NULL;
        int32_t l_status;
        //++m_stat.m_total_add_timer;
        l_status = m_evr_loop->add_event(a_time_ms,
                                         a_cb,
                                         a_data,
                                         &l_e);
        if(l_status != STATUS_OK)
        {
                return STATUS_ERROR;
        }
        *ao_event = l_e;
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::cancel_event(void *a_event)
{
        if(!m_evr_loop)
        {
                return STATUS_ERROR;
        }
        if(!a_event)
        {
                return STATUS_OK;
        }
        evr_event_t *l_t = static_cast<evr_event_t *>(a_event);
        return m_evr_loop->cancel_event(l_t);
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::get_stat(t_stat_cntr_t &ao_stat)
{
        pthread_mutex_lock(&m_stat_cache_mutex);
        ao_stat = m_stat_cache;
        pthread_mutex_unlock(&m_stat_cache_mutex);
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
int32_t t_srvr::s_stat_update(void *a_data)
{
        t_srvr *l_t_srvr = static_cast<t_srvr *>(a_data);
        if(!l_t_srvr)
        {
                return STATUS_ERROR;
        }
        l_t_srvr->stat_update();
        if(!l_t_srvr->m_t_conf->m_stat_update_ms)
        {
                return STATUS_OK;
        }
        // Add timer for next...
        void *l_timer = NULL;
        l_t_srvr->add_timer(l_t_srvr->m_t_conf->m_stat_update_ms,
                            s_stat_update,
                            a_data,
                            &l_timer);
        return STATUS_OK;
}
//: ----------------------------------------------------------------------------
//: \details: TODO
//: \return:  TODO
//: \param:   TODO
//: ----------------------------------------------------------------------------
void t_srvr::stat_update(void)
{
        pthread_mutex_lock(&m_stat_cache_mutex);
        m_stat_cache = m_stat;
        pthread_mutex_unlock(&m_stat_cache_mutex);
}
} //namespace ns_is2 {
