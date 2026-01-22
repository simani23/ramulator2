#include <fstream>
#include <sstream>
#include <array>

#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

/**
 * @brief Pattern entry for FlexProf scheduling
 */
struct FlexProfPattern {
  int domain_id;
  int op;    // 0 = read, 1 = write
  int bank;
};

class FlexProfController final : public IDRAMController, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, FlexProfController, "FlexProf", 
    "FlexProf DRAM controller with pattern-based scheduling and per-domain queues.");

private:
  static constexpr int MAX_DOMAINS = 12;

  std::deque<Request> m_pending;          // Requests waiting for data
  ReqBuffer m_active_buffer;              // Requests being served (row open)
  ReqBuffer m_priority_buffer;            // High-priority requests (refresh)
  
  // Per-domain read and write queues
  std::array<ReqBuffer, MAX_DOMAINS> m_domain_read_buffers;
  std::array<ReqBuffer, MAX_DOMAINS> m_domain_write_buffers;

  // Pattern data
  std::vector<FlexProfPattern> m_pattern;
  size_t m_pattern_idx = 0;
  
  // Timing state
  int m_current_turn_time = 0;
  int m_turn_len = 11;
  int m_rank_used = -1;
  bool m_read_ready = false;
  bool m_write_ready = false;

  // Configuration
  int m_num_domains = 7;
  int m_alteration = 4;
  std::string m_pattern_file;

  // DRAM info
  int m_bank_addr_idx = -1;
  int m_rank_addr_idx = -1;

  // Statistics
  size_t s_num_read_reqs = 0;
  size_t s_num_write_reqs = 0;
  size_t s_pattern_turns = 0;
  size_t s_read_latency = 0;
  std::array<size_t, MAX_DOMAINS> s_domain_reads = {0};
  std::array<size_t, MAX_DOMAINS> s_domain_writes = {0};

public:
  void init() override {
    m_num_domains = param<int>("num_domains").desc("Number of security domains").default_val(7);
    m_alteration = param<int>("alteration").desc("Bank rotation factor").default_val(4);
    m_pattern_file = param<std::string>("pattern_file").desc("Path to FlexProf pattern file").required();

    // Load the pattern file
    load_pattern(m_pattern_file);

    // Create child components
    m_scheduler = create_child_ifce<IScheduler>();
    m_refresh = create_child_ifce<IRefreshManager>();
    m_rowpolicy = create_child_ifce<IRowPolicy>();

    // Load plugins if configured
    if (m_config["plugins"]) {
      YAML::Node plugin_configs = m_config["plugins"];
      for (auto it = plugin_configs.begin(); it != plugin_configs.end(); ++it) {
        m_plugins.push_back(create_child_ifce<IControllerPlugin>(*it));
      }
    }
  }

  void load_pattern(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
      throw ConfigurationError("FlexProf: Cannot open pattern file: {}", filename);
    }

    std::string line;
    while (std::getline(file, line)) {
      std::istringstream iss(line);
      FlexProfPattern entry;
      if (iss >> entry.domain_id >> entry.op >> entry.bank) {
        m_pattern.push_back(entry);
      }
    }

    if (m_pattern.empty()) {
      throw ConfigurationError("FlexProf: Pattern file is empty: {}", filename);
    }

    spdlog::info("FlexProf: Loaded {} pattern entries from {}", m_pattern.size(), filename);
  }

  void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
    m_dram = memory_system->get_ifce<IDRAM>();
    m_bank_addr_idx = m_dram->m_levels("bank");
    m_rank_addr_idx = m_dram->m_levels("rank");
    
    m_priority_buffer.max_size = 512 * 3 + 32;

    // Set per-domain queue capacities
    for (int d = 0; d < m_num_domains; d++) {
      m_domain_read_buffers[d].max_size = 64;
      m_domain_write_buffers[d].max_size = 64;
    }

    // Register statistics
    register_stat(s_num_read_reqs).name("num_read_reqs_{}", m_channel_id);
    register_stat(s_num_write_reqs).name("num_write_reqs_{}", m_channel_id);
    register_stat(s_pattern_turns).name("flexprof_pattern_turns_{}", m_channel_id);
    register_stat(s_read_latency).name("total_read_latency_{}", m_channel_id);

    for (int d = 0; d < m_num_domains; d++) {
      register_stat(s_domain_reads[d]).name("domain_{}_reads", d);
      register_stat(s_domain_writes[d]).name("domain_{}_writes", d);
    }
  }

  bool send(Request& req) override {
    req.final_command = m_dram->m_request_translations(req.type_id);

    // Use source_id as domain_id (0-indexed)
    int domain_id = req.source_id;
    if (domain_id < 0 || domain_id >= m_num_domains) {
      domain_id = 0;  // Default to domain 0
    }

    req.arrive = m_clk;

    bool success = false;
    if (req.type_id == Request::Type::Read) {
      // Check for forwarding from write buffer
      auto compare_addr = [&req](const Request& wreq) {
        return wreq.addr == req.addr;
      };
      if (std::find_if(m_domain_write_buffers[domain_id].begin(), 
                       m_domain_write_buffers[domain_id].end(), 
                       compare_addr) != m_domain_write_buffers[domain_id].end()) {
        req.depart = m_clk + 1;
        m_pending.push_back(req);
        return true;
      }

      success = m_domain_read_buffers[domain_id].enqueue(req);
      if (success) {
        s_num_read_reqs++;
        s_domain_reads[domain_id]++;
      }
    } else if (req.type_id == Request::Type::Write) {
      success = m_domain_write_buffers[domain_id].enqueue(req);
      if (success) {
        s_num_write_reqs++;
        s_domain_writes[domain_id]++;
      }
    }

    return success;
  }

  bool priority_send(Request& req) override {
    req.final_command = m_dram->m_request_translations(req.type_id);
    return m_priority_buffer.enqueue(req);
  }

  void tick() override {
    m_clk++;

    // 1. Serve completed reads
    serve_completed_reads();

    // 2. Handle refresh
    m_refresh->tick();

    // 3. Find a request to serve using FlexProf pattern
    ReqBuffer::iterator req_it;
    ReqBuffer* buffer = nullptr;
    bool request_found = schedule_flexprof_request(req_it, buffer);

    // 4. Update row policy
    m_rowpolicy->update(request_found, req_it);

    // 5. Update plugins
    for (auto plugin : m_plugins) {
      plugin->update(request_found, req_it);
    }

    // 6. Issue command if request found
    if (request_found) {
      m_dram->issue_command(req_it->command, req_it->addr_vec);

      // Check if this is the final command
      if (req_it->command == req_it->final_command) {
        if (req_it->type_id == Request::Type::Read) {
          req_it->depart = m_clk + m_dram->m_read_latency;
          m_pending.push_back(*req_it);
        }
        buffer->remove(req_it);
      } else {
        // If opening a row, move to active buffer
        if (m_dram->m_command_meta(req_it->command).is_opening) {
          if (m_active_buffer.enqueue(*req_it)) {
            buffer->remove(req_it);
          }
        }
      }
    }

    // 7. Advance pattern timing
    advance_pattern_timing();
  }

