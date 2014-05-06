/* Copyright 2013 Bliksem Labs. See the LICENSE file at the top-level directory of this distribution and at https://github.com/bliksemlabs/rrrr/. */

/* tdata.c : handles memory mapped data file containing transit timetable etc. */

#include "tdata.h" // make sure it works alone

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#include "config.h"
#include "bitset.h"
#include "util.h"
#include "radixtree.h"
#include "gtfs-realtime.pb-c.h"

// file-visible struct
typedef struct tdata_header tdata_header_t;
struct tdata_header {
    char version_string[8]; // should read "TTABLEV3"
    uint64_t calendar_start_time;
    calendar_t dst_active;

    uint32_t n_stops;
    uint32_t n_stop_attributes;
    uint32_t n_stop_coords;
    uint32_t n_routes;
    uint32_t n_route_stops;
    uint32_t n_route_stop_attributes;
    uint32_t n_stop_times;
    uint32_t n_trips;
    uint32_t n_stop_routes;
    uint32_t n_transfer_target_stops;
    uint32_t n_transfer_dist_meters;
    uint32_t n_trip_active;
    uint32_t n_route_active;
    uint32_t n_platformcodes;
    uint32_t n_stop_names; /* length of the object in bytes */
    uint32_t n_stop_nameidx;
    uint32_t n_agency_ids;
    uint32_t n_agency_names;
    uint32_t n_agency_urls;
    uint32_t n_headsigns; /* length of the object in bytes */
    uint32_t n_route_shortnames;
    uint32_t n_productcategories;
    uint32_t n_route_ids;
    uint32_t n_stop_ids;
    uint32_t n_trip_ids;

    uint32_t loc_stops;
    uint32_t loc_stop_attributes;
    uint32_t loc_stop_coords;
    uint32_t loc_routes;
    uint32_t loc_route_stops;
    uint32_t loc_route_stop_attributes;
    uint32_t loc_stop_times;
    uint32_t loc_trips;
    uint32_t loc_stop_routes;
    uint32_t loc_transfer_target_stops;
    uint32_t loc_transfer_dist_meters;
    uint32_t loc_trip_active;
    uint32_t loc_route_active;
    uint32_t loc_platformcodes;
    uint32_t loc_stop_names;
    uint32_t loc_stop_nameidx;
    uint32_t loc_agency_ids;
    uint32_t loc_agency_names;
    uint32_t loc_agency_urls;
    uint32_t loc_headsigns;
    uint32_t loc_route_shortnames;
    uint32_t loc_productcategories;
    uint32_t loc_route_ids;
    uint32_t loc_stop_ids;
    uint32_t loc_trip_ids;
};

inline char *tdata_route_id_for_index(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    return td->route_ids + (td->route_ids_width * route_index);
}

inline char *tdata_stop_id_for_index(tdata_t *td, uint32_t stop_index) {
    return td->stop_ids + (td->stop_ids_width * stop_index);
}

inline uint8_t *tdata_stop_attributes_for_index(tdata_t *td, uint32_t stop_index) {
    return td->stop_attributes + stop_index;
}

inline char *tdata_trip_id_for_index(tdata_t *td, uint32_t trip_index) {
    return td->trip_ids + (td->trip_ids_width * trip_index);
}

inline char *tdata_trip_id_for_route_trip_index(tdata_t *td, uint32_t route_index, uint32_t trip_index) {
    return td->trip_ids + (td->trip_ids_width * (td->routes[route_index].trip_ids_offset + trip_index));
}

inline char *tdata_agency_id_for_index(tdata_t *td, uint32_t agency_index) {
    return td->agency_ids + (td->agency_ids_width * agency_index);
}

inline char *tdata_agency_name_for_index(tdata_t *td, uint32_t agency_index) {
    return td->agency_names + (td->agency_names_width * agency_index);
}

inline char *tdata_agency_url_for_index(tdata_t *td, uint32_t agency_index) {
    return td->agency_urls + (td->agency_urls_width * agency_index);
}

inline char *tdata_headsign_for_offset(tdata_t *td, uint32_t headsign_offset) {
    return td->headsigns + headsign_offset;
}

inline char *tdata_route_shortname_for_index(tdata_t *td, uint32_t route_shortname_index) {
    return td->route_shortnames + (td->route_shortnames_width * route_shortname_index);
}

inline char *tdata_productcategory_for_index(tdata_t *td, uint32_t productcategory_index) {
    return td->productcategories + (td->productcategories_width * productcategory_index);
}

inline char *tdata_stop_name_for_index(tdata_t *td, uint32_t stop_index) {
    switch (stop_index) {
    case NONE :
        return "NONE";
    case ONBOARD :
        return "ONBOARD";
    default :
        return td->stop_names + td->stop_nameidx[stop_index];
    }
}

inline char *tdata_platformcode_for_index(tdata_t *td, uint32_t stop_index) {
    switch (stop_index) {
    case NONE :
        return NULL;
    case ONBOARD :
        return NULL;
    default :
        return td->platformcodes + (td->platformcodes_width * stop_index);
    }
}

inline uint32_t tdata_stopidx_by_stop_name(tdata_t *td, char* stop_desc, uint32_t start_index) {
    for (uint32_t stop_index = start_index; stop_index < td->n_stops; stop_index++) {
        if (strcasestr(td->stop_names + td->stop_nameidx[stop_index], stop_desc)) {
            return stop_index;
        }
    }
    return NONE;
}

inline uint32_t tdata_stopidx_by_stop_id(tdata_t *td, char* stop_id, uint32_t start_index) {
    for (uint32_t stop_index = start_index; stop_index < td->n_stops; stop_index++) {
        if (strcasestr(td->stop_ids + (td->stop_ids_width * stop_index), stop_id)) {
            return stop_index;
        }
    }
    return NONE;
}

