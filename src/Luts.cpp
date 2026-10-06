#include "Luts.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "date.h"

template <typename T>
BaseLut<T>::BaseLut(const std::filesystem::path path) {
  lut_path = path;
}

ForecastLut::ForecastLut(const std::string path)
    : BaseLut<double>(std::filesystem::path(path)) {
  std::cout << "Csv: " << lut_path.string() << std::endl;
  load_LUT();
}

void ForecastLut::load_LUT() {
  std::fstream file(lut_path);
  RUNTIME_EXCEPTION(file.is_open(),
                    "Forecast file not found " + lut_path.string());
  std::string times_line;
  file >> times_line;
  std::stringstream times_stream(times_line);

  // Remove 'latitude' and 'longitude' from first 2 cols of csv input.
  std::string time;
  std::getline(times_stream, time, ',');
  std::getline(times_stream, time, ',');

  /* Create an array of the time keys */
  while (!times_stream.eof()) {
    std::getline(times_stream, time, ',');
    RUNTIME_EXCEPTION(isDouble(time), "Time " + time +
                                          " is not a number in ForecastLUT " +
                                          lut_path.string());
    uint64_t temp_time = std::stoull(time);
    int seconds = temp_time % 100;
    temp_time /= 100;
    int minutes = temp_time % 100;
    temp_time /= 100;
    int hours = temp_time % 100;
    temp_time /= 100;
    int days = temp_time % 100;
    temp_time /= 100;
    int month = (temp_time % 100);
    temp_time /= 100;
    int year = temp_time;

    /* Construct YYYY-MM-DD HH:MM:SS string */
    std::string forecast_time =
        "20" + std::to_string(year) + "-" + std::to_string(month) + "-" +
        std::to_string(days) + " " + std::to_string(hours) + ":" +
        std::to_string(minutes) + ":" + std::to_string(seconds);

    std::istringstream iss(forecast_time);
    date::sys_time<std::chrono::seconds> epoch_time;
    iss >> date::parse("%F %T", epoch_time);
    time_t local_time_t = std::chrono::system_clock::to_time_t(epoch_time);

    forecast_times.push_back(local_time_t);
  }

  int row_counter = 0;
  while (!file.eof()) {
    std::string file_line;
    file >> file_line;
    std::stringstream file_linestream(file_line);
    if (file_linestream.str().empty()) break;

    std::string cell;
    ForecastCoord coord{};
    std::getline(file_linestream, cell, ',');
    RUNTIME_EXCEPTION(isDouble(cell), "Value " + cell +
                                          " is not a number in Forecast LUT " +
                                          lut_path.string());
    coord.lat = std::stod(cell);

    std::getline(file_linestream, cell, ',');
    RUNTIME_EXCEPTION(isDouble(cell),
                      "Value " + cell + " is not a number in Efficiency LUT " +
                          lut_path.string());
    coord.lon = std::stod(cell);

    forecast_coords.emplace_back(coord);

    std::getline(file_linestream, cell, ',');
    RUNTIME_EXCEPTION(isDouble(cell),
                      "Value " + cell + " is not a number in Efficiency LUT " +
                          lut_path.string());
    double value = std::stod(cell);
    std::vector<double> inner_vector;
    inner_vector.emplace_back(value);
    this->values.push_back(inner_vector);

    int column_counter = 0;
    while (!file_linestream.eof()) {
      std::getline(file_linestream, cell, ',');
      RUNTIME_EXCEPTION(isDouble(cell),
                        "Value " + cell +
                            " is not a number in Efficiency LUT " +
                            lut_path.string());
      this->values[row_counter].push_back(std::stod(cell));
      column_counter++;
    }

    row_counter++;
  }

  this->num_rows = forecast_coords.size();
  this->num_cols = forecast_times.size();

  row_cache = 0;
  column_cache = 0;
}

