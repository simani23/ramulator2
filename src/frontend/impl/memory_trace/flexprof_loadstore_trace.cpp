#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

/**
 * @brief Frontend that reads Load/Store traces with FlexProf domain isolation support.
 * 
 * This frontend extends the standard LoadStoreTrace format to support optional
 * domain_id for FlexProf per-domain queue isolation.
 * 
 * Supported trace formats:
 *   LD <addr>                    (read, domain 0)
 *   ST <addr>                    (write, domain 0)
 *   LD <addr> <domain_id>        (read, specified domain)
 *   ST <addr> <domain_id>        (write, specified domain)
 *   R <addr>                     (read, domain 0)
 *   W <addr>                     (write, domain 0)
 *   R <addr> <domain_id>         (read, specified domain)
 *   W <addr> <domain_id>         (write, specified domain)
 * 
 * Addresses can be decimal or hex (0x prefix).
 * 
 * This allows running standard ramulator2 traces through FlexProf's
 * controller-level spatial partitioning with per-domain queues.
 */
class FlexProfLoadStoreTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, FlexProfLoadStoreTrace, "FlexProfLoadStoreTrace", 
    "Load/Store trace with FlexProf domain ID support for spatial isolation.")

private:
  struct TraceEntry {
    bool is_write;
    Addr_t addr;
    int domain_id;
  };

  std::vector<TraceEntry> m_trace;
  size_t m_trace_length = 0;
  size_t m_curr_trace_idx = 0;

  size_t m_requests_sent = 0;
  size_t m_max_requests = 0;

  // Default domain when not specified in trace
  int m_default_domain = 0;
  int m_num_domains = 1;

  Logger_t m_logger;

public:
  void init() override {
    std::string trace_path_str = param<std::string>("path")
      .desc("Path to the load/store trace file.").required();
    m_clock_ratio = param<uint>("clock_ratio").required();
    m_max_requests = param<size_t>("max_requests")
      .desc("Maximum requests to send (0 = run full trace once)").default_val(0);
    m_default_domain = param<int>("default_domain")
      .desc("Default domain ID when not specified in trace").default_val(0);

    m_logger = Logging::create_logger("FlexProfLoadStoreTrace");
    m_logger->info("Loading trace file {} ...", trace_path_str);
    init_trace(trace_path_str);
    m_logger->info("Loaded {} entries across {} domains.", m_trace.size(), m_num_domains);
  }

  void tick() override {
    if (is_finished()) {
      return;
    }

    const TraceEntry& entry = m_trace[m_curr_trace_idx];
    
    Request req(
      entry.addr,
      entry.is_write ? Request::Type::Write : Request::Type::Read,
      entry.domain_id,  // Use domain_id as source_id for FlexProf controller
      nullptr
    );

    if (m_memory_system->send(req)) {
      m_requests_sent++;
      m_curr_trace_idx++;
      
      if (m_curr_trace_idx >= m_trace_length) {
        if (m_max_requests > 0 && m_requests_sent < m_max_requests) {
          // Wrap around if max_requests not yet reached
          m_curr_trace_idx = 0;
        }
      }
    }
  }

  bool is_finished() override {
    if (m_max_requests > 0) {
      return m_requests_sent >= m_max_requests;
    }
    return m_curr_trace_idx >= m_trace_length;
  }

  int get_num_cores() override {
    return m_num_domains;
  }

  SimulationProgress get_progress() override {
    SimulationProgress progress;
    progress.requests_sent = m_requests_sent;
    progress.max_requests = m_max_requests;
    progress.trace_length = m_trace_length;
    progress.has_progress_info = true;
    return progress;
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

    int max_domain_seen = 0;
    std::string line;
    while (std::getline(trace_file, line)) {
      if (line.empty() || line[0] == '#') {
        continue;  // Skip empty lines and comments
      }

      std::istringstream iss(line);
      TraceEntry entry;
      entry.domain_id = m_default_domain;

      std::string op_type;
      std::string addr_str;
      
      if (!(iss >> op_type >> addr_str)) {
        continue;  // Skip malformed lines
      }

      // Parse operation type
      if (op_type == "LD" || op_type == "R" || op_type == "r" || op_type == "ld") {
        entry.is_write = false;
      } else if (op_type == "ST" || op_type == "W" || op_type == "w" || op_type == "st") {
        entry.is_write = true;
      } else {
        m_logger->warn("Unknown operation type '{}', skipping line", op_type);
        continue;
      }

      // Parse address
      entry.addr = parse_address(addr_str);

      // Parse optional domain_id
      int domain_id;
      if (iss >> domain_id) {
        entry.domain_id = domain_id;
      }

      if (entry.domain_id > max_domain_seen) {
        max_domain_seen = entry.domain_id;
      }

      m_trace.push_back(entry);
    }

    trace_file.close();
    m_trace_length = m_trace.size();
    m_num_domains = max_domain_seen + 1;

    if (m_trace_length == 0) {
      throw ConfigurationError("Trace {} is empty!", file_path_str);
    }
  }

  Addr_t parse_address(const std::string& addr_str) {
    if (addr_str.size() >= 2 && 
        (addr_str.substr(0, 2) == "0x" || addr_str.substr(0, 2) == "0X")) {
      return static_cast<Addr_t>(std::stoull(addr_str.substr(2), nullptr, 16));
    }
    return static_cast<Addr_t>(std::stoull(addr_str));
  }
};

}  // namespace Ramulator