inline uint32_t tdata_routeidx_by_route_id(tdata_t *td, char* route_id, uint32_t start_index) {
    for (uint32_t route_index = start_index; route_index < td->n_routes; route_index++) {
        if (strcasestr(td->route_ids + (td->route_ids_width * route_index), route_id)) {
            return route_index;
        }
    }
    return NONE;
}

inline char *tdata_trip_ids_for_route(tdata_t *td, uint32_t route_index) {
    route_t route = (td->routes)[route_index];
    uint32_t char_offset = route.trip_ids_offset * td->trip_ids_width;
    return td->trip_ids + char_offset;
}

inline calendar_t *tdata_trip_masks_for_route(tdata_t *td, uint32_t route_index) {
    route_t route = (td->routes)[route_index];
    return td->trip_active + route.trip_ids_offset;
}

inline char *tdata_headsign_for_route(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    route_t route = (td->routes)[route_index];
    return td->headsigns + route.headsign_offset;
}

inline char *tdata_shortname_for_route(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    route_t route = (td->routes)[route_index];
    return td->route_shortnames + (td->route_shortnames_width * route.shortname_index);
}

inline char *tdata_productcategory_for_route(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    route_t route = (td->routes)[route_index];
    return td->productcategories + (td->productcategories_width * route.productcategory_index);
}

inline uint32_t tdata_agencyidx_by_agency_name(tdata_t *td, char* agency_name, uint32_t start_index) {
    for (uint32_t agency_index = start_index; agency_index < td->n_agency_names; agency_index++) {
        if (strcasestr(td->agency_names + (td->agency_names_width * agency_index), agency_name)) {
            return agency_index;
        }
    }
    return NONE;
}

inline char *tdata_agency_id_for_route(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    route_t route = (td->routes)[route_index];
    return td->agency_ids + (td->agency_ids_width * route.agency_index);
}

inline char *tdata_agency_name_for_route(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    route_t route = (td->routes)[route_index];
    return td->agency_names + (td->agency_names_width * route.agency_index);
}

inline char *tdata_agency_url_for_route(tdata_t *td, uint32_t route_index) {
    if (route_index == NONE) return "NONE";
    route_t route = (td->routes)[route_index];
    return td->agency_urls + (td->agency_urls_width * route.agency_index);
}

void tdata_check_coherent (tdata_t *tdata) {
    printf ("checking tdata coherency...\n");
    /* Check that all lat/lon look like valid coordinates. */
    float min_lat = -55.0; // farther south than Ushuaia, Argentina
    float max_lat = +70.0; // farther north than Tromsø and Murmansk
    float min_lon = -180.0;
    float max_lon = +180.0;
    for (uint32_t s = 0; s < tdata->n_stops; ++s) {
        latlon_t ll = tdata->stop_coords[s];
        if (ll.lat < min_lat || ll.lat > max_lat || ll.lon < min_lon || ll.lon > max_lon) {
            printf ("stop lat/lon out of range: lat=%f, lon=%f \n", ll.lat, ll.lon);
        }
    }
    /* Check that all timedemand types start at 0 and consist of monotonically increasing times. */
    for (uint32_t r = 0; r < tdata->n_routes; ++r) {
        route_t route = tdata->routes[r];
        trip_t *trips = tdata->trips + route.trip_ids_offset;
        int n_nonincreasing_trips = 0;
        for (int t = 0; t < route.n_trips; ++t) {
            trip_t trip = trips[t];
            stoptime_t *prev_st = NULL;
            for (int s = 0; s < route.n_stops; ++s) {
                stoptime_t *st = tdata->stop_times + trip.stop_times_offset + s;
                if (s == 0 && st->arrival != 0) printf ("timedemand type begins at %d,%d not 0.\n", st->arrival, st->departure);
                if (st->departure < st->arrival) printf ("departure before arrival at route %d, trip %d, stop %d.\n", r, t, s);
                if (prev_st != NULL) {
                    if (st->arrival < prev_st->departure) {
                        // printf ("negative travel time arriving at route %d, trip %d, stop %d.\n", r, t, s);
                        // printf ("(%d, %d) -> (%d, %d)\n", prev_st->arrival, prev_st->departure, st->arrival, st->departure);
                        n_nonincreasing_trips += 1;
                    } // there are also lots of 0 travel times...
                }
                prev_st = st;
            }
        }
        if (n_nonincreasing_trips > 0) printf ("route %d has %d trips with negative travel times\n", r, n_nonincreasing_trips);
    }
    /* Check that all transfers are symmetric. */
    int n_transfers_checked = 0;
    for (uint32_t stop_index_from = 0; stop_index_from < tdata->n_stops; ++stop_index_from) {
        /* Iterate over all transfers going out of this stop */
        uint32_t t  = tdata->stops[stop_index_from    ].transfers_offset;
        uint32_t tN = tdata->stops[stop_index_from + 1].transfers_offset;
        for ( ; t < tN ; ++t) {
            uint32_t stop_index_to = tdata->transfer_target_stops[t];
            uint32_t forward_distance = tdata->transfer_dist_meters[t] << 4; // actually in units of 16 meters
            if (stop_index_to == stop_index_from) printf ("loop transfer from/to stop %d.\n", stop_index_from);
            /* Find the reverse transfer (stop_index_to -> stop_index_from) */
            bool found_reverse = false;
            uint32_t u  = tdata->stops[stop_index_to    ].transfers_offset;
            uint32_t uN = tdata->stops[stop_index_to + 1].transfers_offset;
            for ( ; u < uN ; ++u) {
                n_transfers_checked += 1;
                if (tdata->transfer_target_stops[u] == stop_index_from) {
                    /* this is the same transfer in reverse */
                    uint32_t reverse_distance = tdata->transfer_dist_meters[u] << 4;
                    if (reverse_distance != forward_distance) {
                        printf ("transfer from %d to %d is not symmetric. "
                                "forward distance is %d, reverse distance is %d.\n",
                                stop_index_from, stop_index_to, forward_distance, reverse_distance);
                    }
                    found_reverse = true;
                    break;
                }
            }
            if ( ! found_reverse) printf ("transfer from %d to %d does not have an equivalent reverse transfer.\n", stop_index_from, stop_index_to);
        }
    }
    printf ("checked %d transfers for symmetry.\n", n_transfers_checked);
}

