// SPDX-License-Identifier: MIT
//
// One binary, one exit code. The same binary is what CI runs, so there is no
// second definition of "passing" to drift away from the first.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "harness.hpp"
#include "suites.hpp"

namespace {

bool readFile(const std::string& path, std::string* out) {
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered. A test binary that crashes takes its buffered output with it,
  // which is precisely when the output is most wanted.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Default the partition directory so `python tools/gate.py` works from the repo
  // root, but let CI or a caller point it elsewhere.
  const std::string partDir = argc > 1 ? std::string(argv[1]) : std::string("firmware/partitions");

  const char* names[] = {"quadboot.csv", "dualboot.csv"};
  std::vector<PartitionTableUnderTest> tables;
  for (const char* n : names) {
    PartitionTableUnderTest t;
    t.path = partDir + "/" + n;
    t.loaded = readFile(t.path, &t.csv);
    tables.push_back(t);
  }

  std::printf("lora-multiboot :: portable logic gate\n");

  suite_protocol_id();
  suite_channel_plan();
  suite_slot_table();
  suite_provisioning();
  suite_slot_lifecycle();
  suite_airtime();
  suite_radio_plan();
  suite_statistics();
  suite_status_panel();
  suite_roles();
  suite_system_update();
  suite_shared_context();
  suite_reclaim();
  suite_radio_profiles();
  suite_partition_csv(tables.data(), static_cast<int>(tables.size()));

  std::printf("\n%d checks, %d failed\n", harness::checks(), harness::failures());
  if (harness::failures() == 0) {
    std::printf("PASS\n");
    return 0;
  }
  std::printf("FAIL\n");
  return 1;
}
