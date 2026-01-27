#include <fstream>
#include <sstream>
#include <array>
#include <numeric>
#include <algorithm>
#include <cmath>

#include "dram_controller/controller.h"
#include "dram_controller/flexprof_dynamic.h"
#include "memory_system/memory_system.h"

namespace Ramulator {

/**
 * @brief FlexProf Dynamic Controller with runtime-adaptable domain scheduling
 * 
 * This controller implements FlexProf's temporal partitioning mechanism with:
 * - Per-domain read/write queues for isolation
 * - Dynamic per-domain service turn allocation (adaptable at runtime)
 * - Dynamic read/write scheduling bias per domain (adaptable at runtime)
 * - Turn-based round-robin scheduling across domains
 * 
 * SST provides: domain_id, read/write type, address
 * Controller provides: temporal isolation via turn-based scheduling
 */
class FlexProfDynamicController final : public IDRAMController, public Implementation, 
                                        public IFlexProfDynamicConfig {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, FlexProfDynamicController, "FlexProfDynamic", 
    "FlexProf DRAM controller with dynamic turn-based domain scheduling and runtime-adaptable parameters.");

private:
  static constexpr int MAX_DOMAINS = 16;
  static constexpr int DEFAULT_TURN_LENGTH = 11;  // Cycles per turn (matches DRAM timing)
  static constexpr int READ_TURN_LENGTH = 11;     // Cycles for read-biased turn
  static constexpr int WRITE_TURN_LENGTH = 12;    // Cycles for write-biased turn

  // Request buffers
  std::deque<Request> m_pending;          // Requests waiting for completion callback
  ReqBuffer m_active_buffer;              // Requests with open rows (highest priority)
  ReqBuffer m_priority_buffer;            // High-priority requests (refresh, etc.)
  
  // Per-domain read and write queues for isolation
  std::array<ReqBuffer, MAX_DOMAINS> m_domain_read_buffers;
  std::array<ReqBuffer, MAX_DOMAINS> m_domain_write_buffers;

  // ============================================================
  // Dynamic Turn-Based Scheduling State
  // ============================================================
  
  // Per-domain turn allocation: how many consecutive turns each domain gets
  // Sum of all allocations determines the scheduling period
  std::array<int, MAX_DOMAINS> m_domain_turn_allocation;
  
  // Per-domain read/write bias: ratio of read turns vs write turns
  // 0.0 = all writes, 1.0 = all reads, 0.5 = balanced
  std::array<float, MAX_DOMAINS> m_domain_rw_bias;
  
  // Current scheduling state
  int m_current_domain = 0;               // Which domain is currently being served
  int m_domain_turns_remaining = 0;       // Turns remaining for current domain
  int m_current_turn_cycle = 0;           // Cycle within current turn
  int m_current_turn_length = DEFAULT_TURN_LENGTH;
  bool m_current_turn_is_read = true;     // Whether current turn prioritizes reads
  
  // Track read/write turns issued per domain (for bias tracking within allocation)
  std::array<int, MAX_DOMAINS> m_domain_read_turns_issued;
  std::array<int, MAX_DOMAINS> m_domain_write_turns_issued;

  // ============================================================
  // Configuration Parameters
  // ============================================================
  int m_num_domains = 8;
  int m_default_turn_allocation = 1;      // Default turns per domain
  float m_default_rw_bias = 0.6f;         // Default read bias (60% reads)
  int m_queue_size = 64;                  // Per-domain queue size
  int m_adaptation_interval = 10000;      // Cycles between auto-adaptation (0 = disabled)
  bool m_auto_adapt = false;              // Enable automatic adaptation based on queue pressure

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
  size_t s_idle_turns = 0;                // Turns where domain had no ready requests
  size_t s_read_latency = 0;
  size_t s_adaptation_events = 0;
  
  std::array<size_t, MAX_DOMAINS> s_domain_reads = {0};
  std::array<size_t, MAX_DOMAINS> s_domain_writes = {0};
  std::array<size_t, MAX_DOMAINS> s_domain_turns_served = {0};
  std::array<size_t, MAX_DOMAINS> s_domain_idle_turns = {0};

