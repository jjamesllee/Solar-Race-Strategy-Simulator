## Efficient interpolated weather lookup

Irradiance values are interpolated for coordinates and timestamps that are not present directly in "dni.csv". By caching the nearest weather-table row index from the previous lookup, the function can start its next location search from that row, significantly reducing simulation runtime. This is because the car moves forward along the route, the nearest weather-table row will be either before or after the previous one, eleminating the need to search all 1000+ weather locations again. Caching took 4 sec total whereas before it took 3 second per incriment in speed. 

## Configurable segment-based driving strategy

Three simulation modes: a whole-race constant-speed sweep, a strategy that changes speed roughly every 10 km, and a strategy that can change speed at every CSV route segment. The strategy modes tests speeds using upcoming route distance and elevation, current DNI, and battery reserve.

Settings are in "StrategyConfig" in "include/Sim.hpp". Settings only affect simulation decisions.

In option 2, the strategy chooses a new speed after approximately "segment_length_km" of travel (10 km default). In option 3, it uses the same speed-selection and race simulation logic, but chooses a new speed before each pair of points in "baseroute.csv". These CSV segments are not equal in length. Both options use the configured look-ahead distance when testing speeds.

Option 3 changes how often a decision is made, not how far ahead it looks.

The strategy settings include the route-segment switch, initial speed, segment and look-ahead distances, normal speed range and increment, battery reserve, and maximum speed change between decisions. The initial speed and maximum speed change prevent the first decision from jumping directly to an extreme speed. If no candidate meets the predicted reserve, the strategy uses a controlled fallback instead of immediately dropping to the minimum speed. These controls do not modify the car constants or official race rules.

## Optional final-sprint strategy and menu

After choosing simulation mode 2 or 3, a strategy-settings menu appears. Final sprint is disabled by default. Enter "final sprint" to toggle it on or off, the program prints the new state. Enter "start" to run the selected strategy.

When enabled, final sprint begins once the remaining route distance is below a value. Reaching it causes a immediate speed decision, even if previous distance interval has not been reached.

During the sprint, lookahead uses distance from current point to finish, and test speed up to the final sprint max speed kph, and uses final sprint battery precent as the expected finish reserve. Actual final battery is calculated after the route ends.

LIMITATIONS:

Since normal driving optimizes only over the configured look-ahead distance, it may not find the fastest whole-route strategy.
 - Segments beyond the normal look-ahead distance do not influence the current choice.
 - The predictor does not plan for known control-stop or overnight charging.
 - It sums positive elevation changes in the look-ahead window rather than modelling the exact order and steepness of hills.

The simulator uses one selected speed for each strategy interval and changes speed only at CSV route points. Candidate speeds are separated by "speed_step_kph", although the settings may use decimal values.

The decision predictor uses the current DNI value for its look-ahead estimate instead of sampling future DNI values along the predicted trip. Actual driving and charging still use the time- and location-based DNI lookup.

Final sprint starts at the first CSV route point inside the configured distance, not at an exact position partway through a CSV segment. Its battery estimate is approximate, so the final simulated battery may finish above or below the prediction; the actual final target is checked afterward.

The strategy does not model safety or handling limits.

The simulator does not model acceleration or braking time and energy; speed changes take effect at route-point boundaries.