#ifdef RRRR_REALTIME_EXPANDED
void tdata_alloc_expanded(tdata_t *td) {
    td->trip_stoptimes = (stoptime_t **) calloc(td->n_trips, sizeof(stoptime_t *));
    td->trip_routes = (uint32_t *) malloc(td->n_trips * sizeof(uint32_t));
    for (uint32_t r = 0; r < td->n_routes; ++r) {
        for (uint32_t t = 0; t < td->routes[r].n_trips; ++t) {
            td->trip_routes[td->routes[r].trip_ids_offset + t] = r;
        }
    }

    td->rt_stop_routes = (list_t **) calloc(td->n_stops, sizeof(list_t *));
}

void tdata_free_expanded(tdata_t *td) {
    free (td->trip_routes);

    for (uint32_t t = 0; t < td->n_trips; ++t) {
        free (td->trip_stoptimes[t]);
    }

    free (td->trip_stoptimes);

    for (uint32_t s = 0; s < td->n_stops; ++s) {
        if (td->rt_stop_routes[s]) {
            free (td->rt_stop_routes[s]->list);
        }
    }

    free (td->rt_stop_routes);
}
#endif

void tdata_load_dynamic(char *filename, tdata_t *td) {
    tdata_header_t h;
    tdata_header_t *header = &h;

    int fd = open(filename, O_RDONLY);
    if (fd == -1)
        die("could not find input file");

    read (fd, header, sizeof(*header));

    td->base = NULL;
    td->size = 0;

    if( strncmp("TTABLEV3", header->version_string, 8) )
        die("the input file does not appear to be a timetable or is of the wrong version");

    td->calendar_start_time = header->calendar_start_time;
    td->dst_active = header->dst_active;

    load_dynamic (fd, stops, stop_t);
    load_dynamic (fd, stop_attributes, uint8_t);
    load_dynamic (fd, stop_coords, latlon_t);
    load_dynamic (fd, routes, route_t);
    load_dynamic (fd, route_stops, uint32_t);
    load_dynamic (fd, route_stop_attributes, uint8_t);
    load_dynamic (fd, stop_times, stoptime_t);
    load_dynamic (fd, trips, trip_t);
    load_dynamic (fd, stop_routes, uint32_t);
    load_dynamic (fd, transfer_target_stops, uint32_t);
    load_dynamic (fd, transfer_dist_meters, uint8_t);
    load_dynamic (fd, trip_active, calendar_t);
    load_dynamic (fd, route_active, calendar_t);
    load_dynamic (fd, headsigns, char);
    load_dynamic (fd, stop_names, char);
    load_dynamic (fd, stop_nameidx, uint32_t);

    load_dynamic_string (fd, platformcodes);
    load_dynamic_string (fd, stop_ids);
    load_dynamic_string (fd, trip_ids);
    load_dynamic_string (fd, agency_ids);
    load_dynamic_string (fd, agency_names);
    load_dynamic_string (fd, agency_urls);
    load_dynamic_string (fd, route_shortnames);
    load_dynamic_string (fd, route_ids);
    load_dynamic_string (fd, productcategories);

    #ifdef RRRR_REALTIME_EXPANDED
    tdata_alloc_expanded(td);
    #endif

    td->alerts = NULL;
}

/* Map an input file into memory and reconstruct pointers to its contents. */
void tdata_load(char *filename, tdata_t *td) {

    int fd = open(filename, O_RDWR);
    if (fd == -1)
        die("could not find input file");

    struct stat st;
    if (stat(filename, &st) == -1)
        die("could not stat input file");

    td->base = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    td->size = st.st_size;
    if (td->base == (void*)(-1))
        die("could not map input file");

    void *b = td->base;
    tdata_header_t *header = b;
    if( strncmp("TTABLEV3", header->version_string, 8) )
        die("the input file does not appear to be a timetable or is of the wrong version");
    td->calendar_start_time = header->calendar_start_time;
    td->dst_active = header->dst_active;

    load_mmap (b, stops, stop_t);
    load_mmap (b, stop_attributes, uint8_t);
    load_mmap (b, stop_coords, latlon_t);
    load_mmap (b, routes, route_t);
    load_mmap (b, route_stops, uint32_t);
    load_mmap (b, route_stop_attributes, uint8_t);
    load_mmap (b, stop_times, stoptime_t);
    load_mmap (b, trips, trip_t);
    load_mmap (b, stop_routes, uint32_t);
    load_mmap (b, transfer_target_stops, uint32_t);
    load_mmap (b, transfer_dist_meters, uint8_t);
    load_mmap (b, trip_active, calendar_t);
    load_mmap (b, route_active, calendar_t);
    load_mmap (b, headsigns, char);
    load_mmap (b, stop_names, char);
    load_mmap (b, stop_nameidx, uint32_t);

    load_mmap_string (b, platformcodes);
    load_mmap_string (b, stop_ids);
    load_mmap_string (b, trip_ids);
    load_mmap_string (b, agency_ids);
    load_mmap_string (b, agency_names);
    load_mmap_string (b, agency_urls);
    load_mmap_string (b, route_shortnames);
    load_mmap_string (b, route_ids);
    load_mmap_string (b, productcategories);

    #ifdef RRRR_REALTIME_EXPANDED
    tdata_alloc_expanded(td);
    #endif

    td->alerts = NULL;

    // This is probably a bit slow and is not strictly necessary, but does page in all the timetable entries.
    tdata_check_coherent(td);
    D tdata_dump(td);
}

