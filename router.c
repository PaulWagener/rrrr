/* router.c : the main routing algorithm */
#include "router.h" // first to ensure it works alone

#include "util.h"
#include "config.h"
#include "qstring.h"
#include "tdata.h"
#include "bitset.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define WALK -2

void router_setup(router_t *router, tdata_t *td) {
    srand(time(NULL));
    router->tdata = *td;
    router->table_size = td->n_stops * RRRR_MAX_ROUNDS;
    router->best_time = malloc(sizeof(rtime_t) * td->n_stops); 
    router->states = malloc(sizeof(router_state_t) * router->table_size);
    router->updated_stops = bitset_new(td->n_stops);
    router->updated_routes = bitset_new(td->n_routes);
    if ( ! (router->best_time && router->states && router->updated_stops && router->updated_routes))
        die("failed to allocate router scratch space");
}

static inline void router_reset(router_t router) {

}

void router_teardown(router_t *router) {
    free(router->best_time);
    free(router->states);
    bitset_destroy(router->updated_stops);
    bitset_destroy(router->updated_routes);
}

// flag_routes_for_stops... all at once after doing transfers?
/* Given a stop index, mark all routes that serve it as updated. */
static inline void flag_routes_for_stop (router_t *r, int stop_index, uint32_t date_mask) {
    /*restrict*/ int *routes;
    int n_routes = tdata_routes_for_stop (&(r->tdata), stop_index, &routes);
    for (int i = 0; i < n_routes; ++i) {
        uint32_t route_active_flags = r->tdata.route_active[routes[i]];
        I printf ("  flagging route %d at stop %d\n", routes[i], stop_index);
        // CHECK that there are any trips running on this route (another bitfield)
        // printf("route flags %d", route_active_flags);
        // printBits(4, &route_active_flags);
        if (date_mask & route_active_flags) { // seems to provide about 14% increase in throughput
           bitset_set (r->updated_routes, routes[i]);
           I printf ("  route running\n");
        }
    }
}

/* 
 For each updated stop and each destination of a transfer from an updated stop, 
 set the associated routes as updated. The routes bitset is cleared before the operation, 
 and the stops bitset is cleared after all transfers have been computed and all routes have been set.
 */
static inline void apply_transfers (router_t r, int round, float speed_meters_sec, 
                                    uint32_t date_mask, bool arrv) {
    tdata_t d = r.tdata; // this is copying... 
    router_state_t *states = r.states + (round * d.n_stops);
    bitset_reset (r.updated_routes);
    for (int stop_index_from = bitset_next_set_bit (r.updated_stops, 0); stop_index_from >= 0;
             stop_index_from = bitset_next_set_bit (r.updated_stops, stop_index_from + 1)) {
        I printf ("stop %d was marked as updated \n", stop_index_from);
        flag_routes_for_stop (&r, stop_index_from, date_mask);
        router_state_t *state_from = states + stop_index_from;
        rtime_t time_from = r.best_time[stop_index_from];
        if (time_from != UNREACHED) {
            I printf ("  applying transfer at %d (%s) \n", stop_index_from, tdata_stop_id_for_index(&d, stop_index_from));
            /* change to begin, length rather than begin, end in order to restrict pointers ? */
            transfer_t *tr     = d.transfers + d.stops[stop_index_from    ].transfers_offset;
            transfer_t *tr_end = d.transfers + d.stops[stop_index_from + 1].transfers_offset;
            // int n_transfers_for_stop = tr_end - tr;
            for ( ; tr < tr_end ; ++tr) {
                int stop_index_to = tr->target_stop;
                rtime_t transfer_duration = (((int)(tr->dist_meters / speed_meters_sec + RRRR_WALK_SLACK_SEC)) >> 1); // 2-sec units
                rtime_t time_to = arrv ? time_from - transfer_duration
                                       : time_from + transfer_duration;
                if (arrv ? time_to > time_from : time_to < time_from) {
                    printf ("\ntransfer overflow: %d %d\n", time_from, time_to);
                    continue;
                }
                I printf ("    target %d %s (%s) \n", stop_index_to, timetext(r.best_time[stop_index_to]), 
                          tdata_stop_id_for_index(&d, stop_index_to));
                I printf ("    transfer time   %s\n", timetext(transfer_duration));
                I printf ("    transfer result %s\n", timetext(time_to));
                bool better = (r.best_time[stop_index_to] == UNREACHED) ||
                              (arrv ? time_to > r.best_time[stop_index_to]
                                    : time_to < r.best_time[stop_index_to]);
                if (better) {
                    router_state_t *state_to = states + stop_index_to;
                    I printf ("      setting %d to %s\n", stop_index_to, timetext(time_to));
                    state_to->time = time_to; 
                    r.best_time[stop_index_to] = time_to;
                    state_to->back_route = WALK; // need sym const for walk distinct from NONE
                    state_to->back_stop = stop_index_from;
                    state_to->back_trip_id = "walk;walk"; // semicolon to provide headsign field in demo
                    state_to->board_time = state_from->time;
                    flag_routes_for_stop (&r, stop_index_to, date_mask);
                }
            } 
        }
    }
    bitset_reset (r.updated_stops);
}