  // Queue pressure tracking for auto-adaptation
  std::array<size_t, MAX_DOMAINS> m_domain_queue_pressure_accum = {0};
  size_t m_pressure_sample_count = 0;

public:
  void init() override {
    // Configuration parameters
    m_num_domains = param<int>("num_domains").desc("Number of security domains").default_val(8);
    m_default_turn_allocation = param<int>("default_turn_allocation").desc("Default turns per domain").default_val(1);
    m_default_rw_bias = param<float>("default_rw_bias").desc("Default read/write bias (0.0-1.0, higher = more reads)").default_val(0.6f);
    m_queue_size = param<int>("queue_size").desc("Per-domain queue size").default_val(64);
    m_adaptation_interval = param<int>("adaptation_interval").desc("Cycles between auto-adaptation (0 = disabled)").default_val(0);
    m_auto_adapt = param<bool>("auto_adapt").desc("Enable automatic adaptation based on queue pressure").default_val(false);

    // Initialize per-domain parameters to defaults
    for (int d = 0; d < MAX_DOMAINS; d++) {
      m_domain_turn_allocation[d] = m_default_turn_allocation;
      m_domain_rw_bias[d] = m_default_rw_bias;
      m_domain_read_turns_issued[d] = 0;
      m_domain_write_turns_issued[d] = 0;
    }

    // Parse initial per-domain configuration if provided
    if (m_config["domain_turn_allocations"]) {
      auto allocations = m_config["domain_turn_allocations"].as<std::vector<int>>();
      for (size_t i = 0; i < allocations.size() && i < MAX_DOMAINS; i++) {
        m_domain_turn_allocation[i] = allocations[i];
      }
    }
    
    if (m_config["domain_rw_biases"]) {
      auto biases = m_config["domain_rw_biases"].as<std::vector<float>>();
      for (size_t i = 0; i < biases.size() && i < MAX_DOMAINS; i++) {
        m_domain_rw_bias[i] = std::clamp(biases[i], 0.0f, 1.0f);
      }
    }

    // Initialize scheduling state
    m_current_domain = 0;
    m_domain_turns_remaining = m_domain_turn_allocation[0];
    m_current_turn_cycle = 0;
    m_current_turn_is_read = should_be_read_turn(0);
    m_current_turn_length = m_current_turn_is_read ? READ_TURN_LENGTH : WRITE_TURN_LENGTH;

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

    spdlog::info("FlexProfDynamic: Initialized with {} domains, auto_adapt={}", 
                 m_num_domains, m_auto_adapt);
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
    register_stat(s_total_turns).name("flexprof_dyn_total_turns_{}", m_channel_id);
    register_stat(s_read_turns).name("flexprof_dyn_read_turns_{}", m_channel_id);
    register_stat(s_write_turns).name("flexprof_dyn_write_turns_{}", m_channel_id);
    register_stat(s_idle_turns).name("flexprof_dyn_idle_turns_{}", m_channel_id);
    register_stat(s_read_latency).name("total_read_latency_{}", m_channel_id);
    register_stat(s_adaptation_events).name("flexprof_dyn_adaptation_events_{}", m_channel_id);

    for (int d = 0; d < m_num_domains; d++) {
      register_stat(s_domain_reads[d]).name("domain_{}_reads", d);
      register_stat(s_domain_writes[d]).name("domain_{}_writes", d);
      register_stat(s_domain_turns_served[d]).name("domain_{}_turns_served", d);
      register_stat(s_domain_idle_turns[d]).name("domain_{}_idle_turns", d);
    }
  }

  // ============================================================
  // Runtime Configuration API (for SST integration)
  // Implements IFlexProfDynamicConfig interface
  // ============================================================
  
  /**
   * @brief Set the turn allocation for a specific domain
   * @param domain_id Domain identifier (0 to num_domains-1)
   * @param turns Number of consecutive turns allocated to this domain
   */
  void set_domain_turn_allocation(int domain_id, int turns) override {
    if (domain_id >= 0 && domain_id < m_num_domains && turns > 0) {
      m_domain_turn_allocation[domain_id] = turns;
      spdlog::debug("FlexProfDynamic: Domain {} turn allocation set to {}", domain_id, turns);
    }
  }

  /**
   * @brief Set the read/write bias for a specific domain
   * @param domain_id Domain identifier (0 to num_domains-1)
   * @param bias Read bias (0.0 = all writes, 1.0 = all reads)
   */
  void set_domain_rw_bias(int domain_id, float bias) override {
    if (domain_id >= 0 && domain_id < m_num_domains) {
      m_domain_rw_bias[domain_id] = std::clamp(bias, 0.0f, 1.0f);
      // Reset turn tracking for this domain to apply new bias
      m_domain_read_turns_issued[domain_id] = 0;
      m_domain_write_turns_issued[domain_id] = 0;
      spdlog::debug("FlexProfDynamic: Domain {} R/W bias set to {:.2f}", domain_id, bias);
    }
  }