void tdata_close_dynamic(tdata_t *td) {
    free (td->stops);
    free (td->stop_attributes);
    free (td->stop_coords);
    free (td->routes);
    free (td->route_stops);
    free (td->route_stop_attributes);
    free (td->stop_times);
    free (td->trips);
    free (td->stop_routes);
    free (td->transfer_target_stops);
    free (td->transfer_dist_meters);
    free (td->trip_active);
    free (td->route_active);
    free (td->headsigns);
    free (td->stop_names);
    free (td->stop_nameidx);

    free (td->platformcodes);
    free (td->stop_ids);
    free (td->trip_ids);
    free (td->agency_ids);
    free (td->agency_names);
    free (td->agency_urls);
    free (td->route_shortnames);
    free (td->route_ids);
    free (td->productcategories);

    #ifdef RRRR_REALTIME_EXPANDED
    tdata_free_expanded(td);
    #endif
}

void tdata_close(tdata_t *td) {
    munmap(td->base, td->size);

    #ifdef RRRR_REALTIME_EXPANDED
    tdata_free_expanded(td);
    #endif
}

// TODO should pass pointer to tdata?
inline uint32_t *tdata_stops_for_route(tdata_t *td, uint32_t route) {
    route_t route0 = td->routes[route];
    return td->route_stops + route0.route_stops_offset;
}

inline uint8_t *tdata_stop_attributes_for_route(tdata_t *td, uint32_t route) {
    route_t route0 = td->routes[route];
    return td->route_stop_attributes + route0.route_stops_offset;
}

inline uint32_t tdata_routes_for_stop(tdata_t *td, uint32_t stop, uint32_t **routes_ret) {
    stop_t stop0 = td->stops[stop];
    stop_t stop1 = td->stops[stop + 1];
    *routes_ret = td->stop_routes + stop0.stop_routes_offset;
    return stop1.stop_routes_offset - stop0.stop_routes_offset;
}

// TODO used only in dumping routes; trip_index is not used in the expression?
inline stoptime_t *tdata_timedemand_type(tdata_t *td, uint32_t route_index, uint32_t trip_index) {
    return td->stop_times + td->trips[td->routes[route_index].trip_ids_offset + trip_index].stop_times_offset;
}

inline trip_t *tdata_trips_for_route (tdata_t *td, uint32_t route_index) {
    return td->trips + td->routes[route_index].trip_ids_offset;
}

void tdata_dump_route(tdata_t *td, uint32_t route_idx, uint32_t trip_idx) {
    uint32_t *stops = tdata_stops_for_route(td, route_idx);
    route_t route = td->routes[route_idx];
    printf("\nRoute details for %s %s %s '%s %s' [%d] (n_stops %d, n_trips %d)\n", tdata_agency_name_for_route(td, route_idx),
        tdata_agency_id_for_route(td, route_idx), tdata_agency_url_for_route(td, route_idx),
        tdata_shortname_for_route(td, route_idx), tdata_headsign_for_route(td, route_idx), route_idx, route.n_stops, route.n_trips);
    printf("tripid, stop sequence, stop name (index), departures  \n");
    for (uint32_t ti = (trip_idx == NONE ? 0 : trip_idx); ti < (trip_idx == NONE ? route.n_trips : trip_idx + 1); ++ti) {
        // TODO should this really be a 2D array ?
        stoptime_t (*times)[route.n_stops] = (void*) tdata_timedemand_type(td, route_idx, ti);

        printf("%s\n", tdata_trip_id_for_index(td, route.trip_ids_offset + ti));
        for (uint32_t si = 0; si < route.n_stops; ++si) {
            char *stop_id = tdata_stop_name_for_index (td, stops[si]);
            printf("%4d %35s [%06d] : %s %s", si, stop_id, stops[si], timetext(times[0][si].arrival + td->trips[route.trip_ids_offset + ti].begin_time + RTIME_ONE_DAY), timetext(times[0][si].departure + td->trips[route.trip_ids_offset + ti].begin_time + RTIME_ONE_DAY));

            #ifdef RRRR_REALTIME_EXPANDED
            if (td->trip_stoptimes && td->trip_stoptimes[route.trip_ids_offset + ti]) {
                printf (" %s %s", timetext(td->trip_stoptimes[route.trip_ids_offset + ti][si].arrival + RTIME_ONE_DAY), timetext(td->trip_stoptimes[route.trip_ids_offset + ti][si].departure + RTIME_ONE_DAY));
            }
            #endif

             printf("\n");
         }
         printf("\n");
    }
    printf("\n");
}

