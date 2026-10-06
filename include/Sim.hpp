/*
Class to run the a full scale simulation on a WSC type route
*/

#pragma once

#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "Car.hpp"
#include "CustomTime.hpp"
#include "Luts.hpp"

// These values control the driving strategy only.  They do not
// change the specified car model or official race timings
struct StrategyConfig {
  // Distance between strategy decisions. Optimizer picks one speed, uses it for
  // given distance, then makes a new decision once distance exceeded
  double segment_length_km = 10.0;
  // If true, reconsider speed at every CSV route point instead of every above
  // km
  bool every_route_segment = false;
  // Max and min segment speeds, as well as the canidate speeds tested by the
  // simulation (e.g. min = 30, max = 70, step = 5, tests 30, 35, 40, 45... 70)
  // The initial target gives the first decision a realistic starting point;
  // max_speed_change_kph limits how far the first choice can move from it.
  double initial_speed_kph = 50.0;
  double min_speed_kph = 30.0;
  double max_speed_kph = 70.0;
  double speed_step_kph = 5.0;
  // Energy that must remain in battery after simulating next distance segment
  // (battery in %), safety buffer
  double battery_reserve_percent = 15.0;
  // Distance of upcoming segment considered when making each speed change
  // decision, e.g. if climb expected within next 50 km, will not choose to
  // accelerate, conserve energy for the climb
  double lookahead_distance_km = 75.0;
  // Max speed change per segment, pervents sudden jumps in speed
  double max_speed_change_kph = 10.0;
  // Terminal output for segment stats (e.g. Strategy segment 4 at 30.0 km: 50.0
  // km/h, battery 83.5%)
  bool print_segment_decisions = true;

  // Final sprint settings, last xx km will use all battery and leave xx % left
  bool final_sprint = false;
  double final_sprint_max_speed_kph = 90.0;
  double final_sprint_distance_km = 100.0;
  double final_sprint_battery_percent = 2.0;
};

class Simulator {
 private:
  // Lookup tables
  Route route;
  ForecastLut forecast_lut;

  // Control stops
  std::unordered_set<size_t> control_stops;

  // NO TOUCH ANYTHING IN SIMULATION PARAMETERS
  /* ---------------------- Simulation parameters ------------------------- */
  // Step size in seconds when charging
  const int CHARGING_STEP_SIZE = 30;
  // Stop time at a control stop in seconds
  const int control_stop_charge_time = 1800;
  // For each timestamp, we track the UTC offset. For the location of the race
  // (Australia), it is 9.5 hours ahead of UTC Day one start time in 24 hour
  // local time
  const Time day_one_start_time = Time("2023-10-22 10:00:00", -9.5);
  // Day one end time in 24 hour local time
  const Time day_one_end_time = Time("2023-10-22 18:00:00", -9.5);
  // Start time from day 2 onwards in 24 hour local time
  const Time day_start_time = Time("09:00:00");
  // End time from day 2 onwards in 24 hour local time
  const Time day_end_time = Time("17:00:00");
  // End time of the entire race in 24 hour local time
  const Time race_end_time = Time("2023-10-28 17:00:00", -9.5);
  // NO TOUCH
  /* ---------------------- Simulation parameters ------------------------- */

  // Starting coordinate of the car
  Coord starting_coord;
  // Starting time of the simulation
  Time curr_time;
  Time starting_time;

  /* Energy model of the car to simulate on */
  std::shared_ptr<Car> car;

  // Run simulation can run both segment-based and fixed-speed simulation:
  // inputing no strategy means a fixed-speed run, while inputing strategy means
  // returning a speed at each route segment
  bool run_simulation(double constant_speed, const StrategyConfig* strategy);

 public:
  /** Construct all simulator objects this way
   * @param model Energy model for your car
   * @param starting_coord The starting coordinate of the car
   * @param current_time Current starting time of the simulation
   */
  Simulator(std::shared_ptr<Car> model, Coord starting_coord,
            Time starting_time);

  // Setters
  inline void set_control_stops(std::unordered_set<size_t> stops) {
    control_stops = stops;
  }
  inline void set_route(Route new_route) { route = new_route; }
  inline void set_forecast_lut(ForecastLut new_forecast_lut) {
    forecast_lut = new_forecast_lut;
  }

  /** @brief Run a full simulation with a car object and a series of route
   * points
   *
   * @param speed: The speed in kph
   *
   * @return True if this is a posible
   */
  bool run_sim(const double speed);

  bool run_segment_strategy(const StrategyConfig& strategy);
};