  /**
   * @brief Get current turn allocation for a domain
   */
  int get_domain_turn_allocation(int domain_id) const override {
    if (domain_id >= 0 && domain_id < m_num_domains) {
      return m_domain_turn_allocation[domain_id];
    }
    return 0;
  }

  /**
   * @brief Get current R/W bias for a domain
   */
  float get_domain_rw_bias(int domain_id) const override {
    if (domain_id >= 0 && domain_id < m_num_domains) {
      return m_domain_rw_bias[domain_id];
    }
    return 0.5f;
  }

  /**
   * @brief Get queue occupancy for a domain (useful for SST monitoring)
   */
  std::pair<size_t, size_t> get_domain_queue_occupancy(int domain_id) const override {
    if (domain_id >= 0 && domain_id < m_num_domains) {
      return {m_domain_read_buffers[domain_id].size(), 
              m_domain_write_buffers[domain_id].size()};
    }
    return {0, 0};
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
      // Check for forwarding from write buffer (same domain)
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

    // 3. Auto-adaptation (if enabled)
    if (m_auto_adapt && m_adaptation_interval > 0 && 
        (m_clk % m_adaptation_interval) == 0) {
      perform_auto_adaptation();
    }

    // 4. Find a request to serve using dynamic turn-based scheduling
    ReqBuffer::iterator req_it;
    ReqBuffer* buffer = nullptr;
    bool request_found = schedule_dynamic_request(req_it, buffer);

    // 5. Update row policy
    m_rowpolicy->update(request_found, req_it);

    // 6. Update plugins
    for (auto plugin : m_plugins) {
      plugin->update(request_found, req_it);
    }

    // 7. Issue command if request found
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

    // 8. Advance turn timing
    advance_turn_timing();

    // 9. Accumulate queue pressure for adaptation
    if (m_auto_adapt) {
      accumulate_queue_pressure();
    }
  }

private:
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
   * @brief Determine if the next turn for a domain should be read or write
   * 
   * Uses the domain's R/W bias and tracks issued turns to maintain the ratio
   */
  bool should_be_read_turn(int domain_id) {
    float bias = m_domain_rw_bias[domain_id];
    int read_issued = m_domain_read_turns_issued[domain_id];
    int write_issued = m_domain_write_turns_issued[domain_id];
    int total_issued = read_issued + write_issued;
    
    if (total_issued == 0) {
      // First turn: use bias as probability threshold
      return bias >= 0.5f;
    }
    
    // Calculate current read ratio and compare to target bias
    float current_read_ratio = static_cast<float>(read_issued) / total_issued;
    return current_read_ratio < bias;
  }