static void dump_results(router_t *prouter) {
    router_t router = *prouter;
    router_state_t (*states)[router.tdata.n_stops] = (void*) router.states;
    char id_fmt[10];
    sprintf(id_fmt, "%%%ds", router.tdata.stop_id_width);
    printf("\nRouter states:\n");
    printf(id_fmt, "Stop name");
    printf(" [sindex]");
    for (int r = 0; r < RRRR_MAX_ROUNDS; ++r){
        printf("  round %d", r);
    }
    printf("\n");
    for (int stop = 0; stop < router.tdata.n_stops; ++stop) {
        bool set = false;
        for (int round = 0; round < RRRR_MAX_ROUNDS; ++round) {
            if (states[round][stop].time < UNREACHED) {
                set = true;
                break;
            } 
        }
        if ( ! set)
            continue;
        char *stop_id = tdata_stop_id_for_index (&(router.tdata), stop);
        printf(id_fmt, stop_id);
        printf(" [%6d]", stop);
        for (int round = 0; round < RRRR_MAX_ROUNDS; ++round) {
            printf(" %8s", timetext(states[round][stop].time));
        }
        printf("\n");
    }
    printf("\n");
}

// WARNING we are not currently storing trip IDs so this will segfault
void dump_trips(router_t *prouter) {
    router_t router = *prouter;
    int n_routes = router.tdata.n_routes;
    for (int ridx = 0; ridx < n_routes; ++ridx) {
        route_t route = router.tdata.routes[ridx];
        char (*trip_ids)[router.tdata.trip_id_width] = (void*)
            tdata_trip_ids_for_route(&(router.tdata), ridx);
        uint32_t *trip_masks = tdata_trip_masks_for_route(&(router.tdata), ridx);
        printf ("route %d (of %d), n trips %d, n stops %d\n", ridx, n_routes, route.n_trips, route.n_stops);
        for (int tidx = 0; tidx < route.n_trips; ++tidx) {
            printf ("trip index %d trip_id %s mask ", tidx, trip_ids[tidx]);
            printBits (4, & (trip_masks[tidx]));
            printf ("\n");
        }
    }
    exit(0);
}

/* Find a suitable trip to board at the given time and stop.
   Returns the trip index within the route. */
int find_departure(route_t *route, stoptime_t (*stop_times)[route->n_stops]) {
    return NONE;
}

