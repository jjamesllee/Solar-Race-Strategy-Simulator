#include "Sim.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Utils.hpp"

bool Simulator::run_sim(const double speed) {
  // Runs the simulation with no strategy, constant speed throughout race
  return run_simulation(speed, nullptr);
}

// Runs segment-based strategy
bool Simulator::run_segment_strategy(const StrategyConfig& strategy) {
  // Ensures segment actually has distance before contuining with simulation.
  RUNTIME_EXCEPTION(strategy.segment_length_km > 0.0,
                    "Segment length must be greater than zero");
  // Ensures max speed > min speed, and speed increments > 0
  RUNTIME_EXCEPTION(strategy.min_speed_kph > 0.0 &&
                        strategy.max_speed_kph >= strategy.min_speed_kph &&
                        strategy.initial_speed_kph >= strategy.min_speed_kph &&
                        strategy.initial_speed_kph <= strategy.max_speed_kph &&
                        strategy.speed_step_kph > 0.0,
                    "Invalid speed range in strategy configuration");
  RUNTIME_EXCEPTION(
      strategy.final_sprint_max_speed_kph >= strategy.max_speed_kph,
      "Max sprint speed must be at least the normal max speed");
  // Ensures battery reserve settings within 0-100%
  RUNTIME_EXCEPTION(strategy.battery_reserve_percent >= 0.0 &&
                        strategy.battery_reserve_percent < 100.0,
                    "Battery reserve must be between 0 and 100 percent");
  RUNTIME_EXCEPTION(strategy.final_sprint_distance_km > 0.0,
                    "Final sprint distance must be greater than zero");
  RUNTIME_EXCEPTION(strategy.final_sprint_battery_percent >= 0.0 &&
                        strategy.final_sprint_battery_percent < 100.0,
                    "Final sprint battery must be between 0 and 100 percent");
  // The speed value is ignored when a strategy is given, it selects speeds
  // automatically
  return run_simulation(0.0, &strategy);
}