  /**
   * @brief Schedule a request using dynamic turn-based domain scheduling
   */
  bool schedule_dynamic_request(ReqBuffer::iterator& req_it, ReqBuffer*& buffer) {
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

    // 3. Dynamic turn-based scheduling for current domain
    int domain = m_current_domain;
    
    // Select primary buffer based on current turn type (read or write)
    ReqBuffer& primary_buffer = m_current_turn_is_read ? 
      m_domain_read_buffers[domain] : 
      m_domain_write_buffers[domain];
    
    ReqBuffer& secondary_buffer = m_current_turn_is_read ? 
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
   * @brief Advance the turn timing and switch domains when needed
   */
  void advance_turn_timing() {
    m_current_turn_cycle++;

    // Check if current turn is complete
    if (m_current_turn_cycle >= m_current_turn_length) {
      // End of turn - update statistics
      s_total_turns++;
      s_domain_turns_served[m_current_domain]++;
      
      if (m_current_turn_is_read) {
        s_read_turns++;
        m_domain_read_turns_issued[m_current_domain]++;
      } else {
        s_write_turns++;
        m_domain_write_turns_issued[m_current_domain]++;
      }

      // Check if domain had any activity this turn
      // (Simplified check - more sophisticated tracking could be added)
      
      m_current_turn_cycle = 0;
      m_domain_turns_remaining--;

      // Check if we need to switch to next domain
      if (m_domain_turns_remaining <= 0) {
        advance_to_next_domain();
      } else {
        // Same domain, but may switch read/write mode
        m_current_turn_is_read = should_be_read_turn(m_current_domain);
        m_current_turn_length = m_current_turn_is_read ? READ_TURN_LENGTH : WRITE_TURN_LENGTH;
      }
    }
  }

  /**
   * @brief Advance to the next domain in the round-robin schedule
   */
  void advance_to_next_domain() {
    // Find next domain with non-zero allocation
    int start_domain = m_current_domain;
    do {
      m_current_domain = (m_current_domain + 1) % m_num_domains;
    } while (m_domain_turn_allocation[m_current_domain] == 0 && 
             m_current_domain != start_domain);

    // Reset state for new domain
    m_domain_turns_remaining = m_domain_turn_allocation[m_current_domain];
    m_current_turn_is_read = should_be_read_turn(m_current_domain);
    m_current_turn_length = m_current_turn_is_read ? READ_TURN_LENGTH : WRITE_TURN_LENGTH;

    // Reset read/write turn tracking for fresh bias calculation
    // (done periodically to prevent drift)
    int total_turns = m_domain_read_turns_issued[m_current_domain] + 
                      m_domain_write_turns_issued[m_current_domain];
    if (total_turns > 100) {
      m_domain_read_turns_issued[m_current_domain] = 0;
      m_domain_write_turns_issued[m_current_domain] = 0;
    }
  }

  /**
   * @brief Accumulate queue pressure for auto-adaptation
   */
  void accumulate_queue_pressure() {
    for (int d = 0; d < m_num_domains; d++) {
      m_domain_queue_pressure_accum[d] += 
        m_domain_read_buffers[d].size() + m_domain_write_buffers[d].size();
    }
    m_pressure_sample_count++;
  }

  /**
   * @brief Perform automatic adaptation based on queue pressure
   * 
   * Domains with higher queue pressure get more turns
   */
  void perform_auto_adaptation() {
    if (m_pressure_sample_count == 0) return;

    // Calculate average pressure per domain
    std::array<float, MAX_DOMAINS> avg_pressure;
    float total_pressure = 0.0f;
    
    for (int d = 0; d < m_num_domains; d++) {
      avg_pressure[d] = static_cast<float>(m_domain_queue_pressure_accum[d]) / 
                        m_pressure_sample_count;
      total_pressure += avg_pressure[d];
    }

    // Skip if no pressure
    if (total_pressure < 1.0f) {
      // Reset accumulators
      std::fill(m_domain_queue_pressure_accum.begin(), 
                m_domain_queue_pressure_accum.end(), 0);
      m_pressure_sample_count = 0;
      return;
    }

    // Calculate total turn budget (maintain sum of allocations)
    int total_turns = 0;
    for (int d = 0; d < m_num_domains; d++) {
      total_turns += m_domain_turn_allocation[d];
    }
    if (total_turns == 0) total_turns = m_num_domains;

    // Redistribute turns based on pressure
    bool changed = false;
    for (int d = 0; d < m_num_domains; d++) {
      float pressure_ratio = avg_pressure[d] / total_pressure;
      int new_allocation = std::max(1, static_cast<int>(std::round(pressure_ratio * total_turns)));
      
      if (new_allocation != m_domain_turn_allocation[d]) {
        m_domain_turn_allocation[d] = new_allocation;
        changed = true;
      }

      // Also adapt R/W bias based on read vs write queue pressure
      if (m_domain_read_buffers[d].size() + m_domain_write_buffers[d].size() > 0) {
        float read_pressure = static_cast<float>(m_domain_read_buffers[d].size());
        float total_domain_pressure = read_pressure + m_domain_write_buffers[d].size();
        float new_bias = read_pressure / total_domain_pressure;
        
        // Smooth the bias change
        m_domain_rw_bias[d] = 0.8f * m_domain_rw_bias[d] + 0.2f * new_bias;
      }
    }

    if (changed) {
      s_adaptation_events++;
      spdlog::debug("FlexProfDynamic: Auto-adaptation performed at cycle {}", m_clk);
    }

    // Reset accumulators
    std::fill(m_domain_queue_pressure_accum.begin(), 
              m_domain_queue_pressure_accum.end(), 0);
    m_pressure_sample_count = 0;
  }

  void finalize() override {
    spdlog::info("FlexProfDynamic Controller {} finalized:", m_channel_id);
    spdlog::info("  Total turns: {}, Read turns: {}, Write turns: {}, Idle turns: {}", 
                 s_total_turns, s_read_turns, s_write_turns, s_idle_turns);
    spdlog::info("  Adaptation events: {}", s_adaptation_events);
    
    for (int d = 0; d < m_num_domains; d++) {
      if (s_domain_reads[d] > 0 || s_domain_writes[d] > 0) {
        spdlog::info("  Domain {}: {} reads, {} writes, {} turns, final allocation={}, final bias={:.2f}",
                     d, s_domain_reads[d], s_domain_writes[d], s_domain_turns_served[d],
                     m_domain_turn_allocation[d], m_domain_rw_bias[d]);
      }
    }
  }
};

}  // namespace Ramulator