bool router_route(router_t *prouter, router_request_t *preq) {
    // why copy? consider changing though router contains mostly pointers.
    // or just assume a single router per thread, and move struct fields into this module
    router_t router = *prouter; 
    router_request_t req = *preq;
    //router_request_dump(prouter, preq);

    int n_stops = router.tdata.n_stops;
    uint32_t date_mask = 1 << 3; // as a demo, search on the 3rd day of the schedule
    // yesterday, today, tomorrow
    // uint32_t date_masks[3] = {date_mask >> 1, date_mask, date_mask << 1};

    // Internal router time units are 2 seconds in order to fit 1.5 days into a uint16_t.
    rtime_t start_time = req.time >> 1; // TODO: make a function for this
    I router_request_dump(prouter, preq);
    T printf("\nstart_time %s \n", timetext(start_time));
    T tdata_dump(&(router.tdata));
    
    I printf("Initializing router state \n");
    // Router state is a C99 dynamically dimensioned array of size [RRRR_MAX_ROUNDS][n_stops]
    router_state_t (*states)[n_stops] = (void*) router.states; 
    for (int round = 0; round < RRRR_MAX_ROUNDS; ++round) {
        for (int stop = 0; stop < n_stops; ++stop) {
            states[round][stop].time = UNREACHED;
            // printf("%li ", &(states[round][stop]) - router.states);
        }
    }
    for (int s = 0; s < n_stops; ++s) {
        router.best_time[s] = UNREACHED;
    }
    router.best_time[req.from] = start_time;
    states[0][req.from].time = start_time;
    bitset_reset(router.updated_stops);
    bitset_set(router.updated_stops, req.from);
    // Apply transfers to initial state, which also initializes the updated routes bitset.
    apply_transfers(router, 0, req.walk_speed, date_mask, req.arrive_by);
    I dump_results(prouter);

    // TODO restrict pointers?
    // Iterate over rounds. In round N, we have made N transfers.
    for (int round = 0; round < RRRR_MAX_ROUNDS; ++round) { // RRRR_MAX_ROUNDS
        int last_round = (round == 0) ? 0 : round - 1;
        I printf("round %d\n", round);
        // Iterate over all routes which contain a stop that was updated in the last round.
        for (int route_idx = bitset_next_set_bit (router.updated_routes, 0); route_idx >= 0;
                 route_idx = bitset_next_set_bit (router.updated_routes, route_idx + 1)) {
            route_t route = router.tdata.routes[route_idx];
            I printf("  route %d: %s\n", route_idx, tdata_route_id_for_index(&(router.tdata), route_idx));
            T tdata_dump_route(&(router.tdata), route_idx);
            // For each stop in this route, its global stop index.
            int *route_stops = tdata_stops_for_route(router.tdata, route_idx);
            // C99 dynamically dimensioned array of size [route.n_trips][route.n_stops]
            stoptime_t (*stop_times)[route.n_stops] = (void*)
                 tdata_stoptimes_for_route(&(router.tdata), route_idx);
            char (*trip_ids)[router.tdata.trip_id_width] = (void*)
                 tdata_trip_ids_for_route(&(router.tdata), route_idx); 
            uint32_t *trip_masks = tdata_trip_masks_for_route(&(router.tdata), route_idx); 
            int trip = NONE;       // trip index within the route. NONE means not yet boarded.
            char *trip_id = NULL;  // the trip_id of the currently boarded trip
            int board_stop = NONE; // stop index where that trip was boarded
            int board_time = NONE; // time when that trip was boarded
            // Iterate over stop indexes within the route. Each one corresponds to a global stop index.
            // Note that the stop times array should be accessed with [trip][route_stop] not [trip][stop].
            for (int route_stop = req.arrive_by ? route.n_stops - 1 : 0;
                 req.arrive_by ? route_stop >= 0 : route_stop < route.n_stops; 
                 req.arrive_by ? --route_stop : ++route_stop ) {
                int stop = route_stops[route_stop];
                I printf("    stop %2d [%d] %s %s\n", route_stop, stop,
                    timetext(router.best_time[stop]), tdata_stop_id_for_index (&(router.tdata), stop));
                // TODO: check if this is the last stop -- no point boarding there or marking routes
                if (trip == NONE || (
                    // check to avoid overflow since UNREACHED is UINT16_MAX
                    states[last_round][stop].time < UNREACHED - RRRR_XFER_SLACK_2SEC && 
                    states[last_round][stop].time + RRRR_XFER_SLACK_2SEC < 
                    stop_times[trip][route_stop].departure)) {
                    /* If we have not yet boarded a trip on this route, see if we can board one.
                       Also handle the case where we hit a stop with an existing better arrival time. 
                       It would be "more efficient" if we scan backward but scanning forward seems 
                       reasonably performant. */
                    if (router.best_time[stop] == UNREACHED) {
                        // This stop has not been reached, move on to the next one.
                        continue; 
                    }
                    if (states[last_round][stop].time == UNREACHED) {
                        // Only attempt boarding at places that were reached in the last round.
                        continue;
                    }
                    // Scan to the nearest trip that can be boarded, if any.
                    D printf("hit previously-reached stop %d\n", stop);
                    T tdata_dump_route(&(router.tdata), route_idx);
                    // Real-time updates can ruin FIFO ordering within routes; track the best trip.
                    // Scanning through the whole list reduces speed by ~20 percent.
                    int     best_trip = NONE;
                    rtime_t best_time = req.arrive_by ? 0 : UINT16_MAX; // should just add a conditional on UNREACHED below for readability
                    for (int this_trip = 0; this_trip < route.n_trips; ++this_trip) {
                        T printf("    board option %d at %s \n", this_trip, 
                                 timetext(stop_times[this_trip][route_stop].departure));
                        D printBits(4, & (trip_masks[this_trip]));
                        D printBits(4, & date_mask);
                        D printf("\n");
                        if ( ! (date_mask & trip_masks[this_trip])) // trip is not running today
                            continue; 
                        rtime_t time = req.arrive_by ? stop_times[this_trip][route_stop].arrival
                                                     : stop_times[this_trip][route_stop].departure;
                        // If this trip's arrival time is later than the best known for this stop,
                        // and if this trip is running, board this trip.
                        if (req.arrive_by ? 
                            time + RRRR_XFER_SLACK_2SEC <= router.best_time[stop] && time > best_time :
                            time - RRRR_XFER_SLACK_2SEC >= router.best_time[stop] && time < best_time) {
                            best_trip = this_trip;
                            best_time = time;
                        }
                    } // end for (trips within this route)
                    if (best_trip != NONE) {
                        I printf("    boarding trip %d at %s \n", best_trip, timetext(best_time));
                        // use a router_state struct for all this?
                        trip_id = trip_ids[best_trip];
                        board_time = best_time;
                        board_stop = stop;
                        trip = best_trip;
                        if (req.arrive_by ? best_time > start_time : best_time < start_time)
                            printf("ERROR: boarded before start time, trip %d stop %d \n", trip, stop);
                    } else {
                        T printf("    no suitable trip to board.\n");
                    }
                    continue; // to the next stop in the route
                } else if (trip != NONE) { // We have already boarded a trip along this route.
                    rtime_t time = req.arrive_by ? stop_times[trip][route_stop].departure 
                                                 : stop_times[trip][route_stop].arrival;
                    T printf("    on board trip %d considering time %s \n", trip, timetext(time)); 
                    if ((router.best_time[req.to] != UNREACHED) && 
                        (req.arrive_by ? time < router.best_time[req.to] 
                                       : time > router.best_time[req.to])) { // "target pruning" sec. 3.1
                        T printf("    (target pruning)\n");
                        continue; // could we even break out of this route entirely?
                    }
                    bool improved = (router.best_time[stop] == UNREACHED) || 
                                    (req.arrive_by ? time > router.best_time[stop] 
                                                   : time < router.best_time[stop]);
                    if (!improved) {
                        I printf("    (no improvement)\n");
                        continue; // the current trip does not improve on the best time at this stop
                    }
                    char *route_id = tdata_route_id_for_index(&(router.tdata), route_idx); // for demo, actually contains a detailed route description
                    I printf("    setting stop to %s \n", timetext(time)); 
                    router.best_time[stop] = time;
                    states[round][stop].time = time;
                    states[round][stop].back_route = route_idx; 
                    states[round][stop].back_trip_id = route_id;  // changed for demo, was: trip_id; 
                    states[round][stop].back_stop = board_stop;
                    states[round][stop].board_time = board_time;
                    bitset_set(router.updated_stops, stop);   // mark stop for next round.
                } 
            } // end for (stop)
        } // end for (route)
        // if (round < RRRR_MAX_ROUNDS - 1) { /* can only do this optimization when transfers and marking are separated */
        // Update list of routes for next round based on stops that were touched in this round.
        apply_transfers(router, round, req.walk_speed, date_mask, req.arrive_by);
        // just in case
        states[round][req.from].time = start_time;
        states[round][req.from].back_stop = -1;
        states[round][req.from].back_route = -1;
        // dump_results(prouter); // DEBUG
        // exit(0);
    } // end for (round)
    return true;
}

