#include <iostream>
#include <chrono>
#include <iomanip>

#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "base/base.h"
#include "base/config.h"
#include "frontend/frontend.h"
#include "memory_system/memory_system.h"
#include "example/example_ifce.h"

namespace {
  // Format duration in human-readable form
  std::string format_duration(std::chrono::seconds secs) {
    auto hours = std::chrono::duration_cast<std::chrono::hours>(secs);
    secs -= hours;
    auto mins = std::chrono::duration_cast<std::chrono::minutes>(secs);
    secs -= mins;
    
    std::ostringstream oss;
    if (hours.count() > 0) {
      oss << hours.count() << "h ";
    }
    if (mins.count() > 0 || hours.count() > 0) {
      oss << mins.count() << "m ";
    }
    oss << secs.count() << "s";
    return oss.str();
  }
}

int main(int argc, char* argv[]) {
  // Parse command line arguments
  argparse::ArgumentParser program("Ramulator", "2.0");
  program.add_argument("-c", "--config").metavar("\"dumped YAML configuration\"")
    .help("String dump of the yaml configuration.");
  program.add_argument("-f", "--config_file").metavar("path-to-configuration-file")
    .help("Path to a YAML configuration file.");
  program.add_argument("-p", "--param").metavar("KEY=VALUE")
    .append()
    .help("Specify parameter to override in the configuration file. Repeat this option to change multiple parameters.");
  program.add_argument("-v", "--verbose")
    .default_value(false)
    .implicit_value(true)
    .help("Enable verbose output with periodic progress updates.");
  program.add_argument("--progress-interval").metavar("CYCLES")
    .default_value(std::string("10000000"))
    .help("Cycles between progress updates when verbose (default: 10M cycles).");

  try {
    program.parse_args(argc, argv);
  }
  catch (const std::runtime_error& err) {
    spdlog::error(err.what());
    std::cerr << program;
    std::exit(1);
  }

  // Are we accepting the configuration YAML through commandline dump?
  bool use_dumped_yaml = false;
  std::string dumped_config;
  if (auto arg = program.present<std::string>("-c")) {
    use_dumped_yaml = true;
    dumped_config = *arg;
  }

  // Are we gettign a path to a YAML document?
  bool use_yaml_file = false;
  std::string config_file_path;
  if (auto arg = program.present<std::string>("-f")) {
    use_yaml_file = true;
    config_file_path = *arg;
  }

  // Are we overriding some parameters in a YAML document from the comand line?
  bool has_param_override = false;
  std::vector<std::string> params;
  if (auto arg = program.present<std::vector<std::string>>("-p")) {
    has_param_override = true;
    params = *arg;
  }

  // Some sanity check of the inputs
  if (use_dumped_yaml && use_yaml_file) {
    spdlog::error("Dumped config and loaded config cannot be used together!");
    std::cerr << program;
    std::exit(1);
  } else if (!(use_dumped_yaml || use_yaml_file)) {
    spdlog::error("No configuration specified!");
    std::cerr << program;
    std::exit(1);
  }

  if (use_dumped_yaml && has_param_override) {
    spdlog::warn("Using dumped configuration. Parameter overrides with -p/--param will be ignored!");
  }

  // Get verbose and progress interval settings
  bool verbose = program.get<bool>("-v");
  uint64_t progress_interval = std::stoull(program.get<std::string>("--progress-interval"));
  
  // Parse the configurations
  YAML::Node config;
  if (use_dumped_yaml) {
    std::string dumped_config = program.get<std::string>("-c");
    config = YAML::Load(dumped_config);
  } else if (use_yaml_file) {
    config = Ramulator::Config::parse_config_file(config_file_path, params);
  }

  // Instaniate the frontend of the simulated system, this is one of the top-level objects in Ramulator 2.0.
  // It also recursively instaniate all components in the frontend.
  auto frontend = Ramulator::Factory::create_frontend(config);
  // Instaniate the memory system of the simulated system, this is one of the top-level objects in Ramulator 2.0
  // It also recursively instaniate all components in the memory system.
  auto memory_system = Ramulator::Factory::create_memory_system(config);

  // Connect the frontend and the memory system together,
  // this recursively calls the "setup" function in all instaniated components
  // so that they can get each other's parameters (if needed) after their initialization
  frontend->connect_memory_system(memory_system);
  memory_system->connect_frontend(frontend);

  // Get the relative clock ratio between the frontend and memory system
  int frontend_tick = frontend->get_clock_ratio();
  int mem_tick = memory_system->get_clock_ratio();

  int tick_mult = frontend_tick * mem_tick;

  // Start timing
  auto start_time = std::chrono::steady_clock::now();
  uint64_t last_progress_cycle = 0;
  
  if (verbose) {
    auto progress = frontend->get_progress();
    if (progress.has_progress_info) {
      size_t target = progress.max_requests > 0 ? progress.max_requests : progress.trace_length;
      spdlog::info("Starting simulation: {} requests to process", target);
    }
    spdlog::info("Progress updates every {} cycles", progress_interval);
  }

  for (uint64_t i = 0;; i++) {
    if (((i % tick_mult) % mem_tick) == 0) {
      frontend->tick();
    }

    if (frontend->is_finished()) {
      break;
    }

    if ((i % tick_mult) % frontend_tick == 0) {
      memory_system->tick();
    }

    // Progress output (only in verbose mode)
    if (verbose && (i - last_progress_cycle) >= progress_interval) {
      last_progress_cycle = i;
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
      
      auto progress = frontend->get_progress();
      if (progress.has_progress_info) {
        size_t target = progress.max_requests > 0 ? progress.max_requests : progress.trace_length;
        double pct = (target > 0) ? (100.0 * progress.requests_sent / target) : 0.0;
        
        // Estimate remaining time
        std::string eta_str = "calculating...";
        if (progress.requests_sent > 0 && elapsed.count() > 0) {
          double rate = static_cast<double>(progress.requests_sent) / elapsed.count();
          size_t remaining = target - progress.requests_sent;
          auto eta_secs = std::chrono::seconds(static_cast<long>(remaining / rate));
          eta_str = format_duration(eta_secs);
        }
        
        spdlog::info("Progress: {}/{} ({:.1f}%) | Elapsed: {} | ETA: {}",
                     progress.requests_sent, target, pct,
                     format_duration(elapsed), eta_str);
      } else {
        spdlog::info("Cycle: {} | Elapsed: {}", i, format_duration(elapsed));
      }
    }
  }

  // Calculate total duration
  auto end_time = std::chrono::steady_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  // Finalize the simulation. Recursively print all statistics from all components
  frontend->finalize();
  memory_system->finalize();

  // Print timing summary
  spdlog::info("Simulation completed in {}", format_duration(
    std::chrono::duration_cast<std::chrono::seconds>(total_duration)));
  spdlog::info("Total time: {:.3f} seconds", total_duration.count() / 1000.0);

  return 0;
}