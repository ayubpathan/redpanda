// Copyright 2022 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "config/validators.h"

#include <seastar/testing/thread_test_case.hh>

SEASTAR_THREAD_TEST_CASE(test_client_groups_byte_rate_quota_invalid_config) {
    std::unordered_map<ss::sstring, config::client_group_quota> repeated_group{
      {"group1", {"group1", "group1", 1}},
      {"group2", {"group2", "group1", 1}},
      {"group3", {"group3", "group3", 1}}};
    auto result = config::validate_client_groups_byte_rate_quota(
      repeated_group);
    BOOST_TEST(result.has_value());
    BOOST_TEST(
      result->find("Group client prefix can not be prefix for another group")
      != ss::sstring::npos);

    std::unordered_map<ss::sstring, config::client_group_quota> prefix_group{
      {"group1", {"group1", "group1", 1}},
      {"special_group", {"special_group", "special_group", 1}},
      {"group", {"group", "group", 1}}};
    result = config::validate_client_groups_byte_rate_quota(prefix_group);
    BOOST_TEST(result.has_value());
    BOOST_TEST(
      result->find("Group client prefix can not be prefix for another group")
      != ss::sstring::npos);

    std::unordered_map<ss::sstring, config::client_group_quota> prefix_group_2{
      {"g", {"g", "g", 1}},
      {"group1", {"group1", "group1", 1}},
      {"group2", {"group2", "group2", 1}}};
    result = config::validate_client_groups_byte_rate_quota(prefix_group_2);
    BOOST_TEST(result.has_value());
    BOOST_TEST(
      result->find("Group client prefix can not be prefix for another group")
      != ss::sstring::npos);

    std::unordered_map<ss::sstring, config::client_group_quota> zero_rate{
      {"group1", {"group1", "group1", 1}}, {"group2", {"group2", "group2", 0}}};
    result = config::validate_client_groups_byte_rate_quota(zero_rate);
    BOOST_TEST(result.has_value());
    BOOST_TEST(
      result->find("Quota must be a non zero positive number")
      != ss::sstring::npos);

    std::unordered_map<ss::sstring, config::client_group_quota> negative_rate{
      {"group1", {"group1", "group1", 1}},
      {"group2", {"group2", "group2", -10}}};
    result = config::validate_client_groups_byte_rate_quota(negative_rate);
    BOOST_TEST(result.has_value());
    BOOST_TEST(
      result->find("Quota must be a non zero positive number")
      != ss::sstring::npos);

    std::unordered_map<ss::sstring, config::client_group_quota> valid_config{
      {"group1", {"group1", "group1", 9223372036854775807}},
      {"group2", {"group2", "group2", 1073741824}},
      {"another_group", {"another_group", "another_group", 1}}};
    result = config::validate_client_groups_byte_rate_quota(valid_config);
    BOOST_TEST(!result.has_value());
}