int router_result_dump(router_t *prouter, router_request_t *preq, char *buf, int buflen) {
    router_t router = *prouter;
    router_request_t req = *preq;
    char *b = buf;
    char *b_end = buf + buflen;
    for (int round_outer = 0; round_outer < RRRR_MAX_ROUNDS; ++round_outer) {
        int s = req.to;
        router_state_t *states = router.states + router.tdata.n_stops * round_outer;
        if (states[s].time == UNREACHED)
            continue;
        b += sprintf (b, "\nA %d VEHICLES \n", round_outer + 1);
        int round = round_outer;
        while (round >= 0) {
            states = router.states + router.tdata.n_stops * round;
            if (states[s].time == UNREACHED) {
                round -= 1;
                b += sprintf (b, "%d UNREACHED \n", s);
                continue;
            } 
            //b += sprintf (b, "round %d  ", round);
            int last_stop = states[s].back_stop;
            if (s < 0 || s > router.tdata.n_stops) { 
                // this was causing segfaults
                b += sprintf (b, "neg stopid %d\n", s);
                break;
            }
            int route = states[s].back_route;
            char *last_stop_id = tdata_stop_id_for_index(&(router.tdata), last_stop);
            char *this_stop_id = tdata_stop_id_for_index(&(router.tdata), s);
            int board  = states[s].board_time;
            int alight = states[s].time;
            char cboard[255];
            char calight[255];
            btimetext(board, cboard, 255);
            btimetext(alight, calight, 255);
            char *trip_id = states[s].back_trip_id;

            b += sprintf (b, "%s;%s;%s;%s;%s\n", trip_id, 
                last_stop_id, cboard, this_stop_id, calight);
            if (b > b_end) {
                printf ("buffer overflow\n");
                break;
            }
            if (last_stop == req.from) 
                break;
            if (route >= 0 && round > 0)
                round -= 1;
            s = last_stop;
        }
    }
    *b = '\0';
    return b - buf;
}

