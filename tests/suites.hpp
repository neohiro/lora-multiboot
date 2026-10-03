// SPDX-License-Identifier: MIT

#pragma once

#include <string>

// A partition table as it exists on disk, so the suites validate the artefacts
// this repo actually ships rather than a copy that can quietly drift away from
// them. main.cpp reads the files and hands them over.
struct PartitionTableUnderTest {
  std::string path;  // for failure messages
  std::string csv;
  bool loaded = false;
};

void suite_protocol_id();
void suite_channel_plan();
void suite_slot_table();
void suite_provisioning();
void suite_slot_lifecycle();
void suite_airtime();
void suite_radio_plan();
void suite_statistics();
void suite_status_panel();
void suite_roles();
void suite_system_update();
void suite_partition_csv(const PartitionTableUnderTest* tables, int count);