/*
Reimplmetned to include linear interpoleration for values.

Estimates the solar irradiance at a given location and UTC time using the FLU.
Finds 2 nearby wether locations on the table and the surrouning timestamps,
interpolates the irradience between them according to time and distance from/ to
car. Cashed location data makes lookups significantly faster

Lookups start from the row of last known location, if weather location at rows
ahead (based on location), advances by 1 row, else, back by 1 row, loops till we
get to nearest value. Essentally, takes the average irridance between 2 points,
plots a slope connecting 2 points, put my point on it.
*/
double ForecastLut::get_value(ForecastCoord coord, time_t time) {
  RUNTIME_EXCEPTION(num_rows > 0 && num_cols > 0,
                    "Forecast LUT is empty " + lut_path.string());

  // Weather locations follows the route in order. Move the cached location to
  // the nearest row instead of scanning the full table for every lookup
  auto distance_to_row = [&](size_t row) {
    // Calculate the  distance from current coordinate to a given
    // weather-table coordinate
    return get_forecast_coord_distance(coord, forecast_coords[row]);
  };

  // Advance the cache while the next row is closer to the requested point
  while (row_cache + 1 < static_cast<int>(num_rows) &&
         distance_to_row(row_cache + 1) < distance_to_row(row_cache)) {
    ++row_cache;
  }

  // Move backwards if this lookup is behind the currently cached row
  while (row_cache > 0 &&
         distance_to_row(row_cache - 1) < distance_to_row(row_cache)) {
    --row_cache;
  }

  // The cached row is the closest weather location
  const size_t first_row = static_cast<size_t>(row_cache);
  size_t second_row = first_row;

  // Select a second nearby row so irradiance can be interpolated by location.
  if (num_rows > 1) {
    if (first_row == 0) {
      second_row = 1;
    } else if (first_row + 1 == num_rows) {
      second_row = first_row - 1;
    } else {
      // Choose whatever location is closer, then orders which is 1st, which is
      // 2nd
      const double previous_distance = distance_to_row(first_row - 1);
      const double next_distance = distance_to_row(first_row + 1);
      second_row =
          previous_distance <= next_distance ? first_row - 1 : first_row + 1;
    }
  }

  // Find the first forecast timestamp thats >= to the requested time
  const auto upper_time =
      std::lower_bound(forecast_times.begin(), forecast_times.end(), time);

  // Convert timestamp loc to column index
  size_t upper_col = static_cast<size_t>(upper_time - forecast_times.begin());
  size_t lower_col = upper_col;

  // Boundary case: times outside the table range changed to the first or last
  // available value
  if (upper_col == num_cols) {
    lower_col = upper_col = num_cols - 1;
  } else if (upper_col > 0 && forecast_times[upper_col] != time) {
    // If time between two columns, use the lower column as
    // lower timestamp and the lower_bound result as the upper timestamp
    lower_col = upper_col - 1;
  }

  // Linearly interpolate the DNI value at a location between the two time
  // columns above
  auto interpolate_time = [&](size_t row) {
    if (lower_col == upper_col) {
      // If there is no surrounding timestamp on one side, so use the exact
      // table value
      return values[row][lower_col];
    }

    // Calculate how far the requested time is  between the two timestamps
    const double interval = static_cast<double>(forecast_times[upper_col] -
                                                forecast_times[lower_col]);
    const double fraction =
        static_cast<double>(time - forecast_times[lower_col]) / interval;

    // Starting at lower value, move calculated fraction toward upper value to
    // solve for interpolated DNI value
    return values[row][lower_col] +
           fraction * (values[row][upper_col] - values[row][lower_col]);
  };

  // Estimate DNI at the car's current time for the closest weather location.
  const double first_value = interpolate_time(first_row);
  if (first_row == second_row) {
    // If two rows are same, just return first row value.
    return first_value;
  }

  // Calculate distance from car to closest weather location
  const double first_distance = distance_to_row(first_row);
  if (first_distance == 0.0) {
    // if right on location, return it
    return first_value;
  }

  // Calculate distance from car to 2nd weather location
  const double second_distance = distance_to_row(second_row);
  const double distance_sum = first_distance + second_distance;
  if (distance_sum == 0.0) {
    // Boundary case for division by 0
    return first_value;
  }

  // Estimate DNI at the car's current time for the second weather location.
  const double second_value = interpolate_time(second_row);

  // Weighted linear interpoleration result
  return (first_value * second_distance + second_value * first_distance) /
         distance_sum;
}

