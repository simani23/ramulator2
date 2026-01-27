#ifndef RAMULATOR_CONTROLLER_FLEXPROF_DYNAMIC_H
#define RAMULATOR_CONTROLLER_FLEXPROF_DYNAMIC_H

#include <utility>
#include "dram_controller/controller.h"

namespace Ramulator {

/**
 * @brief Interface for FlexProf Dynamic Controller runtime configuration
 * 
 * This interface allows external components (like SST) to dynamically
 * configure the FlexProf memory controller's domain scheduling parameters
 * at runtime.
 * 
 * Usage from SST:
 *   1. Get the controller instance from the memory system
 *   2. Cast to IFlexProfDynamicConfig if supported
 *   3. Use the API to adjust scheduling parameters
 * 
 * Example:
 *   auto* controller = memory_system->get_controller();
 *   if (auto* flexprof = dynamic_cast<IFlexProfDynamicConfig*>(controller)) {
 *     flexprof->set_domain_turn_allocation(domain_id, turns);
 *     flexprof->set_domain_rw_bias(domain_id, bias);
 *   }
 */
class IFlexProfDynamicConfig {
public:
  virtual ~IFlexProfDynamicConfig() = default;

  /**
   * @brief Set the turn allocation for a specific domain
   * 
   * Controls how many consecutive turns a domain receives before
   * the scheduler moves to the next domain. Higher allocation means
   * more bandwidth for that domain.
   * 
   * @param domain_id Domain identifier (0 to num_domains-1)
   * @param turns Number of consecutive turns (must be > 0)
   */
  virtual void set_domain_turn_allocation(int domain_id, int turns) = 0;

  /**
   * @brief Set the read/write bias for a specific domain
   * 
   * Controls the ratio of read-prioritized turns vs write-prioritized
   * turns within a domain's allocation. This allows per-domain
   * optimization based on workload characteristics.
   * 
   * @param domain_id Domain identifier (0 to num_domains-1)
   * @param bias Read bias value:
   *             - 0.0 = all writes (write-heavy workload)
   *             - 0.5 = balanced
   *             - 1.0 = all reads (read-heavy workload)
   */
  virtual void set_domain_rw_bias(int domain_id, float bias) = 0;

  /**
   * @brief Get current turn allocation for a domain
   * 
   * @param domain_id Domain identifier
   * @return Current turn allocation, or 0 if domain_id is invalid
   */
  virtual int get_domain_turn_allocation(int domain_id) const = 0;

  /**
   * @brief Get current R/W bias for a domain
   * 
   * @param domain_id Domain identifier
   * @return Current R/W bias (0.0-1.0), or 0.5 if domain_id is invalid
   */
  virtual float get_domain_rw_bias(int domain_id) const = 0;

  /**
   * @brief Get queue occupancy for a domain
   * 
   * Useful for SST to monitor queue pressure and make scheduling decisions.
   * 
   * @param domain_id Domain identifier
   * @return Pair of (read_queue_size, write_queue_size), or (0,0) if invalid
   */
  virtual std::pair<size_t, size_t> get_domain_queue_occupancy(int domain_id) const = 0;
};

/**
 * @brief Domain scheduling configuration structure
 * 
 * Convenience structure for bulk configuration updates from SST.
 */
struct FlexProfDomainConfig {
  int domain_id;
  int turn_allocation;
  float rw_bias;
  
  FlexProfDomainConfig(int id = 0, int turns = 1, float bias = 0.5f)
    : domain_id(id), turn_allocation(turns), rw_bias(bias) {}
};

/**
 * @brief Helper function to configure multiple domains at once
 * 
 * @param controller Controller implementing IFlexProfDynamicConfig
 * @param configs Vector of domain configurations
 */
inline void configure_flexprof_domains(
    IFlexProfDynamicConfig* controller,
    const std::vector<FlexProfDomainConfig>& configs) {
  if (!controller) return;
  for (const auto& config : configs) {
    controller->set_domain_turn_allocation(config.domain_id, config.turn_allocation);
    controller->set_domain_rw_bias(config.domain_id, config.rw_bias);
  }
}

}  // namespace Ramulator

#endif  // RAMULATOR_CONTROLLER_FLEXPROF_DYNAMIC_H