void tdata_dump(tdata_t *td) {
    printf("\nCONTEXT\n"
           "n_stops: %d\n"
           "n_routes: %d\n", td->n_stops, td->n_routes);
    printf("\nSTOPS\n");
    for (uint32_t i = 0; i < td->n_stops; i++) {
        printf("stop %d at lat %f lon %f\n", i, td->stop_coords[i].lat, td->stop_coords[i].lon);
        stop_t s0 = td->stops[i];
        stop_t s1 = td->stops[i+1];
        uint32_t j0 = s0.stop_routes_offset;
        uint32_t j1 = s1.stop_routes_offset;
        uint32_t j;
        printf("served by routes ");
        for (j=j0; j<j1; ++j) {
            printf("%d ", td->stop_routes[j]);
        }
        printf("\n");
    }
    printf("\nROUTES\n");
    for (uint32_t i = 0; i < td->n_routes; i++) {
        printf("route %d\n", i);
        printf("having trips %d\n", td->routes[i].n_trips);
        route_t r0 = td->routes[i];
        route_t r1 = td->routes[i+1];
        uint32_t j0 = r0.route_stops_offset;
        uint32_t j1 = r1.route_stops_offset;
        uint32_t j;
        printf("serves stops ");
        for (j=j0; j<j1; ++j) {
            printf("%d ", td->route_stops[j]);
        }
        printf("\n");
    }
    printf("\nSTOPIDS\n");
    for (uint32_t i = 0; i < td->n_stops; i++) {
        printf("stop %03d has id %s \n", i, tdata_stop_name_for_index(td, i));
    }
    for (uint32_t i = 0; i < td->n_routes; i++) {
        // printf("route %03d has id %s and first trip id %s \n", i,
        //    tdata_route_desc_for_index(td, i),
        //    tdata_trip_ids_for_route(td, i));
        tdata_dump_route(td, i, NONE);
    }
}

#ifdef RRRR_REALTIME

static inline
void tdata_apply_gtfsrt_time (TransitRealtime__TripUpdate__StopTimeUpdate *update, stoptime_t *stoptime) {
    if (update->arrival) {
        if (update->arrival->has_time) {
            stoptime->arrival  = epoch_to_rtime ((time_t) update->arrival->time, NULL) - RTIME_ONE_DAY;
        } else if (update->arrival->has_delay) {
            stoptime->arrival += SEC_TO_RTIME(update->arrival->delay);
        }
    }

    if (update->departure) {
        if (update->departure->has_time) {
            stoptime->departure  = epoch_to_rtime ((time_t) update->departure->time, NULL) - RTIME_ONE_DAY;
        } else if (update->departure->has_delay) {
            stoptime->departure += SEC_TO_RTIME(update->departure->delay);
        }
    }
}

static inline
void realtime_free_tripidx (tdata_t *tdata, uint32_t trip_index) {
    if (tdata->trip_stoptimes[trip_index]) {
        free(tdata->trip_stoptimes[trip_index]);
        tdata->trip_stoptimes[trip_index] = NULL;
        /* TODO: also free a forked route and the reference to it */
        /* TODO: restore original validity */
    }
}

/* rt_stop_routes store the delta to the planned stop_routes */
static inline
void tdata_rt_stop_routes_append (tdata_t *tdata, uint32_t stop_index, uint32_t route_index) {
    if (tdata->rt_stop_routes[stop_index]) {
        for (uint32_t i = 0; i < tdata->rt_stop_routes[stop_index]->len; ++i) {
            if (((uint32_t *) tdata->rt_stop_routes[stop_index]->list)[i] == route_index) return;
        }
    } else {
        tdata->rt_stop_routes[stop_index] = (list_t *) calloc(1, sizeof(list_t));
    }

    if (tdata->rt_stop_routes[stop_index]->len == tdata->rt_stop_routes[stop_index]->size) {
        tdata->rt_stop_routes[stop_index]->list = realloc(tdata->rt_stop_routes[stop_index]->list, (tdata->rt_stop_routes[stop_index]->size + 8) * sizeof(uint32_t));
        tdata->rt_stop_routes[stop_index]->size += 8;
    }

    ((uint32_t *) tdata->rt_stop_routes[stop_index]->list)[tdata->rt_stop_routes[stop_index]->len++] = route_index;
}

static inline
void tdata_rt_stop_routes_remove (tdata_t *tdata, uint32_t stop_index, uint32_t route_index) {
    for (uint32_t i = 0; i < tdata->rt_stop_routes[stop_index]->len; ++i) {
        if (((uint32_t *) tdata->rt_stop_routes[stop_index]->list)[i] == route_index) {
            tdata->rt_stop_routes[stop_index]->len--;
            ((uint32_t *) tdata->rt_stop_routes[stop_index]->list)[i] = ((uint32_t *) tdata->rt_stop_routes[stop_index]->list)[tdata->rt_stop_routes[stop_index]->len];
            return;
        }
    }
}

static inline
uint16_t tdata_route_new(tdata_t *tdata, char *trip_id, uint16_t n_stops, uint16_t n_trips, uint16_t attributes, uint32_t headsign_offset, uint16_t agency_index, uint16_t shortname_index, uint16_t productcategory_index) {
    route_t *new = &tdata->routes[tdata->n_routes];
    new->route_stops_offset = tdata->n_route_stops;
    new->trip_ids_offset = tdata->n_trips;
    new->headsign_offset = headsign_offset;
    new->n_stops = n_stops;
    new->n_trips = n_trips;
    new->attributes = attributes;
    new->agency_index = agency_index;
    new->productcategory_index = productcategory_index;

    rxt_insert(tdata->routeid_index, trip_id, tdata->n_routes);

    strncpy(&tdata->trip_ids[tdata->n_trips * tdata->trip_ids_width], trip_id, tdata->trip_ids_width);

    tdata->n_route_stops += n_stops;
    tdata->n_route_stop_attributes += n_stops;
    tdata->n_trips += n_trips;
    tdata->n_trip_ids += n_trips;
    tdata->n_trip_active += n_trips;
    tdata->n_route_active++;

    for (uint32_t i = 0; i < n_stops; i++) {
        tdata->route_stops[new->route_stops_offset + i] = NONE;
    }

    return tdata->n_routes++;
}


