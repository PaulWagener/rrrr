#include "rrrr_types.h"
#include "tdata.h"

/* file-visible struct */
typedef struct tdata_header tdata_header_t;
struct tdata_header {
    /* Contents must read "TTABLEV3" */
    char version_string[8];
    uint64_t calendar_start_time;
    calendar_t dst_active;
    uint32_t n_stops;
    uint32_t n_stop_attributes;
    uint32_t n_stop_coords;
    uint32_t n_journey_patterns;
    uint32_t n_journey_pattern_points;
    uint32_t n_journey_pattern_point_attributes;
    uint32_t n_stop_times;
    uint32_t n_vjs;
    uint32_t n_journey_patterns_at_stop;
    uint32_t n_transfer_target_stops;
    uint32_t n_transfer_dist_meters;
    uint32_t n_vj_active;
    uint32_t n_journey_pattern_active;
    uint32_t n_platformcodes;
    /* length of the object in bytes */
    uint32_t n_stop_names;
    uint32_t n_stop_nameidx;
    uint32_t n_agency_ids;
    uint32_t n_agency_names;
    uint32_t n_agency_urls;


    /* length of the object in bytes */
    uint32_t n_headsigns;
    uint32_t n_line_codes;
    uint32_t n_productcategories;
    uint32_t n_line_ids;
    uint32_t n_stop_ids;
    uint32_t n_vj_ids;
    uint32_t loc_stops;
    uint32_t loc_stop_attributes;
    uint32_t loc_stop_coords;
    uint32_t loc_journey_patterns;
    uint32_t loc_journey_pattern_points;
    uint32_t loc_journey_pattern_point_attributes;
    uint32_t loc_stop_times;
    uint32_t loc_vjs;
    uint32_t loc_journey_patterns_at_stop;
    uint32_t loc_transfer_target_stops;
    uint32_t loc_transfer_dist_meters;
    uint32_t loc_vj_active;
    uint32_t loc_journey_pattern_active;
    uint32_t loc_platformcodes;
    uint32_t loc_stop_names;
    uint32_t loc_stop_nameidx;
    uint32_t loc_agency_ids;
    uint32_t loc_agency_names;
    uint32_t loc_agency_urls;
    uint32_t loc_headsigns;
    uint32_t loc_line_codes;
    uint32_t loc_productcategories;
    uint32_t loc_line_ids;
    uint32_t loc_stop_ids;
    uint32_t loc_vj_ids;
};

bool tdata_io_v3_load(tdata_t *td, char* filename);
void tdata_io_v3_close(tdata_t *td);
