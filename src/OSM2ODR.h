// Copyright (c) 2020 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include <string>
#include <vector>

namespace osm2odr {

  struct OSM2ODRSettings {
    bool use_offsets = false;
    double offset_x = 0;
    double offset_y = 0;
    double default_lane_width = 4.0;
    double elevation_layer_height = 0;
    std::string proj_string = "+proj=tmerc";
    bool center_map = true;
    bool generate_traffic_lights = true;
    bool all_junctions_traffic_lights = false;
    std::vector<std::string> osm_highways_types = 
        {"motorway", "motorway_link", "trunk", 
         "trunk_link", "primary", "primary_link", 
         "secondary", "secondary_link", "tertiary", 
         "tertiary_link", "unclassified", "residential"};
    std::vector<std::string> tl_excluded_highways_types = 
        {"motorway_link", "primary_link", "secondary_link",
         "tertiary_link"};
  };

  std::string ConvertOSMToOpenDRIVE(std::string osm_file, OSM2ODRSettings settings = OSM2ODRSettings());

} // namespace osm2odr
