#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>

#include "frontend/frontend.h"
#include "base/exception.h"

namespace Ramulator {

namespace fs = std::filesystem;

/**
 * @brief Frontend that reads multiple standard ramulator2 traces with FlexProf isolation.
 * 
 * This frontend allows using regular ramulator2 traces (SimpleO3 format) with the
 * FlexProf controller's per-domain queue isolation mechanism. Each trace file
 * represents a different security domain.
 * 
 * Supported trace formats per file:
 *   <bubble_count> <addr>                    (read-only)
 *   <bubble_count> <load_addr> <store_addr>  (read + write)
 * 
 * Addresses can be decimal or hex (0x prefix).
 * 
 * The domain_id is assigned based on the trace file index in the configuration.
 */
class FlexProfMultiTrace : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, FlexProfMultiTrace, "FlexProfMultiTrace", 
    "Multi-trace FlexProf frontend: each trace file = one security domain.")

private:
  struct TraceEntry {
    int bubble_count;
    Addr_t load_addr;
    Addr_t store_addr;  // -1 if no store
  };

  struct DomainTrace {
    std::vector<TraceEntry> entries;
    size_t trace_length = 0;
    size_t curr_idx = 0;
    int remaining_bubbles = 0;
    bool load_pending = false;
    bool store_pending = false;
    Addr_t pending_load_addr = -1;
    Addr_t pending_store_addr = -1;
    size_t requests_sent = 0;
    bool finished = false;
  };

  std::vector<DomainTrace> m_domain_traces;
  int m_num_domains = 0;
  int m_current_domain = 0;  // Round-robin scheduling among domains

  size_t m_total_requests_sent = 0;
  size_t m_max_requests = 0;

  // Scheduling mode: "round_robin" or "interleaved"
  std::string m_scheduling_mode = "round_robin";

  Logger_t m_logger;

