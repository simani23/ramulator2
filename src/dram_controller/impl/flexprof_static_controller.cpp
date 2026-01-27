#include <fstream>
#include <sstream>
#include <array>
#include <numeric>
#include <algorithm>
#include <cmath>

#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

/**
 * @brief FlexProf Static Controller with pre-configured turn-based domain scheduling
 * 
 * This controller implements FlexProf's temporal partitioning mechanism with STATIC
 * (pre-configured at startup) scheduling parameters:
 * 
 * Key Features:
 * - Per-domain read/write queues for temporal isolation
 * - Static per-domain service turn allocation (configured at startup)
 * - Static read/write scheduling bias per domain (configured at startup)
 * - Turn-based round-robin scheduling across domains
 * 
 * SST Integration:
 * - SST provides: domain_id (via source_id), read/write type, address
 * - Controller implements: temporal isolation via pre-computed turn schedule
 * 
 * The turn schedule is computed once at initialization from:
 * - domain_turn_allocations: how many turns each domain gets per round
 * - domain_rw_biases: ratio of read vs write turns per domain
 * 
 * Example: With 3 domains, allocations=[2,1,1], biases=[0.5, 1.0, 0.0]:
 *   Turn 0: Domain 0, Read
 *   Turn 1: Domain 0, Write
 *   Turn 2: Domain 1, Read
 *   Turn 3: Domain 2, Write
 *   (schedule repeats)
 */
class FlexProfStaticController final : public IDRAMController, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, FlexProfStaticController, "FlexProfStatic", 
    "FlexProf DRAM controller with static pre-configured turn-based domain scheduling.");

private:
  static constexpr int MAX_DOMAINS = 16;
  static constexpr int READ_TURN_LENGTH = 11;     // Cycles for read-biased turn (tCAS + tBURST)
  static constexpr int WRITE_TURN_LENGTH = 12;    // Cycles for write-biased turn (tCWL + tBURST)

  // Request buffers
  std::deque<Request> m_pending;          // Requests waiting for completion callback
  ReqBuffer m_active_buffer;              // Requests with open rows (highest priority)
  ReqBuffer m_priority_buffer;            // High-priority requests (refresh, etc.)
  
  // Per-domain read and write queues for isolation
  std::array<ReqBuffer, MAX_DOMAINS> m_domain_read_buffers;
  std::array<ReqBuffer, MAX_DOMAINS> m_domain_write_buffers;

  // ============================================================
  // Static Turn Schedule (computed once at initialization)
  // ============================================================
  
  struct TurnEntry {
    int domain_id;
    bool is_read;     // true = read-biased turn, false = write-biased turn
    int turn_length;  // cycles for this turn type
  };
  
  std::vector<TurnEntry> m_turn_schedule;   // Pre-computed static schedule
  size_t m_schedule_idx = 0;                // Current position in schedule
  int m_current_turn_cycle = 0;             // Cycle within current turn

  // ============================================================
  // Configuration Parameters (static, set at initialization)
  // ============================================================
  int m_num_domains = 8;
  int m_default_turn_allocation = 1;        // Default turns per domain
  float m_default_rw_bias = 0.6f;           // Default read bias (60% reads)
  int m_queue_size = 64;                    // Per-domain queue size
  
  // Per-domain static configuration
  std::array<int, MAX_DOMAINS> m_domain_turn_allocation;
  std::array<float, MAX_DOMAINS> m_domain_rw_bias;

  // DRAM level indices
  int m_bank_addr_idx = -1;
  int m_rank_addr_idx = -1;

  // ============================================================
  // Statistics
  // ============================================================
  size_t s_num_read_reqs = 0;
  size_t s_num_write_reqs = 0;
  size_t s_total_turns = 0;
  size_t s_read_turns = 0;
  size_t s_write_turns = 0;
  size_t s_idle_turns = 0;                  // Turns where domain had no ready requests
  size_t s_read_latency = 0;
  
  std::array<size_t, MAX_DOMAINS> s_domain_reads = {0};
  std::array<size_t, MAX_DOMAINS> s_domain_writes = {0};
  std::array<size_t, MAX_DOMAINS> s_domain_turns_served = {0};