private:
  void serve_completed_reads() {
    while (m_pending.size() && m_pending.front().depart <= m_clk) {
      auto& req = m_pending.front();
      if (req.depart - req.arrive > 1) {
        s_read_latency += req.depart - req.arrive;
      }
      if (req.callback) {
        req.callback(req);
      }
      m_pending.pop_front();
    }
  }

  bool schedule_flexprof_request(ReqBuffer::iterator& req_it, ReqBuffer*& buffer) {
    bool request_found = false;

    // 1. First check active buffer (requests with open rows)
    if (req_it = m_scheduler->get_best_request(m_active_buffer); 
        req_it != m_active_buffer.end()) {
      if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
        request_found = true;
        buffer = &m_active_buffer;
        return request_found;
      }
    }

    // 2. Check priority buffer (refresh, etc.)
    if (m_priority_buffer.size() != 0) {
      req_it = m_priority_buffer.begin();
      req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);
      if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
        buffer = &m_priority_buffer;
        return true;
      }
      // Priority buffer has requests but can't issue - block other requests
      return false;
    }

    // 3. FlexProf pattern-based scheduling
    auto& pattern_entry = m_pattern[m_pattern_idx];
    int domain_turn = pattern_entry.domain_id;
    int write_mode = pattern_entry.op;
    int bank_turn = m_pattern_idx % m_alteration;

    // Select appropriate domain queue based on pattern operation type
    ReqBuffer& domain_buffer = write_mode ? 
      m_domain_write_buffers[domain_turn] : 
      m_domain_read_buffers[domain_turn];

    // Check timing within turn (FlexProf sends at specific cycle offsets)
    bool can_send = false;
    if (write_mode == 0) {  // Read mode
      can_send = (m_current_turn_time == 0 || m_current_turn_time == 6);
    } else {  // Write mode
      can_send = (m_current_turn_time == 6 || m_current_turn_time == 12);
    }

    if (!can_send) {
      return false;
    }

    // Find matching request from domain buffer
    for (auto it = domain_buffer.begin(); it != domain_buffer.end(); ++it) {
      it->command = m_dram->get_preq_command(it->final_command, it->addr_vec);

      // Check bank alignment with pattern
      int req_bank = it->addr_vec[m_bank_addr_idx];
      int req_rank = it->addr_vec[m_rank_addr_idx];
      
      // Use rank for bank alternation (as in original FlexProf)
      if ((req_rank % m_alteration) != bank_turn) {
        continue;
      }

      // Check if command is ready
      if (m_dram->check_ready(it->command, it->addr_vec)) {
        req_it = it;
        buffer = &domain_buffer;
        request_found = true;
        break;
      }
    }

    // If no matching request found, try fallback to other operation type
    if (!request_found && can_send) {
      ReqBuffer& fallback_buffer = write_mode ? 
        m_domain_read_buffers[domain_turn] : 
        m_domain_write_buffers[domain_turn];

      for (auto it = fallback_buffer.begin(); it != fallback_buffer.end(); ++it) {
        it->command = m_dram->get_preq_command(it->final_command, it->addr_vec);

        int req_rank = it->addr_vec[m_rank_addr_idx];
        if ((req_rank % m_alteration) != bank_turn) {
          continue;
        }

        if (m_dram->check_ready(it->command, it->addr_vec)) {
          req_it = it;
          buffer = &fallback_buffer;
          request_found = true;
          break;
        }
      }
    }

    return request_found;
  }

  void advance_pattern_timing() {
    m_current_turn_time++;

    if (m_current_turn_time >= m_turn_len) {
      // Move to next pattern entry
      m_current_turn_time = 0;
      m_pattern_idx = (m_pattern_idx + 1) % m_pattern.size();
      s_pattern_turns++;

      // Set turn length based on next operation type
      auto& next_entry = m_pattern[m_pattern_idx];
      m_turn_len = (next_entry.op == 0) ? 11 : 12;  // Reads: 11 cycles, Writes: 12 cycles

      // Reset per-turn state
      m_rank_used = -1;
      m_read_ready = false;
      m_write_ready = false;
    }
  }

  void finalize() override {
    spdlog::info("FlexProf Controller {} finalized. Total pattern turns: {}", 
                 m_channel_id, s_pattern_turns);
  }
};

}  // namespace Ramulator