inline static void set_defaults(router_request_t *req) {
    req->walk_speed = 1.3; // m/sec
    req->from = req->to = req->time = NONE; 
    req->arrive_by = false;
}

int rrrrandom(int limit) {
    return (int) (limit * (random() / (RAND_MAX + 1.0)));
}

void router_request_randomize(router_request_t *req) {
    req->walk_speed = 1.5; // m/sec
    req->from = rrrrandom(6600);
    req->to = rrrrandom(6600);
    req->time = 3600 * 12 + rrrrandom(3600 * 6);
    req->arrive_by = true;
}

inline static bool range_check(router_request_t *req) {
    if (req->walk_speed < 0.1) return false;
    if (req->from < 0) return false;
    if (req->to < 0) return false;
    if (req->time < 0) return false;
    return true;
}

#define BUFLEN 255
bool router_request_from_qstring(router_request_t *req) {
    char *qstring = getenv("QUERY_STRING");
    if (qstring == NULL) 
        qstring = "";
    set_defaults(req);
    char key[BUFLEN];
    char *val;
    while (qstring_next_pair(qstring, key, &val, BUFLEN)) {
        if (strcmp(key, "time") == 0) {
            req->time = atoi(val);
        } else if (strcmp(key, "from") == 0) {
            req->from = atoi(val);
        } else if (strcmp(key, "to") == 0) {
            req->to = atoi(val);
        } else if (strcmp(key, "speed") == 0) {
            req->walk_speed = atof(val);
        } else if (strcmp(key, "randomize") == 0) {
            printf("RANDOMIZING\n");
            router_request_randomize(req);
            return true;
        } else {
            printf("unrecognized parameter: key=%s val=%s\n", key, val);
        }
    }
    return true;
}

void router_request_dump(router_t *router, router_request_t *req) {
    char *from_stop_id = tdata_stop_id_for_index(&(router->tdata), req->from);
    char *to_stop_id = tdata_stop_id_for_index(&(router->tdata), req->to);
    printf("from: %s [%d]\n"
           "to:   %s [%d]\n"
           "time: %s [%ld]\n"
           "speed: %f\n", from_stop_id, req->from, to_stop_id, req->to, 
                timetext(req->time >> 1), req->time, req->walk_speed);
}

