#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

/**
 * @brief Frontend that reads FlexProf (USIMM) trace format directly.
 * 
 * FlexProf trace format:
 *   <non_mem_ops> R <hex_addr> <pc> <domain_id>   (Read)
 *   <non_mem_ops> W <hex_addr> <domain_id>        (Write)
 * 
 * This frontend preserves the domain_id from the trace, allowing
 * proper per-domain queue assignment in the FlexProf controller.
 */
class FlexProfTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, FlexProfTrace, "FlexProfTrace", 
    "FlexProf (USIMM) memory trace with domain IDs.")

private:
  struct TraceEntry {
    int bubble_count;
    bool is_write;
    Addr_t addr;
    int domain_id;
  };

  std::vector<TraceEntry> m_trace;
  size_t m_trace_length = 0;
  size_t m_curr_trace_idx = 0;
  int m_remaining_bubbles = 0;
  
  size_t m_num_requests_sent = 0;
  size_t m_max_requests = 0;

  Logger_t m_logger;

public:
  void init() override {
    std::string trace_path_str = param<std::string>("path").desc("Path to FlexProf trace file").required();
    m_clock_ratio = param<uint>("clock_ratio").required();
    m_max_requests = param<size_t>("max_requests").desc("Maximum requests to send (0 = unlimited)").default_val(0);

    m_logger = Logging::create_logger("FlexProfTrace");
    m_logger->info("Loading FlexProf trace file {} ...", trace_path_str);
    init_trace(trace_path_str);
    m_logger->info("Loaded {} trace entries.", m_trace.size());

    // Initialize first entry
    if (m_trace_length > 0) {
      m_remaining_bubbles = m_trace[0].bubble_count;
    }
  }

  void tick() override {
    if (m_max_requests > 0 && m_num_requests_sent >= m_max_requests) {
      return;
    }

    // Process bubble cycles (non-memory operations)
    if (m_remaining_bubbles > 0) {
      m_remaining_bubbles--;
      return;
    }

    // Try to send the current memory request
    const TraceEntry& entry = m_trace[m_curr_trace_idx];
    
    Request req(
      entry.addr,
      entry.is_write ? Request::Type::Write : Request::Type::Read,
      entry.domain_id,  // Use domain_id from trace as source_id
      nullptr
    );

    bool sent = m_memory_system->send(req);
    
    if (sent) {
      m_num_requests_sent++;
      
      // Move to next trace entry
      m_curr_trace_idx = (m_curr_trace_idx + 1) % m_trace_length;
      m_remaining_bubbles = m_trace[m_curr_trace_idx].bubble_count;
    }
  }

  bool is_finished() override {
    if (m_max_requests > 0) {
      return m_num_requests_sent >= m_max_requests;
    }
    return m_num_requests_sent >= m_trace_length;
  }

private:
  void init_trace(const std::string& file_path_str) {
    fs::path trace_path(file_path_str);
    if (!fs::exists(trace_path)) {
      throw ConfigurationError("Trace {} does not exist!", file_path_str);
    }

    std::ifstream trace_file(trace_path);
    if (!trace_file.is_open()) {
      throw ConfigurationError("Trace {} cannot be opened!", file_path_str);
    }

    std::string line;
    while (std::getline(trace_file, line)) {
      std::istringstream iss(line);
      TraceEntry entry;
      
      std::string op_type;
      std::string addr_str;
      
      // Parse: <bubbles> <R/W> <addr> [<pc>] <domain_id>
      if (!(iss >> entry.bubble_count >> op_type >> addr_str)) {
        continue;  // Skip malformed lines
      }

      // Parse address (hex or decimal)
      if (addr_str.substr(0, 2) == "0x" || addr_str.substr(0, 2) == "0X") {
        entry.addr = std::stoll(addr_str.substr(2), nullptr, 16);
      } else {
        entry.addr = std::stoll(addr_str);
      }

      entry.is_write = (op_type == "W");

      // For reads: skip PC, then read domain_id
      // For writes: read domain_id directly
      if (entry.is_write) {
        if (!(iss >> entry.domain_id)) {
          entry.domain_id = 0;  // Default domain
        }
      } else {
        std::string pc_str;
        if (iss >> pc_str >> entry.domain_id) {
          // Successfully read PC and domain_id
        } else {
          entry.domain_id = 0;  // Default domain
        }
      }

      m_trace.push_back(entry);
    }

    trace_file.close();
    m_trace_length = m_trace.size();

    if (m_trace_length == 0) {
      throw ConfigurationError("Trace {} is empty!", file_path_str);
    }
  }
};

}  // namespace Ramulator