public:
  void init() override {
    // Configuration parameters
    m_num_domains = param<int>("num_domains").desc("Number of security domains").default_val(8);
    m_default_turn_allocation = param<int>("default_turn_allocation").desc("Default turns per domain").default_val(1);
    m_default_rw_bias = param<float>("default_rw_bias").desc("Default read/write bias (0.0-1.0, higher = more reads)").default_val(0.6f);
    m_queue_size = param<int>("queue_size").desc("Per-domain queue size").default_val(64);

    if (m_num_domains > MAX_DOMAINS) {
      throw ConfigurationError("FlexProfStatic: num_domains ({}) exceeds MAX_DOMAINS ({})", 
                               m_num_domains, MAX_DOMAINS);
    }

    // Initialize per-domain parameters to defaults
    for (int d = 0; d < MAX_DOMAINS; d++) {
      m_domain_turn_allocation[d] = m_default_turn_allocation;
      m_domain_rw_bias[d] = m_default_rw_bias;
    }

    // Parse static per-domain turn allocations (if provided)
    if (m_config["domain_turn_allocations"]) {
      auto allocations = m_config["domain_turn_allocations"].as<std::vector<int>>();
      for (size_t i = 0; i < allocations.size() && i < (size_t)m_num_domains; i++) {
        m_domain_turn_allocation[i] = std::max(1, allocations[i]);  // At least 1 turn
      }
    }
    
    // Parse static per-domain R/W biases (if provided)
    if (m_config["domain_rw_biases"]) {
      auto biases = m_config["domain_rw_biases"].as<std::vector<float>>();
      for (size_t i = 0; i < biases.size() && i < (size_t)m_num_domains; i++) {
        m_domain_rw_bias[i] = std::clamp(biases[i], 0.0f, 1.0f);
      }
    }

    // Build the static turn schedule
    build_static_schedule();

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

    spdlog::info("FlexProfStatic: Initialized with {} domains, schedule length = {}", 
                 m_num_domains, m_turn_schedule.size());
  }

  void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
    m_dram = memory_system->get_ifce<IDRAM>();
    m_bank_addr_idx = m_dram->m_levels("bank");
    m_rank_addr_idx = m_dram->m_levels("rank");
    
    m_priority_buffer.max_size = 512 * 3 + 32;

    // Set per-domain queue capacities
    for (int d = 0; d < m_num_domains; d++) {
      m_domain_read_buffers[d].max_size = m_queue_size;
      m_domain_write_buffers[d].max_size = m_queue_size;
    }

    // Register statistics
    register_stat(s_num_read_reqs).name("num_read_reqs_{}", m_channel_id);
    register_stat(s_num_write_reqs).name("num_write_reqs_{}", m_channel_id);
    register_stat(s_total_turns).name("flexprof_static_total_turns_{}", m_channel_id);
    register_stat(s_read_turns).name("flexprof_static_read_turns_{}", m_channel_id);
    register_stat(s_write_turns).name("flexprof_static_write_turns_{}", m_channel_id);
    register_stat(s_idle_turns).name("flexprof_static_idle_turns_{}", m_channel_id);
    register_stat(s_read_latency).name("total_read_latency_{}", m_channel_id);

    for (int d = 0; d < m_num_domains; d++) {
      register_stat(s_domain_reads[d]).name("domain_{}_reads", d);
      register_stat(s_domain_writes[d]).name("domain_{}_writes", d);
      register_stat(s_domain_turns_served[d]).name("domain_{}_turns_served", d);
    }

    // Log the static schedule for debugging
    spdlog::info("FlexProfStatic: Static turn schedule:");
    for (size_t i = 0; i < m_turn_schedule.size(); i++) {
      const auto& turn = m_turn_schedule[i];
      spdlog::info("  Turn {}: Domain {}, {} ({} cycles)", 
                   i, turn.domain_id, turn.is_read ? "READ" : "WRITE", turn.turn_length);
    }
  }

  // ============================================================
  // Request Interface
  // ============================================================

  bool send(Request& req) override {
    req.final_command = m_dram->m_request_translations(req.type_id);

    // Use source_id as domain_id (SST provides this)
    int domain_id = req.source_id;
    if (domain_id < 0 || domain_id >= m_num_domains) {
      domain_id = 0;  // Default to domain 0
    }

    req.arrive = m_clk;

    bool success = false;
    if (req.type_id == Request::Type::Read) {
      // Check for forwarding from write buffer (same domain only - isolation)
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

  // ============================================================
  // Main Tick Function
  // ============================================================

  void tick() override {
    m_clk++;

    // 1. Serve completed reads
    serve_completed_reads();

    // 2. Handle refresh
    m_refresh->tick();

    // 3. Find a request to serve using static turn-based scheduling
    ReqBuffer::iterator req_it;
    ReqBuffer* buffer = nullptr;
    bool request_found = schedule_static_request(req_it, buffer);

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

    // 7. Advance turn timing
    advance_turn_timing();
  }

private:
  // ============================================================
  // Build Static Turn Schedule
  // ============================================================

  /**
   * @brief Build the static turn schedule from configuration
   * 
   * For each domain, we create turns based on:
   * - domain_turn_allocations[d]: number of turns
   * - domain_rw_biases[d]: ratio of read to write turns
   * 
   * The schedule is deterministic and repeats.
   */
  void build_static_schedule() {
    m_turn_schedule.clear();

    for (int d = 0; d < m_num_domains; d++) {
      int total_turns = m_domain_turn_allocation[d];
      float read_bias = m_domain_rw_bias[d];
      
      // Calculate number of read and write turns for this domain
      int read_turns = static_cast<int>(std::round(total_turns * read_bias));
      int write_turns = total_turns - read_turns;
      
      // Ensure at least one of each if total_turns > 1 and bias is not extreme
      if (total_turns > 1) {
        if (read_turns == 0 && read_bias > 0.0f) read_turns = 1;
        if (write_turns == 0 && read_bias < 1.0f) write_turns = 1;
        // Rebalance
        if (read_turns + write_turns > total_turns) {
          if (read_bias >= 0.5f) write_turns = total_turns - read_turns;
          else read_turns = total_turns - write_turns;
        }
      }

      // Add turns for this domain in an interleaved pattern
      // This provides better bandwidth utilization than all reads then all writes
      int r_added = 0, w_added = 0;
      for (int t = 0; t < total_turns; t++) {
        TurnEntry entry;
        entry.domain_id = d;
        
        // Interleave reads and writes based on remaining counts
        if (r_added < read_turns && w_added < write_turns) {
          // Both have remaining, use ratio to decide
          float target_read_ratio = static_cast<float>(read_turns) / total_turns;
          float current_read_ratio = (r_added + w_added > 0) ? 
            static_cast<float>(r_added) / (r_added + w_added) : 0.0f;
          entry.is_read = (current_read_ratio < target_read_ratio);
        } else {
          entry.is_read = (r_added < read_turns);
        }
        
        entry.turn_length = entry.is_read ? READ_TURN_LENGTH : WRITE_TURN_LENGTH;
        
        if (entry.is_read) r_added++;
        else w_added++;
        
        m_turn_schedule.push_back(entry);
      }
    }

    if (m_turn_schedule.empty()) {
      // Fallback: at least one turn per domain
      for (int d = 0; d < m_num_domains; d++) {
        TurnEntry entry;
        entry.domain_id = d;
        entry.is_read = true;
        entry.turn_length = READ_TURN_LENGTH;
        m_turn_schedule.push_back(entry);
      }
    }

    m_schedule_idx = 0;
    m_current_turn_cycle = 0;
  }

  // ============================================================
  // Helper Functions
  // ============================================================

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

  /**
   * @brief Schedule a request using the static turn schedule
   */
  bool schedule_static_request(ReqBuffer::iterator& req_it, ReqBuffer*& buffer) {
    bool request_found = false;

    // 1. First check active buffer (requests with open rows - highest priority)
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

    // 3. Static turn-based scheduling for current turn
    const TurnEntry& current_turn = m_turn_schedule[m_schedule_idx];
    int domain = current_turn.domain_id;
    bool prefer_read = current_turn.is_read;
    
    // Select primary buffer based on current turn type
    ReqBuffer& primary_buffer = prefer_read ? 
      m_domain_read_buffers[domain] : 
      m_domain_write_buffers[domain];
    
    ReqBuffer& secondary_buffer = prefer_read ? 
      m_domain_write_buffers[domain] : 
      m_domain_read_buffers[domain];

    // Try primary buffer first
    request_found = try_schedule_from_buffer(primary_buffer, req_it, buffer);

    // If primary buffer has no ready requests, try secondary buffer
    // This allows flexibility while maintaining the bias over time
    if (!request_found) {
      request_found = try_schedule_from_buffer(secondary_buffer, req_it, buffer);
    }

    return request_found;
  }

  /**
   * @brief Try to schedule a request from a specific buffer
   */
  bool try_schedule_from_buffer(ReqBuffer& buf, ReqBuffer::iterator& req_it, ReqBuffer*& buffer) {
    if (buf.size() == 0) {
      return false;
    }

    // Find best ready request using FRFCFS within the buffer
    for (auto it = buf.begin(); it != buf.end(); ++it) {
      it->command = m_dram->get_preq_command(it->final_command, it->addr_vec);
      
      if (m_dram->check_ready(it->command, it->addr_vec)) {
        req_it = it;
        buffer = &buf;
        return true;
      }
    }

    return false;
  }

  /**
   * @brief Advance the turn timing and move to next turn in schedule
   */
  void advance_turn_timing() {
    m_current_turn_cycle++;

    const TurnEntry& current_turn = m_turn_schedule[m_schedule_idx];

    // Check if current turn is complete
    if (m_current_turn_cycle >= current_turn.turn_length) {
      // End of turn - update statistics
      s_total_turns++;
      s_domain_turns_served[current_turn.domain_id]++;
      
      if (current_turn.is_read) {
        s_read_turns++;
      } else {
        s_write_turns++;
      }

      // Check if domain had any requests this turn (for idle tracking)
      // Note: This is a simplified check; exact tracking would require per-turn state
      
      // Move to next turn in the static schedule (wraps around)
      m_current_turn_cycle = 0;
      m_schedule_idx = (m_schedule_idx + 1) % m_turn_schedule.size();
    }
  }

  void finalize() override {
    spdlog::info("FlexProfStatic Controller {} finalized:", m_channel_id);
    spdlog::info("  Total turns: {}, Read turns: {}, Write turns: {}", 
                 s_total_turns, s_read_turns, s_write_turns);
    spdlog::info("  Schedule length: {}, Schedule cycles completed: {}", 
                 m_turn_schedule.size(), s_total_turns / m_turn_schedule.size());
    
    for (int d = 0; d < m_num_domains; d++) {
      if (s_domain_reads[d] > 0 || s_domain_writes[d] > 0) {
        spdlog::info("  Domain {}: {} reads, {} writes, {} turns (alloc={}, bias={:.2f})",
                     d, s_domain_reads[d], s_domain_writes[d], s_domain_turns_served[d],
                     m_domain_turn_allocation[d], m_domain_rw_bias[d]);
      }
    }
  }
};

}  // namespace Ramulator