bool Simulator::run_simulation(double constant_speed,
                               const StrategyConfig* strategy) {
  // Error checks
  RUNTIME_EXCEPTION(car != nullptr, "Car is null");

  const std::vector<Coord>& route_points = route.get_route_points();
  RUNTIME_EXCEPTION(!route_points.empty(), "Route is empty");

  // Start the simulation with a fully charged battery
  double battery_kwh = car->get_battery_capacity();

  // Reset the simulation time for each speed being tested
  curr_time = starting_time;
  forecast_lut.initialize_caches(starting_coord,
                                 curr_time.get_utc_time_point());

  // Values used for simulation decisions
  double distance_travelled_m = 0.0;
  double total_distance_m = 0.0;
  double minimum_battery_kwh = battery_kwh;
  double minimum_battery_distance_m = 0.0;
  double distance_since_strategy_change_m = 0.0;
  double selected_speed_mps = constant_speed;
  double previous_strategy_speed_kph =
      strategy != nullptr ? strategy->initial_speed_kph : 0.0;
  bool final_sprint_active = false;
  size_t current_strategy_segment_count = 0;

  for (size_t i = 0; i + 1 < route_points.size(); ++i) {
    const Coord& current_point = route_points[i];
    const Coord& next_point = route_points[i + 1];

    total_distance_m += get_distance(current_point, next_point);
  }

  double remaining_distance_m = total_distance_m;

  // Charge at route coordinate for the given duration. Used overnight and at
  // required stops, when the car can't drive. Charges in short steps because
  // the DNI value can changes with time
  auto charge_while_stationary = [&](const Coord& coord,
                                     double duration_seconds) {
    double remaining_charge_time = duration_seconds;

    while (remaining_charge_time > 0.0) {
      if (curr_time >= race_end_time) {
        return false;
      }

      // Use 30-second steps, unless remaning time < 30 sec
      const double step_seconds = std::min(
          remaining_charge_time, static_cast<double>(CHARGING_STEP_SIZE));

      // Determine DNI based on time -> solar power received
      ForecastCoord forecast_coord(coord.lat, coord.lon);
      const double dni = forecast_lut.get_value(forecast_coord,
                                                curr_time.get_utc_time_point());
      const double charging_energy_kwh =
          car->get_solar_power(dni) * step_seconds / JOULES_TO_KWH;

      battery_kwh = std::min(battery_kwh + charging_energy_kwh,
                             car->get_battery_capacity());

      curr_time.update_time_seconds(step_seconds);
      remaining_charge_time -= step_seconds;
    }

    return true;
  };

  // Iterate through all the routes, either finding best average speed (Strategy
  // 1) or best speed per segment (Strategy 2)
  for (size_t i = 0; i + 1 < route_points.size(); ++i) {
    const Coord& current_point = route_points[i];
    const Coord& next_point = route_points[i + 1];

    double distance_m = get_distance(current_point, next_point);
    double delta_altitude_m = next_point.alt - current_point.alt;

    // Becomes true only once, when the car first reaches the final sprint
    // distance. This forces an immediate speed decision. final_sprint_active
    // then stays true for the rest of the race
    const bool entering_final_sprint =
        !final_sprint_active && strategy != nullptr && strategy->final_sprint &&
        remaining_distance_m <= strategy->final_sprint_distance_km * KM_TO_M;
    if (entering_final_sprint) {
      final_sprint_active = true;
    }

    // Fixed-speed skips this section because its speed never changes
    // If this is a segment-based run, choose a speed on the first segment, at
    // every CSV segment for option 3, or after roughly 10 km for option 2
    if (strategy != nullptr &&
        (strategy->every_route_segment || current_strategy_segment_count == 0 ||
         entering_final_sprint ||
         distance_since_strategy_change_m >=
             strategy->segment_length_km * KM_TO_M)) {
      // Look forward through the route to estimate upcoming energy
      // requirements. Prevents choosing a fast speed using only the current CSV
      // segment. Only positive altitude changes are summed
      double future_segment_distance = 0.0;
      double uphill_m = 0.0;
      // Continue adding route segments until the look-ahead distance
      // is reached, final sprint is active, or the route ends
      const double lookahead_limit =
          final_sprint_active ? remaining_distance_m
                              : strategy->lookahead_distance_km * KM_TO_M;

      for (size_t lookahead = i; lookahead + 1 < route_points.size() &&
                                 future_segment_distance < lookahead_limit;
           ++lookahead) {
        // Add route segment distance
        future_segment_distance +=
            get_distance(route_points[lookahead], route_points[lookahead + 1]);
        // Add route segment elevation
        uphill_m += std::max(
            0.0, route_points[lookahead + 1].alt - route_points[lookahead].alt);
      }

      // Use the current location and time to get sunlight estimate for
      // look-ahead
      ForecastCoord forecast_coord(current_point.lat, current_point.lon);
      const double current_dni = forecast_lut.get_value(
          forecast_coord, curr_time.get_utc_time_point());
      // Convert reserve battery % to kWh. Use the finish-line target while
      // sprinting and the normal safety reserve otherwise.
      const double reserve_kwh =
          final_sprint_active
              ? car->get_battery_capacity() *
                    strategy->final_sprint_battery_percent / 100.0
              : car->get_battery_capacity() *
                    strategy->battery_reserve_percent / 100.0;
      // Try speeds from fastest to slowest. First speed that passes test is
      // automatically chosen
      const double max_speed_overall =
          final_sprint_active ? strategy->final_sprint_max_speed_kph
                              : strategy->max_speed_kph;
      // If no speed can fulfill the battery reserve requirement, slow down
      // by no more than the configured limit instead of silently
      // jumping straight to the minimum speed
      double chosen_speed_kph = std::clamp(
          previous_strategy_speed_kph - strategy->max_speed_change_kph,
          strategy->min_speed_kph, max_speed_overall);
      bool candidate_found = false;
      for (double candidate_kph = max_speed_overall;
           candidate_kph >= strategy->min_speed_kph - 1e-9;
           candidate_kph -= strategy->speed_step_kph) {
        // Skip speed if it changes too sharply from the previous speed
        if (previous_strategy_speed_kph > 0.0 &&
            std::abs(candidate_kph - previous_strategy_speed_kph) >
                strategy->max_speed_change_kph) {
          continue;
        }
        // m/s for calculations
        const double candidate_mps = kph2mps(candidate_kph);
        // Time req to cover future segment at given speed
        const double cover_future_segment_time =
            future_segment_distance / candidate_mps;
        // Predict battery energy spent on future segment
        const double predicted_use_kwh =
            car->get_energy_consumption(candidate_kph, future_segment_distance,
                                        uphill_m, cover_future_segment_time);
        // Predict solar energy to be captured over future segment
        const double predicted_solar_kwh = car->get_solar_power(current_dni) *
                                           cover_future_segment_time /
                                           JOULES_TO_KWH;
        // Accept the speed if its predicted remaining battery meets the reserve
        if (battery_kwh - predicted_use_kwh + predicted_solar_kwh >=
            reserve_kwh) {
          chosen_speed_kph = candidate_kph;
          candidate_found = true;
          break;
        }
      }

      if (!candidate_found && strategy->print_segment_decisions) {
        std::cout << "No speed met the predicted battery reserve, "
                  << "using controlled fallback speed " << chosen_speed_kph
                  << " km/h." << std::endl;
      }

      // Save the selected speed. Remains active until the next strategy
      // interval
      selected_speed_mps = kph2mps(chosen_speed_kph);
      previous_strategy_speed_kph = chosen_speed_kph;
      distance_since_strategy_change_m = 0.0;
      ++current_strategy_segment_count;
      if (strategy->print_segment_decisions) {
        std::cout << std::fixed << std::setprecision(1) << "Strategy segment "
                  << current_strategy_segment_count << " at "
                  << distance_travelled_m / KM_TO_M
                  << " km: " << chosen_speed_kph << " km/h, battery "
                  << battery_kwh / car->get_battery_capacity() * 100.0 << "%"
                  << std::endl;
      }
    }

    // From here on, all simulations share same travel/ charging calculations
    double speed_kph = selected_speed_mps * MPS_TO_KPH;
    double travel_time_seconds = distance_m / selected_speed_mps;

    // Driving time still needed to finish the current CSV route segment
    double remaining_time = travel_time_seconds;

    // Return an estimated position between the two route points, allows for DNI
    // lookup bewteen segments
    auto interpolate_position = [&](double fraction) {
      return Coord(
          current_point.lat + fraction * (next_point.lat - current_point.lat),
          current_point.lon + fraction * (next_point.lon - current_point.lon),
          current_point.alt + fraction * (next_point.alt - current_point.alt));
    };

    // While there is still some driving left to complete the current pair of
    // points
    while (remaining_time > 0.0) {
      // Derive current position, e.g., if 8 seconds remain in a 20 second
      // segment, we've driven for 60% of the segment
      const double completed_fraction =
          (travel_time_seconds - remaining_time) / travel_time_seconds;
      const Coord current_position = interpolate_position(completed_fraction);

      // Check if within race times
      bool race_day_one =
          curr_time >= day_one_start_time && curr_time < day_one_end_time;

      bool race_day_two_to_seven =
          curr_time >= day_one_end_time && curr_time >= day_start_time &&
          curr_time < day_end_time && curr_time < race_end_time;

      bool racing = race_day_one || race_day_two_to_seven;

      // Outside driving hours, the car stays at current_position and charges.
      // Continues to checks the clock before it can drive again
      if (!racing) {
        if (curr_time >= race_end_time) {
          return false;
        }

        if (!charge_while_stationary(current_position, CHARGING_STEP_SIZE)) {
          return false;
        }
        continue;
      }

      // Don't drive beyond the remaining segment time or end of th
      // current race day
      double available_drive_time = remaining_time;

      if (race_day_one) {
        available_drive_time =
            std::min(available_drive_time, day_one_end_time - curr_time);
      } else {
        available_drive_time =
            std::min(available_drive_time, day_end_time - curr_time);
      }

      // Convert available_drive_time to its corresponding distance and altitude
      // fractions
      double distance_fraction = available_drive_time / travel_time_seconds;

      double step_distance_m = distance_m * distance_fraction;
      double step_delta_altitude_m = delta_altitude_m * distance_fraction;

      // Use midpoint of this driving interval for DNI
      const Coord step_midpoint =
          interpolate_position(completed_fraction + distance_fraction / 2.0);
      ForecastCoord forecast_coord(step_midpoint.lat, step_midpoint.lon);
      time_t utc_time = curr_time.get_utc_time_point();
      double dni = forecast_lut.get_value(forecast_coord, utc_time);

      // Convert DNI to solar energy in kwh
      double solar_power_w = car->get_solar_power(dni);
      double solar_energy_kwh =
          (solar_power_w * available_drive_time) / JOULES_TO_KWH;

      // Calculate energy needed to drive this segment
      double energy_used_kwh = car->get_energy_consumption(
          speed_kph, step_distance_m, step_delta_altitude_m,
          available_drive_time);

      battery_kwh += solar_energy_kwh;
      battery_kwh -= energy_used_kwh;

      if (battery_kwh < 0.0) {
        if (strategy != nullptr) {
          std::cout << "Strategy failed: battery depleted near "
                    << distance_travelled_m / KM_TO_M << " km." << std::endl;
        }
        return false;
      }
      // Account for "overfilling" of battery
      battery_kwh = std::min(battery_kwh, car->get_battery_capacity());
      // Keep the lowest observed charge for final report
      if (battery_kwh < minimum_battery_kwh) {
        minimum_battery_kwh = battery_kwh;
        minimum_battery_distance_m = distance_travelled_m + step_distance_m;
      }

      // Advance the clock by the same interval used for the distance and energy
      // calculations
      curr_time.update_time_seconds(available_drive_time);

      remaining_time -= available_drive_time;
    }

    distance_travelled_m += distance_m;
    distance_since_strategy_change_m += distance_m;
    remaining_distance_m -= distance_m;

    // Check if control point is reached, if it is, stop and charge
    if (control_stops.count(i + 1) > 0 &&
        !charge_while_stationary(next_point, control_stop_charge_time)) {
      return false;
    }
  }

  bool final_battery_target_met = true;
  if (strategy != nullptr && strategy->final_sprint) {
    const double final_battery_target_kwh =
        car->get_battery_capacity() * strategy->final_sprint_battery_percent /
        100.0;
    final_battery_target_met = battery_kwh >= final_battery_target_kwh;
  }
  const bool met_race_deadline = curr_time <= race_end_time;
  const bool viable = met_race_deadline && final_battery_target_met;

  if (strategy != nullptr) {
    std::cout << std::fixed << std::setprecision(2)
              << "\nSegment strategy summary\n"
              << "  Result: "
              << (viable               ? "viable"
                  : !met_race_deadline ? "missed race deadline"
                                       : "missed final battery target")
              << "\n"
              << "  Finish time: " << curr_time.get_local_readable_time()
              << "\n"
              << "  Final battery: " << battery_kwh << " kWh ("
              << battery_kwh / car->get_battery_capacity() * 100.0 << "%)\n"
              << "  Final battery target: "
              << (!strategy->final_sprint    ? "not enabled"
                  : final_battery_target_met ? "met"
                                             : "missed")
              << "\n"
              << "  Minimum battery: " << minimum_battery_kwh << " kWh near "
              << minimum_battery_distance_m / KM_TO_M << " km\n"
              << "  Strategy segments selected: "
              << current_strategy_segment_count << std::endl;
  }
  return viable;
}

Simulator::Simulator(std::shared_ptr<Car> model, const Coord starting_coord,
                     const Time starting_time)
    : starting_coord(starting_coord),
      curr_time(starting_time),
      starting_time(starting_time),
      car(model) {}
