#include <vector>
#include <fstream>
#include <sstream>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/scheduler.h"

namespace Ramulator {

class FlexProfScheduler : public IScheduler, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IScheduler, FlexProfScheduler, "FlexProf", "FlexProf Pattern-based Scheduler.")

private:
  IDRAM* m_dram;
  int m_bank_addr_idx = -1;

public:
  void init() override { }

  void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
    m_dram = cast_parent<IDRAMController>()->m_dram;
    m_bank_addr_idx = m_dram->m_levels("bank");
  }

  ReqBuffer::iterator compare(ReqBuffer::iterator req1, ReqBuffer::iterator req2) override {
    bool ready1 = m_dram->check_ready(req1->command, req1->addr_vec);
    bool ready2 = m_dram->check_ready(req2->command, req2->addr_vec);

    if (ready1 ^ ready2) {
      return ready1 ? req1 : req2;
    }

    // Fallback to FCFS
    return (req1->arrive <= req2->arrive) ? req1 : req2;
  }

  ReqBuffer::iterator get_best_request(ReqBuffer& buffer) override {
    if (buffer.size() == 0) {
      return buffer.end();
    }

    // Update commands for all requests
    for (auto& req : buffer) {
      req.command = m_dram->get_preq_command(req.final_command, req.addr_vec);
    }

    // Find best ready request using FRFCFS
    auto candidate = buffer.begin();
    for (auto next = std::next(buffer.begin(), 1); next != buffer.end(); next++) {
      candidate = compare(candidate, next);
    }
    
    return candidate;
  }
};

}  // namespace Ramulator
