//: ----------------------------------------------------------------------------
//: Copyright (C) 2018 Verizon.  All Rights Reserved.
//: All Rights Reserved
//:
//: \file:    wb_nconn_pool.cc
//: \details: TODO
//: \author:  Reed P. Morrison
//: \date:    02/16/2016
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
#include "catch/catch.hpp"
#include "is2/nconn/scheme.h"
#include "is2/nconn/nconn.h"
#include "srvr/nconn_pool.h"
//: ----------------------------------------------------------------------------
//: Tests
//: ----------------------------------------------------------------------------
TEST_CASE( "nconn pool test", "[nconn_pool]" )
{
        SECTION("Basic Connection Pool Test")
        {
                INFO("Init");
                ns_is2::nconn_pool l_p(8,16);
                REQUIRE((l_p.get_active_available() == 8));
                REQUIRE((l_p.get_active_size() == 0));
                REQUIRE((l_p.get_idle_size() == 0));
                INFO("Get 4 connections");
                ns_is2::nconn *l_c1 = NULL;
                l_c1 = l_p.get_new_active("MONKEY", ns_is2::SCHEME_TCP);
                ns_is2::nconn *l_c2 = NULL;
                l_c2 = l_p.get_new_active("BANANA", ns_is2::SCHEME_TCP);
                ns_is2::nconn *l_c3 = NULL;
                l_c3 = l_p.get_new_active("GORILLA", ns_is2::SCHEME_TCP);
                ns_is2::nconn *l_c4 = NULL;
                l_c4 = l_p.get_new_active("KOALA", ns_is2::SCHEME_TCP);
                REQUIRE((l_c1 != NULL));
                REQUIRE((l_c2 != NULL));
                REQUIRE((l_c3 != NULL));
                REQUIRE((l_c4 != NULL));
                REQUIRE((l_p.get_active_available() == 4));
                REQUIRE((l_p.get_active_size() == 4));
                REQUIRE((l_p.get_idle_size() == 0));
                INFO("Release 2");
                int32_t l_s;
                l_s = l_p.release(l_c1);
                REQUIRE((l_s == 0));
                l_s = l_p.release(l_c2);
                REQUIRE((l_s == 0));
                REQUIRE((l_p.get_active_available() == 6));
                REQUIRE((l_p.get_active_size() == 2));
                REQUIRE((l_p.get_idle_size() == 0));
                INFO("Add idle");
                l_s = l_p.add_idle(l_c3);
                REQUIRE((l_s == 0));
                REQUIRE((l_p.get_active_available() == 7));
                REQUIRE((l_p.get_active_size() == 1));
                REQUIRE((l_p.get_idle_size() == 1));
                INFO("Get idle");
                ns_is2::nconn *l_ci = NULL;
                l_ci = l_p.get_idle("KOALA");
                REQUIRE((l_ci == NULL));
                l_ci = l_p.get_idle("GORILLA");
                REQUIRE((l_ci != NULL));
                REQUIRE((l_ci->get_label() == "GORILLA"));
                REQUIRE((l_p.get_active_available() == 6));
                REQUIRE((l_p.get_active_size() == 2));
                REQUIRE((l_p.get_idle_size() == 0));
                INFO("Get all free")
                for(uint32_t i = 0; i < 6; ++i)
                {
                        ns_is2::nconn *l_ct = NULL;
                        l_ct = l_p.get_new_active("BLOOP", ns_is2::SCHEME_TCP);
                        REQUIRE((l_ct != NULL));
                }
                REQUIRE((l_p.get_active_available() == 0));
                REQUIRE((l_p.get_active_size() == 8));
                REQUIRE((l_p.get_idle_size() == 0));
                ns_is2::nconn *l_ct = NULL;
                l_ct = l_p.get_new_active("BLOOP", ns_is2::SCHEME_TCP);
                REQUIRE((l_ct == NULL));
                l_ct = l_p.get_new_active("BLOOP", ns_is2::SCHEME_TCP);
                REQUIRE((l_ct == NULL));
        }
}