/*
  Decodes the GTFS-RT message of lenth len in buffer buf, extracting vehicle position messages
  and using the delay extension (1003) to update RRRR's per-trip delay information.
*/
void tdata_apply_gtfsrt (tdata_t *tdata, uint8_t *buf, size_t len) {
    TransitRealtime__FeedMessage *msg;
    msg = transit_realtime__feed_message__unpack (NULL, len, buf);
    if (msg == NULL) {
        fprintf (stderr, "error unpacking incoming gtfs-rt message\n");
        return;
    }
    printf("Received feed message with %zu entities.\n", msg->n_entity);
    for (size_t e = 0; e < msg->n_entity; ++e) {
        TransitRealtime__FeedEntity *rt_entity = msg->entity[e];
        if (rt_entity == NULL) goto cleanup;

        // printf("  entity %d has id %s\n", e, entity->id);
        TransitRealtime__TripUpdate *rt_trip_update = rt_entity->trip_update;
        #ifdef RRRR_REALTIME_EXPANDED
        if (rt_trip_update) {
            TransitRealtime__TripDescriptor *rt_trip = rt_trip_update->trip;
            if (rt_trip == NULL) continue;

            uint32_t trip_index = rxt_find (tdata->tripid_index, rt_trip->trip_id);
            if (trip_index == RADIX_TREE_NONE) {
                printf ("    trip id was not found in the radix tree.\n");
                continue;
            }

            if (rt_entity->is_deleted) {
                realtime_free_tripidx (tdata, trip_index);
                continue;
            }

            if (!rt_trip->start_date) {
                printf("WARNING: not handling realtime updates without a start date!\n");
                continue;
            }

            route_t *route = tdata->routes + tdata->trip_routes[trip_index];
            trip_t  *trip = tdata->trips + trip_index;

            /* Take care of the realtime validity */

            struct tm ltm;
            memset (&ltm, 0, sizeof(struct tm));
            strptime (rt_trip->start_date, "%Y%m%d", &ltm);
            ltm.tm_isdst = -1;
            time_t epochtime = mktime(&ltm);
            uint32_t cal_day;

            if (epochtime >= tdata->calendar_start_time) {
                cal_day = (epochtime - tdata->calendar_start_time) / SEC_IN_ONE_DAY;
                if (cal_day > 31 ) {
                    printf("WARNING: the operational day is 32 further than our calendar!\n");
                    continue;
                }
            }

            if (rt_trip->schedule_relationship == TRANSIT_REALTIME__TRIP_DESCRIPTOR__SCHEDULE_RELATIONSHIP__CANCELED) {
                /* Apply the cancel to the schedule */
                tdata->trip_active[trip_index] &= ~(1 << cal_day);

            } else if (rt_trip->schedule_relationship == TRANSIT_REALTIME__TRIP_DESCRIPTOR__SCHEDULE_RELATIONSHIP__SCHEDULED) {
                /* Mark in the schedule the trip is scheduled */
                tdata->trip_active[trip_index] |=  (1 << cal_day);

                if (rt_trip_update->n_stop_time_update) {
                    /* When the route changes, we have to fork a new route, and implicitly cancel this one */
                    bool changed_route = false;
                    bool nodata_route = false;
                    uint32_t n_stops = 0;

                    for (size_t stu = 0; stu < rt_trip_update->n_stop_time_update; ++stu) {
                        TransitRealtime__TripUpdate__StopTimeUpdate *rt_stop_time_update = rt_trip_update->stop_time_update[stu];

                        changed_route |= (rt_stop_time_update->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__ADDED ||
                                          rt_stop_time_update->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SKIPPED);
                        nodata_route  &= (rt_stop_time_update->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__NO_DATA);

                        n_stops += (rt_stop_time_update->schedule_relationship != TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SKIPPED && rt_stop_time_update->stop_id != NULL);
                    }

                    /* This entire route doesn't have any data */
                    if (nodata_route) {
                        /* If data previously was available, we should fall back to the schedule */
                        realtime_free_tripidx (tdata, trip_index);

                        /* In any case, we are done with processing this entity. */
                        continue;
                    }

                    /* If the trip has a different route, for example stops have been added or cancelled we must fork this trip into a new route */
                    if (changed_route) {

                        printf ("WARNING: this is a changed route!\n");

                        /* The idea is to fork a trip to a new route, based on the trip_id find if the trip_id already exists */
                        char *trip_id_new = (char *) alloca (tdata->trip_ids_width * sizeof(char));
                        trip_id_new[0] = '@';
                        strncpy(&trip_id_new[1], rt_trip->trip_id, tdata->trip_ids_width - 1);

                        uint32_t route_index = rxt_find (tdata->routeid_index, trip_id_new);
                        route_t *route_new = NULL;

                        if (route_index != RADIX_TREE_NONE) {
                            /* Fixes the case where a trip changes a second time */
                            route_new = &tdata->routes[route_index];
                            if (route_new->n_stops != n_stops) {
                                printf ("WARNING: this is a changed route being CHANGED again!\n");
                                tdata->trip_stoptimes[route_new->trip_ids_offset] = (stoptime_t *) realloc(tdata->trip_stoptimes[route_new->trip_ids_offset], n_stops * sizeof(stoptime_t));
                                for (uint32_t i = route_new->n_stops; i < n_stops; i++) {
                                    tdata->route_stops[route_new->route_stops_offset + i] = NONE;
                                }
                                route_new->n_stops = n_stops;
                            }
                        }

                        if (route_new == NULL) {
                            tdata->trip_active[trip_index] &= ~(1 << cal_day);

                            route_index = tdata_route_new(tdata, trip_id_new, n_stops, 1, route->attributes, route->headsign_offset, route->agency_index, route->shortname_index, route->productcategory_index);
                            route_new = &tdata->routes[route_index];
                            trip_index = route_new->trip_ids_offset;
                            tdata->trip_stoptimes[trip_index] = (stoptime_t *) malloc(route_new->n_stops * sizeof(stoptime_t));
                            tdata->trips[trip_index].stop_times_offset = tdata->n_stop_times;
                            for (uint16_t i = 0; i < route_new->n_stops; ++i) {
                                tdata->stop_times[tdata->trips[trip_index].stop_times_offset + i].arrival   = UNREACHED;
                                tdata->stop_times[tdata->trips[trip_index].stop_times_offset + i].departure = UNREACHED;
                            }

                            tdata->n_stop_times += route_new->n_stops;
                            tdata->trips[trip_index].begin_time = UNREACHED;
                            tdata->trips[trip_index].trip_attributes = trip->trip_attributes;
                            tdata->route_active[trip_index] |= (1 << cal_day);
                            tdata->trip_active[trip_index] |= (1 << cal_day);
                            tdata->trip_routes[trip_index] = route_index;
                        }

                        /* We will always overwrite the entire route, due to the problem where we have the same length, but different route */
                        uint16_t rs = 0;
                        for (size_t stu = 0; stu < rt_trip_update->n_stop_time_update; ++stu) {
                            TransitRealtime__TripUpdate__StopTimeUpdate *rt_stop_time_update = rt_trip_update->stop_time_update[stu];
                            if (rt_stop_time_update->schedule_relationship != TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SKIPPED) {
                                char *stop_id = rt_stop_time_update->stop_id;
                                if (stop_id) {
                                    uint32_t stop_index = rxt_find (tdata->stopid_index, stop_id);
                                    if (tdata->route_stops[route_new->route_stops_offset + rs] != NONE && tdata->route_stops[route_new->route_stops_offset + rs] != stop_index) {
                                        tdata_rt_stop_routes_remove (tdata, tdata->route_stops[route_new->route_stops_offset + rs], route_index);
                                    }
                                    /* TODO: Should this be communicated in GTFS-RT? */
                                    tdata->route_stop_attributes[route_new->route_stops_offset + rs] = (rsa_boarding | rsa_alighting);
                                    tdata->route_stops[route_new->route_stops_offset + rs] = stop_index;
                                    tdata_apply_gtfsrt_time (rt_stop_time_update, &tdata->trip_stoptimes[trip_index][rs]);
                                    tdata_rt_stop_routes_append (tdata, stop_index, route_index);
                                    rs++;
                                }
                            }
                        }

                        route_new->min_time = tdata->trip_stoptimes[trip_index][0].arrival;
                        route_new->max_time = tdata->trip_stoptimes[trip_index][route_new->n_stops - 1].departure;

                        tdata->route_stop_attributes[route_new->route_stops_offset] = rsa_boarding;
                        tdata->route_stop_attributes[route_new->route_stops_offset + (route_new->n_stops - 1)] = rsa_alighting;

                    } else {
                        /* Normal case: at least one SCHEDULED or some NO_DATA stops have been observed */
                        if (tdata->trip_stoptimes[trip_index] == NULL) {
                            /* If the expanded timetable does not contain an entry yet, we are creating one */
                            tdata->trip_stoptimes[trip_index] = (stoptime_t *) malloc(route->n_stops * sizeof(stoptime_t));
                        }

                        /* The initial time-demand based schedules */
                        stoptime_t *trip_times = tdata->stop_times + trip->stop_times_offset;

                        /* First re-initialise the old values from the schedule */
                        for (uint32_t rs = 0; rs < route->n_stops; ++rs) {
                            tdata->trip_stoptimes[trip_index][rs].arrival   = trip->begin_time + trip_times[rs].arrival;
                            tdata->trip_stoptimes[trip_index][rs].departure = trip->begin_time + trip_times[rs].departure;
                        }

                        TransitRealtime__TripUpdate__StopTimeUpdate *rt_stop_time_update_prev = NULL;

                        uint32_t rs = 0;
                        for (size_t stu = 0; stu < rt_trip_update->n_stop_time_update; ++stu) {
                            TransitRealtime__TripUpdate__StopTimeUpdate *rt_stop_time_update = rt_trip_update->stop_time_update[stu];

                            char *stop_id = rt_stop_time_update->stop_id;
                            uint32_t stop_index = rxt_find (tdata->stopid_index, stop_id);
                            uint32_t *route_stops = tdata->route_stops + route->route_stops_offset;

                            if (route_stops[rs] == stop_index) {
                                if (rt_stop_time_update->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SCHEDULED) {
                                    tdata_apply_gtfsrt_time (rt_stop_time_update, &tdata->trip_stoptimes[trip_index][rs]);
                                }
                                /* In case of NO_DATA we won't do anything */
                                rs++;

                            } else {
                                /* we do not align up with the realtime messages */
                                if (rt_stop_time_update->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SCHEDULED) {
                                    uint32_t propagate = rs;
                                    while (route_stops[++rs] != stop_index && rs < route->n_stops);
                                    if (route_stops[rs] == stop_index) {
                                        if (rt_stop_time_update_prev) {
                                            if (rt_stop_time_update_prev->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SCHEDULED &&
                                                rt_stop_time_update_prev->departure && rt_stop_time_update_prev->departure->has_delay) {
                                                for (propagate; propagate < rs; ++propagate) {
                                                    tdata->trip_stoptimes[trip_index][propagate].arrival   += SEC_TO_RTIME(rt_stop_time_update_prev->departure->delay);
                                                    tdata->trip_stoptimes[trip_index][propagate].departure += SEC_TO_RTIME(rt_stop_time_update_prev->departure->delay);
                                                }
                                            }
                                        }
                                        tdata_apply_gtfsrt_time (rt_stop_time_update, &tdata->trip_stoptimes[trip_index][rs]);
                                    } else {
                                        /* we couldn't find the stop at all */
                                        rs = propagate;
                                    }
                                    rs++;
                                }
                            }

                            rt_stop_time_update_prev = rt_stop_time_update;
                        }

                        /* the last StopTimeUpdate isn't the end of route_stops set SCHEDULED,
                            * and the departure has a delay, naively propagate the delay */
                        TransitRealtime__TripUpdate__StopTimeUpdate *rt_stop_time_update = rt_trip_update->stop_time_update[rt_trip_update->n_stop_time_update - 1];
                        if (rt_stop_time_update->schedule_relationship == TRANSIT_REALTIME__TRIP_UPDATE__STOP_TIME_UPDATE__SCHEDULE_RELATIONSHIP__SCHEDULED &&
                            rt_stop_time_update->departure && rt_stop_time_update->departure->has_delay) {
                            for (rs; rs < route->n_stops; ++rs) {
                                tdata->trip_stoptimes[trip_index][rs].arrival   += SEC_TO_RTIME(rt_stop_time_update->departure->delay);
                                tdata->trip_stoptimes[trip_index][rs].departure += SEC_TO_RTIME(rt_stop_time_update->departure->delay);
                            }
                        }
                    }
                }
            }
        } else
        #endif
        { continue; }
    }
cleanup:
    transit_realtime__feed_message__free_unpacked (msg, NULL);
}

