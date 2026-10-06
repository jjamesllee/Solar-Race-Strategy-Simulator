/* Starting point of a race simulation */

#include <stdio.h>
#include <stdlib.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Car.hpp"
#include "Sim.hpp"
#include "Utils.hpp"

int main(int argc, char* argv[]) {
  RUNTIME_EXCEPTION(argc == 3,
                    "Need base route location and dni csv location. Example "
                    "./sim.exe baseroute.csv dni.csv");

  // Control stops - DO NOT TOUCH
  // For each idx in control_stops, the car must stop for 30 minutes at
  // route.get_route_points()[idx] This Must be reflected in your Sim class
  // implementation
  std::unordered_set<size_t> control_stops = {2962,  5559,  9462,  11421, 14439,
                                              16990, 20832, 23202, 25987};

  // Load base route csv
  Route route{std::string(argv[1])};

  // Load forecast irradiance csv
  ForecastLut forecast_lut{std::string(argv[2])};

  // Create your model of the car
  std::shared_ptr<Car> car = std::make_shared<Car>();

  // First coordinate in baseroute.csv
  const Coord starting_coord = route.get_route_points()[0];

  // Start time of the first race day
  const Time starting_time = Time("2023-10-22 10:00:00", -9.5);

  // Create your simulator object and set route parameters
  Simulator simulator(car, starting_coord, starting_time);
  simulator.set_control_stops(control_stops);
  simulator.set_forecast_lut(forecast_lut);
  simulator.set_route(route);

  // Ask which simulation to run
  std::cout << "Choose simulation mode:\n"
            << "  1. Whole-race constant-speed sweep\n"
            << "  2. Segment-based speed strategy\n"
            << "  3. Speed strategy for every CSV route segment\n"
            << "Selection: ";
  int mode = 0;
  std::cin >> mode;

  // Test every whole-race constant speed from 1 to 99 km/h
  if (mode == 1) {
    for (int i = 1; i < 100; ++i) {
      const double speed = kph2mps(i);
      if (simulator.run_sim(speed)) {
        std::cout << "Speed " << i << " is viable" << std::endl;
      } else {
        std::cout << "Speed " << i << " is not viable" << std::endl;
      }
    }
    // Both strategy modes share the same optimizer and car/race calculations
  } else if (mode == 2 || mode == 3) {
    // Get strategy
    StrategyConfig strategy;
    // Option 3 reselects speed between every pair of adjacent CSV points
    strategy.every_route_segment = (mode == 3);

    // Let the user enable or disable optional strategy features before the run
    std::cout << "\nStrategy settings (final sprint is off by default):\n"
              << "  Type 'final sprint' to toggle it on or off.\n"
              << "  Type 'start' to run the simulation.\n";

    std::string command;
    while (true) {
      std::cout << "Setting: ";
      std::getline(std::cin >> std::ws, command);

      if (command == "final sprint") {
        strategy.final_sprint = !strategy.final_sprint;
        std::cout << "Final sprint "
                  << (strategy.final_sprint ? "enabled." : "disabled.")
                  << std::endl;
      } else if (command == "start") {
        break;
      } else {
        std::cout << "Unknown setting. Type 'final sprint' or 'start'."
                  << std::endl;
      }
    }

    // Run simulation with strategy
    const bool viable = simulator.run_segment_strategy(strategy);
    std::cout << (viable ? "Segment strategy is viable."
                         : "Segment strategy is not viable.")
              << std::endl;
  } else {
    std::cerr << "Invalid selection. Choose 1, 2, or 3." << std::endl;
    return 1;
  }
  return 0;
}