void ForecastLut::initialize_caches(ForecastCoord coord, time_t time) {
  /* Initialize row cache */
  Coord forecast_coord_as_coord = Coord(coord);
  double min_distance = std::numeric_limits<double>::max();
  for (size_t i = 0; i < num_rows; i++) {
    Coord forecast_coord = Coord(forecast_coords[i]);
    double distance = get_distance(forecast_coord, forecast_coord_as_coord);
    if (distance < min_distance) {
      min_distance = distance;
      row_cache = i;
    }
  }

  /* Initialize column cache */
  double min_time = std::numeric_limits<double>::max();
  for (size_t i = 0; i < num_cols; i++) {
    time_t forecast_time = forecast_times[i];
    int time_diff = time - forecast_time;
    if (std::abs(static_cast<double>(time_diff)) < min_time) {
      min_time = std::abs(static_cast<double>(time_diff));
      column_cache = i;
    }
  }
}

void ForecastLut::initialize_caches(Coord coord, time_t time) {
  /* Initialize row cache */
  double min_distance = std::numeric_limits<double>::max();
  for (size_t i = 0; i < num_rows; i++) {
    Coord forecast_coord = Coord(forecast_coords[i]);
    double distance = get_distance(forecast_coord, coord);
    if (distance < min_distance) {
      min_distance = distance;
      row_cache = i;
    }
  }

  /* Initialize column cache */
  double min_time = std::numeric_limits<double>::max();
  for (size_t i = 0; i < num_cols; i++) {
    time_t forecast_time = forecast_times[i];
    int time_diff = time - forecast_time;
    if (std::abs(static_cast<double>(time_diff)) < min_time) {
      min_time = std::abs(static_cast<double>(time_diff));
      column_cache = i;
    }
  }
}

/* Begins searching from the specified indices */
void ForecastLut::update_index_cache(ForecastCoord coord, time_t time) {
  if (row_cache < num_rows - 1) {
    ForecastCoord next_coord = forecast_coords[row_cache + 1];
    ForecastCoord current_coord = forecast_coords[row_cache];

    double dist_from_current_coord =
        get_forecast_coord_distance(coord, current_coord);
    double dist_from_next_coord =
        get_forecast_coord_distance(coord, next_coord);

    row_cache = dist_from_current_coord <= dist_from_next_coord ? row_cache
                                                                : row_cache + 1;
  }
  if (column_cache < num_cols - 1) {
    uint64_t current_time = forecast_times[column_cache];
    uint64_t next_time = forecast_times[column_cache + 1];

    uint64_t diff_time_from_current =
        abs(static_cast<double>(time - current_time));
    uint64_t diff_time_from_next = abs(static_cast<double>(time - next_time));

    column_cache = diff_time_from_current <= diff_time_from_next
                       ? column_cache
                       : column_cache + 1;
  }
}

double ForecastLut::get_value_with_cache() {
  return this->values[row_cache][column_cache];
}

Route::Route(const std::string lut_path) {
  const std::filesystem::path route_path(lut_path);
  std::fstream base_route(route_path);
  RUNTIME_EXCEPTION(base_route.is_open(),
                    "Base route file not found " + route_path.string());

  Coord last_coord;

  // Read and parse the file
  while (!base_route.eof()) {
    std::string line;
    base_route >> line;
    std::stringstream linestream(line);

    while (!linestream.eof() && !linestream.str().empty()) {
      std::string cell;
      Coord coord{};

      std::getline(linestream, cell, ',');
      RUNTIME_EXCEPTION(isDouble(cell), "Value " + cell + " in route file " +
                                            route_path.string() +
                                            " is not a number");
      coord.lat = std::stod(cell);

      std::getline(linestream, cell, ',');
      RUNTIME_EXCEPTION(isDouble(cell), "Value " + cell + " in route file " +
                                            route_path.string() +
                                            " is not a number");
      coord.lon = std::stod(cell);

      std::getline(linestream, cell, ',');
      RUNTIME_EXCEPTION(isDouble(cell), "Value " + cell + " in route file " +
                                            route_path.string() +
                                            " is not a number");
      coord.alt = std::stod(cell);

      route_points.emplace_back(coord);
    }
  }
}
