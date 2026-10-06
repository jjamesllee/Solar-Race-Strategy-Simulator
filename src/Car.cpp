#include "Car.hpp"

#include <algorithm>
#include <cmath>

#include "Utils.hpp"

// Constructor
Car::Car() {}

// Calculates the battery energy required to travel one route segment
// Returns the energy consumed from the battery in kWh
double Car::get_energy_consumption(double speed_kph, double distance_m,
                                   double delta_altitude_m,
                                   double time_seconds) const {
  double speed_mps = kph2mps(speed_kph);

  // Aerodynamic drag: F = 0.5 * rho * CdA * v^2
  double drag_force = 0.5 * AIR_DENSITY_KG_M3 * CDA * speed_mps * speed_mps;
  double aerodynamic_energy = drag_force * distance_m;

  // Rolling resistance: F = Crr * m * g
  double rolling_force = ROLLING_RESISTANCE * MASS_KG * GRAVITY_ACCELERATION;
  double rolling_energy = rolling_force * distance_m;

  // Gravitational potential energy: E = m * g * delta_h
  double gravitational_energy =
      MASS_KG * GRAVITY_ACCELERATION * delta_altitude_m;

  double mechanical_energy =
      aerodynamic_energy + rolling_energy + gravitational_energy;

  // No regen braking, downhill can't generate energy
  mechanical_energy = std::max(0.0, mechanical_energy);

  // Motor and battery efficiency losses
  double electrical_energy = mechanical_energy / MOTOR_EFFICIENCY;
  electrical_energy /= BATTERY_EFFICIENCY;

  // Add car's continuous electrical load.
  double passive_energy = PASSIVE_POWER_W * time_seconds;
  electrical_energy += passive_energy;

  return electrical_energy / JOULES_TO_KWH;
}

// Calculates the electrical power produced by the solar array.
//
// dni: Direct Normal Irradiance in W/m^2
//
// Returns solar electrical power in watts.
double Car::get_solar_power(double dni) const {
  return dni * ARRAY_AREA_M2 * ARRAY_EFFICIENCY;
}

// Returns the maximum usable battery capacity in kWh.
double Car::get_battery_capacity() const { return MAX_BATTERY_KWH; }