void tdata_clear_gtfsrt (tdata_t *tdata) {
    for (int trip_index = 0; trip_index < tdata->n_trips; ++trip_index) {
        realtime_free_tripidx (tdata, trip_index);
        /* TODO: we don't restore the original trip_active */
    }
}

void tdata_apply_gtfsrt_file (tdata_t *tdata, char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) die("Could not find GTFS_RT input file.\n");
    struct stat st;
    if (stat(filename, &st) == -1) die("Could not stat GTFS_RT input file.\n");
    uint8_t *buf = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) die("Could not map GTFS-RT input file.\n");
    tdata_apply_gtfsrt (tdata, buf, st.st_size);
    munmap (buf, st.st_size);
}

void tdata_apply_gtfsrt_alerts (tdata_t *tdata, uint8_t *buf, size_t len) {
    TransitRealtime__FeedMessage *msg = transit_realtime__feed_message__unpack (NULL, len, buf);
    if (msg == NULL) {
        fprintf (stderr, "error unpacking incoming gtfs-rt message\n");
        return;
    }

    printf("Received feed message with %zu entities.\n", msg->n_entity);
    for (size_t e = 0; e < msg->n_entity; ++e) {
        TransitRealtime__FeedEntity *entity = msg->entity[e];
        if (entity == NULL) goto cleanup;
        // printf("  entity %d has id %s\n", e, entity->id);
        TransitRealtime__Alert *alert = entity->alert;
        if (alert == NULL) goto cleanup;

        for (size_t ie = 0; ie < alert->n_informed_entity; ++ie) {
            TransitRealtime__EntitySelector *informed_entity = alert->informed_entity[ie];
            if (!informed_entity) continue;

            if (informed_entity->route_id) {
                uint32_t route_index = rxt_find (tdata->routeid_index, informed_entity->route_id);
                if (route_index == RADIX_TREE_NONE) {
                     printf ("    route id was not found in the radix tree.\n");
                }
                memcpy (informed_entity->route_id, &route_index, sizeof(route_index));
            }

            if (informed_entity->stop_id) {
                uint32_t stop_index = rxt_find (tdata->stopid_index, informed_entity->stop_id);
                if (stop_index == RADIX_TREE_NONE) {
                     printf ("    stop id was not found in the radix tree.\n");
                }
                memcpy (informed_entity->stop_id, &stop_index, sizeof(stop_index));
            }

            if (informed_entity->trip && informed_entity->trip->trip_id) {
                uint32_t trip_index = rxt_find (tdata->tripid_index, informed_entity->trip->trip_id);
                if (trip_index == RADIX_TREE_NONE) {
                    printf ("    trip id was not found in the radix tree.\n");
                }
                memcpy (informed_entity->trip->trip_id, &trip_index, sizeof(trip_index));
            }
        }
    }

    tdata->alerts = msg;
    return;

    cleanup:
    transit_realtime__feed_message__free_unpacked (msg, NULL);
}

void tdata_clear_gtfsrt_alerts (tdata_t *tdata) {
    if (tdata->alerts) {
        transit_realtime__feed_message__free_unpacked (tdata->alerts, NULL);
        tdata->alerts = NULL;
    }
}

void tdata_apply_gtfsrt_alerts_file (tdata_t *tdata, char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) die("Could not find GTFS_RT input file.\n");
    struct stat st;
    if (stat(filename, &st) == -1) die("Could not stat GTFS_RT input file.\n");
    uint8_t *buf = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) die("Could not map GTFS-RT input file.\n");
    tdata_apply_gtfsrt_alerts (tdata, buf, st.st_size);
    munmap (buf, st.st_size);
}

#endif

// tdata_get_route_stops

/* optional stop ids, names, coordinates... */
