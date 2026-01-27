#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

/**
 * @brief Frontend that reads SST-format memory traces for FlexProf isolation.
 * 
 * This frontend reads traces in the format that SST would provide:
 *   <R|W> <address> <domain_id>
 * 
 * Where:
 *   - R/W: Read or Write operation
 *   - address: Physical memory address (decimal or hex with 0x prefix)
 *   - domain_id: Security domain ID (VM, tenant, etc.)
 * 
 * Supported formats:
 *   R <addr> <domain_id>      Read to specified domain
 *   W <addr> <domain_id>      Write to specified domain
 *   READ <addr> <domain_id>   Alternative read syntax
 *   WRITE <addr> <domain_id>  Alternative write syntax
 * 
 * Example trace:
 *   R 0x1000 0
 *   W 0x2000 0
 *   R 0x3000 1
 *   W 0x4000 1
 *   R 0x5000 2
 * 
 * This is the recommended format for SST-Ramulator integration with FlexProf's
 * memory-controller isolation mechanism.
 */
class SSTTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, SSTTrace, "SSTTrace", 
    "SST-format trace: R/W <address> <domain_id> for FlexProf isolation.")

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

  int m_num_domains = 1;       // Detected from trace
  bool m_wrap_trace = false;   // Whether to wrap around when trace ends

  Logger_t m_logger;

public:
  void init() override {
    std::string trace_path_str = param<std::string>("path")
      .desc("Path to the SST-format trace file.").required();
    m_clock_ratio = param<uint>("clock_ratio").required();
    m_max_requests = param<size_t>("max_requests")
      .desc("Maximum requests to send (0 = run full trace once)").default_val(0);
    m_wrap_trace = param<bool>("wrap_trace")
      .desc("Wrap around when trace ends (useful with max_requests)").default_val(false);

    m_logger = Logging::create_logger("SSTTrace");
    m_logger->info("Loading SST-format trace file {} ...", trace_path_str);
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
      entry.domain_id,  // domain_id as source_id for FlexProf controller
      nullptr
    );

    if (m_memory_system->send(req)) {
      m_requests_sent++;
      m_curr_trace_idx++;
      
      if (m_curr_trace_idx >= m_trace_length) {
        if (m_wrap_trace && (m_max_requests == 0 || m_requests_sent < m_max_requests)) {
          // Wrap around
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
    size_t line_num = 0;
    std::string line;
    
    while (std::getline(trace_file, line)) {
      line_num++;
      
      // Skip empty lines and comments
      if (line.empty() || line[0] == '#') {
        continue;
      }
      
      // Trim leading whitespace
      size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos) {
        continue;
      }
      line = line.substr(start);

      std::istringstream iss(line);
      TraceEntry entry;

      std::string op_type;
      std::string addr_str;
      int domain_id;
      
      // Parse: <R|W> <address> <domain_id>
      if (!(iss >> op_type >> addr_str >> domain_id)) {
        m_logger->warn("Line {}: Malformed entry '{}', skipping", line_num, line);
        continue;
      }

      // Parse operation type
      if (op_type == "R" || op_type == "r" || op_type == "READ" || op_type == "read" || op_type == "LD" || op_type == "ld") {
        entry.is_write = false;
      } else if (op_type == "W" || op_type == "w" || op_type == "WRITE" || op_type == "write" || op_type == "ST" || op_type == "st") {
        entry.is_write = true;
      } else {
        m_logger->warn("Line {}: Unknown operation type '{}', skipping", line_num, op_type);
        continue;
      }

      // Parse address
      entry.addr = parse_address(addr_str);

      // Validate domain_id
      if (domain_id < 0) {
        m_logger->warn("Line {}: Negative domain_id {}, using 0", line_num, domain_id);
        domain_id = 0;
      }
      entry.domain_id = domain_id;

      if (domain_id > max_domain_seen) {
        max_domain_seen = domain_id;
      }

      m_trace.push_back(entry);
    }

    trace_file.close();
    m_trace_length = m_trace.size();
    m_num_domains = max_domain_seen + 1;

    if (m_trace_length == 0) {
      throw ConfigurationError("Trace {} is empty or has no valid entries!", file_path_str);
    }

    // Log domain distribution
    std::vector<size_t> domain_counts(m_num_domains, 0);
    std::vector<size_t> domain_reads(m_num_domains, 0);
    std::vector<size_t> domain_writes(m_num_domains, 0);
    
    for (const auto& entry : m_trace) {
      domain_counts[entry.domain_id]++;
      if (entry.is_write) {
        domain_writes[entry.domain_id]++;
      } else {
        domain_reads[entry.domain_id]++;
      }
    }
    
    for (int d = 0; d < m_num_domains; d++) {
      if (domain_counts[d] > 0) {
        m_logger->info("  Domain {}: {} entries ({} reads, {} writes)", 
                       d, domain_counts[d], domain_reads[d], domain_writes[d]);
      }
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