public:
  void init() override {
    std::vector<std::string> trace_list = param<std::vector<std::string>>("traces")
      .desc("List of trace files. Each file = one security domain.").required();
    m_clock_ratio = param<uint>("clock_ratio").required();
    m_max_requests = param<size_t>("max_requests")
      .desc("Maximum total requests to send (0 = run until all traces complete)").default_val(0);
    m_scheduling_mode = param<std::string>("scheduling_mode")
      .desc("How to schedule between domains: round_robin, interleaved").default_val("round_robin");

    m_logger = Logging::create_logger("FlexProfMultiTrace");
    m_num_domains = trace_list.size();

    m_logger->info("Loading {} trace files for FlexProf multi-domain simulation...", m_num_domains);

    // Load each trace file as a separate domain
    for (int domain_id = 0; domain_id < m_num_domains; domain_id++) {
      DomainTrace domain;
      load_trace(trace_list[domain_id], domain);
      m_domain_traces.push_back(std::move(domain));
      m_logger->info("  Domain {}: {} entries from {}", 
                     domain_id, m_domain_traces[domain_id].trace_length, trace_list[domain_id]);
    }

    // Initialize first entry for each domain
    for (int d = 0; d < m_num_domains; d++) {
      if (m_domain_traces[d].trace_length > 0) {
        auto& entry = m_domain_traces[d].entries[0];
        m_domain_traces[d].remaining_bubbles = entry.bubble_count;
        m_domain_traces[d].pending_load_addr = entry.load_addr;
        m_domain_traces[d].pending_store_addr = entry.store_addr;
        m_domain_traces[d].load_pending = (entry.load_addr != -1);
        m_domain_traces[d].store_pending = (entry.store_addr != -1);
      }
    }

    m_logger->info("FlexProf multi-trace frontend initialized with {} domains", m_num_domains);
  }

  void tick() override {
    if (is_finished()) {
      return;
    }

    // Try to send requests from all domains (round-robin or interleaved)
    int domains_tried = 0;
    while (domains_tried < m_num_domains) {
      int domain_id = m_current_domain;
      DomainTrace& domain = m_domain_traces[domain_id];

      // Move to next domain for next iteration
      m_current_domain = (m_current_domain + 1) % m_num_domains;
      domains_tried++;

      if (domain.finished) {
        continue;
      }

      // Process bubbles
      if (domain.remaining_bubbles > 0) {
        domain.remaining_bubbles--;
        if (m_scheduling_mode == "round_robin") {
          break;  // Only process one domain per tick in round-robin
        }
        continue;
      }

      // Try to send load
      if (domain.load_pending) {
        Request req(
          domain.pending_load_addr,
          Request::Type::Read,
          domain_id,  // domain_id as source_id for FlexProf controller routing
          nullptr
        );

        if (m_memory_system->send(req)) {
          domain.load_pending = false;
          domain.requests_sent++;
          m_total_requests_sent++;

          // Check if we hit max requests limit
          if (m_max_requests > 0 && m_total_requests_sent >= m_max_requests) {
            return;
          }
        } else {
          // Could not send, try next domain
          if (m_scheduling_mode == "round_robin") {
            break;
          }
          continue;
        }
      }

      // Try to send store
      if (domain.store_pending) {
        Request req(
          domain.pending_store_addr,
          Request::Type::Write,
          domain_id,
          nullptr
        );

        if (m_memory_system->send(req)) {
          domain.store_pending = false;
          domain.requests_sent++;
          m_total_requests_sent++;

          if (m_max_requests > 0 && m_total_requests_sent >= m_max_requests) {
            return;
          }
        } else {
          if (m_scheduling_mode == "round_robin") {
            break;
          }
          continue;
        }
      }

      // Both load and store sent (or not present), move to next trace entry
      if (!domain.load_pending && !domain.store_pending) {
        domain.curr_idx++;
        
        if (domain.curr_idx >= domain.trace_length) {
          // Trace completed - wrap around or mark finished
          domain.curr_idx = 0;
          if (m_max_requests == 0) {
            // If no max_requests, run until one pass through all traces
            domain.finished = true;
            continue;
          }
        }

        // Load next entry
        auto& entry = domain.entries[domain.curr_idx];
        domain.remaining_bubbles = entry.bubble_count;
        domain.pending_load_addr = entry.load_addr;
        domain.pending_store_addr = entry.store_addr;
        domain.load_pending = (entry.load_addr != -1);
        domain.store_pending = (entry.store_addr != -1);
      }

      if (m_scheduling_mode == "round_robin") {
        break;  // One domain per tick
      }
    }
  }

  bool is_finished() override {
    if (m_max_requests > 0) {
      return m_total_requests_sent >= m_max_requests;
    }
    
    // Check if all domains have completed their traces
    for (const auto& domain : m_domain_traces) {
      if (!domain.finished) {
        return false;
      }
    }
    return true;
  }

  int get_num_cores() override {
    return m_num_domains;
  }

  SimulationProgress get_progress() override {
    SimulationProgress progress;
    progress.requests_sent = m_total_requests_sent;
    progress.max_requests = m_max_requests;
    size_t total_trace_length = 0;
    for (const auto& domain : m_domain_traces) {
      total_trace_length += domain.trace_length;
    }
    progress.trace_length = total_trace_length;
    progress.has_progress_info = true;
    return progress;
  }

private:
  void load_trace(const std::string& file_path_str, DomainTrace& domain) {
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
      if (line.empty() || line[0] == '#') {
        continue;  // Skip empty lines and comments
      }

      std::istringstream iss(line);
      TraceEntry entry;
      
      std::string token1, token2, token3;
      if (!(iss >> token1 >> token2)) {
        continue;  // Skip malformed lines
      }

      entry.bubble_count = std::stoi(token1);
      entry.load_addr = parse_address(token2);
      entry.store_addr = -1;

      // Check for optional store address
      if (iss >> token3) {
        entry.store_addr = parse_address(token3);
      }

      domain.entries.push_back(entry);
    }

    trace_file.close();
    domain.trace_length = domain.entries.size();

    if (domain.trace_length == 0) {
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
