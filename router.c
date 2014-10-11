/* Copyright 2013 Bliksem Labs.
 * See the LICENSE file at the top-level directory of this distribution and at
 * https://github.com/bliksemlabs/rrrr/
 */

/* router.c : the main routing algorithm */
#include "router.h" /* first to ensure it works alone */

#include "util.h"
#include "config.h"
#include "tdata.h"
#include "bitset.h"
#include "hashgrid.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <math.h>

bool router_setup(router_t *router, tdata_t *tdata) {

    srand(time(NULL));
    router->tdata = tdata;
    router->best_time = (rtime_t *) malloc(sizeof(rtime_t) * tdata->n_stops);
    router->states = (router_state_t *) malloc(sizeof(router_state_t) * (tdata->n_stops * RRRR_DEFAULT_MAX_ROUNDS));
    router->updated_stops = bitset_new(tdata->n_stops);
    router->updated_routes = bitset_new(tdata->n_routes);
    if ( ! (router->best_time && router->states && router->updated_stops && router->updated_routes)) {
        fprintf(stderr, "failed to allocate router scratch space");
        return false;
    }

    #ifdef RRRR_FEATURE_LATLON
    {
        uint32_t i_stop;
        coord_t coords[router->tdata->n_stops];
        for (i_stop = 0; i_stop < router->tdata->n_stops; ++i_stop) {
            coord_from_latlon(coords + i_stop, router->tdata->stop_coords + i_stop);
        }
        HashGrid_init (&router->hg, 100, 500.0, coords, router->tdata->n_stops);
    }
    #endif

    return true;
}

void router_reset(router_t *router) {

    /* Make sure both origin and target are initialised with NONE, so it
     * becomes possible to validate that they have been set to a valid
     * stop index.
     */
    router->origin = NONE;
    router->target = NONE;

    /* The best times to arrive at a stop scratch space is initialised with
     * UNREACHED. This allows to compare for a lesser time candidate in the
     * search.
     */

    memset32((void *) router->best_time, UNREACHED, router->tdata->n_stops);
}

void router_teardown(router_t *router) {
    free(router->best_time);
    free(router->states);
    bitset_destroy(router->updated_stops);
    bitset_destroy(router->updated_routes);
}

/* TODO? flag_routes_for_stops all at once after doing transfers? this would
 * require another stops bitset for transfer target stops.
 */
/* Given a stop index, mark all routes that serve it as updated. */
void flag_routes_for_stop (router_t *router, router_request_t *req, uint32_t stop_index) {
    uint32_t *routes;
    uint32_t i_route;
    uint32_t n_routes = tdata_routes_for_stop (router->tdata, stop_index, &routes);
    for (i_route = 0; i_route < n_routes; ++i_route) {
        calendar_t route_active_flags = router->tdata->route_active[routes[i_route]];
        #ifdef RRRR_INFO
        printf ("  flagging route %d at stop %d\n", routes[i_route], stop_index);
        #endif

        /* CHECK that there are any trips running on this route (another bitfield)
         * printf("route flags %d", route_active_flags);
         * printBits(4, &route_active_flags);
         */
        if ((router->day_mask & route_active_flags) && /* seems to provide about 14% increase in throughput */
            (req->mode & router->tdata->routes[routes[i_route]].attributes) > 0) {
           bitset_set (router->updated_routes, routes[i_route]);
           #ifdef RRRR_INFO
           printf ("  route running\n");
           #endif
        }
    }

    #ifdef RRRR_FEATURE_REALTIME_EXPANDED
    if (router->servicedays[1].apply_realtime && router->tdata->rt_stop_routes[stop_index]) {
        uint32_t i_route;
        routes = router->tdata->rt_stop_routes[stop_index]->list;
        n_routes = router->tdata->rt_stop_routes[stop_index]->len;
        for (i_route = 0; i_route < n_routes; ++i_route) {
            #ifdef RRRR_INFO
            printf ("  flagging changed route %d at stop %d\n", routes[i_route], stop_index);
            #endif
            /* extra routes should only be applied on the current day */
            if ((req->mode & router->tdata->routes[routes[i_route]].attributes) > 0) {
                bitset_set (router->updated_routes, routes[i_route]);
                #ifdef RRRR_INFO
                printf ("  route running\n");
                #endif
            }
        }
    }
    #endif
}

void unflag_banned_routes (router_t *router, router_request_t *req) {
    uint32_t i_banned_route;
     for (i_banned_route = 0;
          i_banned_route < req->n_banned_routes;
          ++i_banned_route) {
         bitset_unset (router->updated_routes, req->banned_route[i_banned_route]);
     }
}

void unflag_banned_stops (router_t *router, router_request_t *req) {
    uint32_t i_banned_stop;
    for (i_banned_stop = 0;
         i_banned_stop < req->n_banned_stops;
         ++i_banned_stop) {
        bitset_unset (router->updated_stops, req->banned_stop[i_banned_stop]);
    }
}

/* Because the first round begins with so few reached stops, the initial state
 * doesn't get its own full array of states. Instead we reuse one of the later
 * rounds (round 1) for the initial state. This means we need to reset the
 * walks in round 1 back to UNREACHED before using them in routing. Rather
 * than iterating over all of them, we only initialize the stops that can be
 * reached by transfers.
 * Alternatively we could instead initialize walks to UNREACHED at the
 * beginning of the transfer calculation function.
 * We should not however reset the best times for those stops reached from the
 * initial stop on foot. This will prevent finding circuitous itineraries that
 * return to them.
 */
void initialize_transfers (router_t *router, uint32_t round, uint32_t stop_index_from) {
    router_state_t *states = router->states + (round * router->tdata->n_stops);
    uint32_t t  = router->tdata->stops[stop_index_from    ].transfers_offset;
    uint32_t tN = router->tdata->stops[stop_index_from + 1].transfers_offset;
    states[stop_index_from].walk_time = UNREACHED;
    for ( ; t < tN ; ++t) {
        uint32_t stop_index_to = router->tdata->transfer_target_stops[t];
        states[stop_index_to].walk_time = UNREACHED;
    }
}

/* Because the first round begins with so few reached stops, the initial state
 * doesn't get its own full array of states
 * Instead we reuse one of the later rounds (round 1) for the initial state.
 * This means we need to reset the walks in round 1 back to UNREACHED before
 * using them in routing. We iterate over all of them, because we don't
 * maintain a list of all the stops that might have been added by the hashgrid.
 */
void initialize_transfers_full (router_t *router, uint32_t round) {
    uint32_t i_stop;
    router_state_t *states = router->states + (round * router->tdata->n_stops);
    for ( i_stop = 0; i_stop < router->tdata->n_stops; ++i_stop) {
        states[i_stop].walk_time = UNREACHED;
    }
}


/* For each updated stop and each destination of a transfer from an updated
 * stop, set the associated routes as updated. The routes bitset is cleared
 * before the operation, and the stops bitset is cleared after all transfers
 * have been computed and all routes have been set.
 * Transfer results are computed within the same round, based on arrival time
 * in the ride phase and stored in the walk time member of states.
 */
void apply_transfers (router_t *router, router_request_t *req, uint32_t round, bool transfer) {
    uint32_t stop_index_from;
    router_state_t *states = router->states + (round * router->tdata->n_stops);
    /* The transfer process will flag routes that should be explored in
     * the next round.
     */
    bitset_clear (router->updated_routes);
    for (stop_index_from  = bitset_next_set_bit (router->updated_stops, 0);
         stop_index_from != BITSET_NONE;
         stop_index_from  = bitset_next_set_bit (router->updated_stops, stop_index_from + 1)) {
        #ifdef RRRR_INFO
        printf ("stop %d was marked as updated \n", stop_index_from);
        #endif
        router_state_t *state_from = states + stop_index_from;
        rtime_t time_from = state_from->time;
        if (time_from == UNREACHED) {
            printf ("ERROR: transferring from unreached stop %d in round %d. \n", stop_index_from, round);
            continue;
        }
        /* At this point, the best time at the from stop may be different than
         * the state_from->time, because the best time may have been updated
         * by a transfer.
         */
        #if 0
        if (time_from != router->best_time[stop_index_from]) {
            printf ("ERROR: time at stop %d in round %d is not the same as its best time. \n", stop_index_from, round);
            printf ("    from time %s \n", timetext(time_from));
            printf ("    walk time %s \n", timetext(state_from->walk_time));
            printf ("    best time %s \n", timetext(router->best_time[stop_index_from]));
            continue;
        }
        #endif
        #ifdef RRRR_INFO
        printf ("  applying transfer at %d (%s) \n", stop_index_from, tdata_stop_name_for_index(router->tdata, stop_index_from));
        #endif
        /* First apply a transfer from the stop to itself,
         * if case that's the best way.
         */
        if (state_from->time == router->best_time[stop_index_from]) {
            /* This state's best time is still its own.
             * No improvements from other transfers.
             */
            state_from->walk_time = time_from;
            state_from->walk_from = stop_index_from;
            /* assert (router->best_time[stop_index_from] == time_from); */
            flag_routes_for_stop (router, req, stop_index_from);
            unflag_banned_routes (router, req);
        }

        if (transfer) {
        /* Then apply transfers from the stop to nearby stops */
        uint32_t tr     = router->tdata->stops[stop_index_from    ].transfers_offset;
        uint32_t tr_end = router->tdata->stops[stop_index_from + 1].transfers_offset;
        for ( ; tr < tr_end ; ++tr) {
            uint32_t stop_index_to = router->tdata->transfer_target_stops[tr];
            /* Transfer distances are stored in units of 16 meters,
             * rounded not truncated, in a uint8_t
             */
            uint32_t dist_meters = router->tdata->transfer_dist_meters[tr] << 4;
            rtime_t transfer_duration = SEC_TO_RTIME((uint32_t)(dist_meters / req->walk_speed + req->walk_slack));
            rtime_t time_to = req->arrive_by ? time_from - transfer_duration
                                             : time_from + transfer_duration;
            /* Avoid reserved values including UNREACHED */
            if (time_to > RTIME_THREE_DAYS) continue;
            /* Catch wrapping/overflow due to limited range of rtime_t
             * this happens normally on overnight routing but should
             * be avoided rather than caught.
             */
            if (req->arrive_by ? time_to > time_from : time_to < time_from) continue;
            #ifdef RRRR_INFO
            printf ("    target %d %s (%s) \n", stop_index_to, timetext(router->best_time[stop_index_to]), tdata_stop_name_for_index(router->tdata, stop_index_to));
            printf ("    transfer time   %s\n", timetext(transfer_duration));
            printf ("    transfer result %s\n", timetext(time_to));
            #endif
            /* TODO verify state_to->walk_time versus router->best_time[stop_index_to] */
            if (router->best_time[stop_index_to] == UNREACHED ||
                (req->arrive_by ? time_to > router->best_time[stop_index_to]
                                : time_to < router->best_time[stop_index_to])) {

                router_state_t *state_to = states + stop_index_to;
                #ifdef RRRR_INFO
                printf ("      setting %d to %s\n", stop_index_to, timetext(time_to));
                #endif
                state_to->walk_time = time_to;
                state_to->walk_from = stop_index_from;
                router->best_time[stop_index_to] = time_to;
                flag_routes_for_stop (router, req, stop_index_to);
                unflag_banned_routes (router, req);
            }
        }
        }
    }
    /* Done with all transfers, reset stop-reached bits for the next round */
    bitset_clear (router->updated_stops);
    /* Check invariant: Every stop reached in this round should have a
     * best time equal to its walk time, and
     * a walk arrival time <= its ride arrival time.
     */
}

/* Get the departure or arrival time of the given trip on the given
 * service day, applying realtime data as needed.
 */
rtime_t
tdata_stoptime (tdata_t* tdata, uint32_t i_route, uint32_t trip_offset, uint32_t route_stop, bool arrive, serviceday_t *serviceday) {
    rtime_t time, time_adjusted;
    stoptime_t *trip_times;

    /* This code is only required if want realtime support in the journey planner */
    #ifdef RRRR_REALTIME_EXPANDED

    /* given that we are at a serviceday the realtime scope applies to */
    if (serviceday->apply_realtime) {

        /* the expanded stoptimes can be found at the same row as the trip */
        trip_times = tdata->trip_stoptimes[tdata->routes[i_route].trip_ids_offset + trip_offset];

        if (trip_times) {
            /* if the expanded stoptimes have been added, the begin_time is precalculated */
            time = 0;
        } else {
            /* if the expanded stoptimes have not been added, or our source is not time-expanded */
            trip_t *trip = tdata_trips_for_route (tdata, i_route) + trip_offset;
            trip_times = &tdata->stop_times[trip->stop_times_offset];
            time = trip->begin_time;
        }
    } else
    #endif /* RRRR_REALTIME_EXPANDED */
    {
        trip_t *trip = tdata_trips_for_route (tdata, i_route) + trip_offset;
        trip_times = &tdata->stop_times[trip->stop_times_offset];
        time = trip->begin_time;
    }

    if (arrive) {
        time += trip_times[route_stop].arrival;
    } else {
        time += trip_times[route_stop].departure;
    }

    time_adjusted = time + serviceday->midnight;

    /*
    printf ("boarding at stop %d, time is: %s \n", route_stop, timetext (time));
    printf ("   after adjusting: %s \n", timetext (time_adjusted));
    printf ("   midnight: %d \n", serviceday->midnight);
    */

    /* Detect overflow (this will still not catch wrapping due to negative
     * delays on small positive times) actually this happens naturally with
     * times like '03:00+1day' transposed to serviceday 'tomorrow'
     */
    if (time_adjusted < time) return UNREACHED;
    return time_adjusted;
}

bool router_route(router_t *router, router_request_t *req) {
    /* In this function we are setting up all initial required elements of the
     * routing engine we also infer what the requestee wants to do, the
     * following use cases can be observed:
     *
     * 0) from is actually onboard an existing trip
     * 1) from/to a station, req->from and/or req->to are filled
     * 2) from/to a coordinate, req->from_coord and/or req->to_coord are filled
     *
     * Given 0, we calculate the previous stop from an existing running trip
     * and determine the best way to the destination.
     *
     * Given 1, the user is actually at a stop and wants to leave from there,
     * in the second round transfers are applied which may move the user by
     * feet to a different stop. We consider the origin and destination as
     * constraint, explicitly enforced by the user.
     * "I am here, only move me if it is strictly required."
     *
     * Given 2, the user is at a location, which may be stop. We start with a
     * search around this coordinate up to a maximum walk distances,
     * configurable by the user. The first forward search starts from all
     * found locations. Based on the distance, a weight factor and the
     * walk-speed an extra time calculated and encounted for.
     * A normal search will match up to the stop which is the closest to the
     * destination.
     * TODO: We store all possible paths from the forward search to all
     * possible destinations.
     * The backward search uses the best times for all near by stops, again
     * the extra walk time is added. The search will now unambiously determine
     * the last possible departure.
     * From the second forward search we only search for the best plan to the
     * best destination. This plan will be marked as "best".
     */

    #ifdef RRRR_DEBUG
    router_request_dump(router, preq);
    #endif
    router->day_mask = req->day_mask;

    /* One serviceday_t for each of: yesterday, today, tomorrow
     * (for overnight searches)
     * Note that yesterday's bit flag will be 0 if today is the first day
     * of the calendar.
     */
    {
         /* One bit for the calendar day on which realtime data should be
          * applied (applying only on the true current calendar day)
          */
        calendar_t realtime_mask = 1 << ((time(NULL) - router->tdata->calendar_start_time) / SEC_IN_ONE_DAY);
         /*  Fake realtime_mask if we are QA testing. */
         /*  realtime_mask = 1; */
        serviceday_t yesterday;
        serviceday_t today;
        serviceday_t tomorrow;
        yesterday.midnight = 0;
        yesterday.mask = router->day_mask >> 1;
        yesterday.apply_realtime = yesterday.mask & realtime_mask;
        today.midnight = RTIME_ONE_DAY;
        today.mask = router->day_mask;
        today.apply_realtime = today.mask & realtime_mask;
        tomorrow.midnight = RTIME_TWO_DAYS;
        tomorrow.mask = router->day_mask << 1;
        tomorrow.apply_realtime = tomorrow.mask & realtime_mask;
        /* Iterate backward over days for arrive-by searches. */
        if (req->arrive_by) {
            router->servicedays[0] = tomorrow;
            router->servicedays[1] = today;
            router->servicedays[2] = yesterday;
        } else {
            router->servicedays[0] = yesterday;
            router->servicedays[1] = today;
            router->servicedays[2] = tomorrow;
        }
        /* set day_mask to catch all service days (0, 1, 2) */
        router->day_mask = yesterday.mask | today.mask | tomorrow.mask;
    }
     /*  for (int i = 0; i < 3; ++i) service_day_dump (&routers->servicedays[i]); */
     /*  day_mask_dump (router->day_mask); */

    #ifdef RRRR_INFO
    router_request_dump(router, req);
    #endif
    #ifdef RRRR_TRIP
    printf("\norigin_time %s \n", timetext(req->time));
    tdata_dump(router->tdata);
    #endif

    {
        uint32_t i;
        #ifdef RRRR_INFO
        printf("Initializing router state \n");
        #endif
        for (i = 0; i < (RRRR_DEFAULT_MAX_ROUNDS * router->tdata->n_stops); ++i) {
            router->states[i].time = UNREACHED;
            router->states[i].walk_time = UNREACHED;
        }

        /*  done in router_reset */
        /*  for (uint32_t s = 0; s < n_stops; ++s) router->best_time[s] = UNREACHED; */

        /* Stop indexes where the search process begins and ends,
         * independent of arrive_by */
        if (req->arrive_by) {
            router->origin = req->to;
            router->target = req->from;
        } else {
            router->origin = req->from;
            router->target = req->to;
        }
    }

    if (req->onboard_trip_route != NONE && req->onboard_trip_offset != NONE) {
        /* We are starting on board a trip, not at a station. */
        /* On-board departure only makes sense for depart-after requests. */
        if (req->arrive_by) {
            fprintf (stderr, "An arrive-by search does not make any sense if you are starting on-board.\n");
            return false;
        }
        {
            /* We cannot expand the start trip into the temporary round (1)
             * during initialization because we may be able to reach the
             * destination on that starting trip.
             * We discover the previous stop and flag only the selected route
             * for exploration in round 0. This would interfere with search
             * reversal, but reversal is meaningless/useless in on-board
             * depart trips anyway.
             */
            route_t  route = router->tdata->routes[req->onboard_trip_route];
            uint32_t *route_stops   = tdata_stops_for_route(router->tdata, req->onboard_trip_route);
            uint32_t prev_stop      = NONE;
            uint32_t i_route_stop;
            rtime_t  prev_stop_time = UNREACHED;
            /* TODO: add tdata function to return next stop and stoptime given
             * route, trip, and time */
            for (i_route_stop = 0; i_route_stop < route.n_stops; ++i_route_stop) {
                rtime_t time = tdata_stoptime (router->tdata, req->onboard_trip_route, req->onboard_trip_offset, i_route_stop, false, &(router->servicedays[1]));
                /* Find stop immediately after the given time on the given trip. */
                if (req->arrive_by ? time > req->time : time < req->time) {
                    if (prev_stop_time == UNREACHED || (req->arrive_by ? time < prev_stop_time : time > prev_stop_time)) {
                        prev_stop = route_stops[i_route_stop];
                        prev_stop_time = time;
                    }
                }
            }
            if (prev_stop != NONE) {
                #ifdef RRRR_INFO
                /* rewrite the request to begin at the previous stop on the starting trip */
                char *prev_stop_id = tdata_stop_name_for_index(router->tdata, prev_stop);   /* TODO Andrew shouldn't this be stop_id? */
                printf ("Based on start trip and time, chose previous stop %s [%d] at %s\n", prev_stop_id, prev_stop, timetext(prev_stop_time));
                #endif

                req->from = ONBOARD;
                /* Initialize origin state */
                router->origin = prev_stop;  /*  only origin is used from here on in routing */
                router->best_time[router->origin]   = prev_stop_time;

                /* set the origin stop in the "2nd round" */
                router->states[1 * router->tdata->n_stops + router->origin].time      = prev_stop_time;
                router->states[1 * router->tdata->n_stops + router->origin].walk_time = prev_stop_time;

                /* When starting on board, only flag one route and do not apply transfers, only a single walk. */
                bitset_clear (router->updated_stops);
                bitset_clear (router->updated_routes);
                bitset_set (router->updated_routes, req->onboard_trip_route);
            }
        }
    }

    /* Initialize origin state if not beginning the search on board. */
    if (req->from != ONBOARD) {

        #ifdef RRRR_FEATURE_LATLON
        /* Case 2: the user requested coordinates */
        if (req->to == NONE || req->from == NONE) {
            /* Hack to communicate the origin time to itinerary renderer. It would be better to just include rtime_t in request structs. */
             /*  TODO eliminate this now that we have rtimes in requests */
            bitset_clear(router->updated_stops);


            if ((req->arrive_by ? req->to == NONE : req->from == NONE)) {
                double distance;
                double best_distance = INFINITY;
                uint32_t stop_index;
                uint32_t best_stop_index = NONE;
                HashGridResult *hg_result;

                if (req->arrive_by) {
                    if (req->to_hg_result.hg == NULL) {
                        coord_t coord;
                        coord_from_latlon (&coord, &req->to_latlon);
                        HashGrid_query (&router->hg, &req->to_hg_result, coord, req->max_walk_distance);
                    }
                    hg_result = &req->to_hg_result;
                } else {
                    if (req->from_hg_result.hg == NULL ) {
                        coord_t coord;
                        coord_from_latlon (&coord, &req->from_latlon);
                        HashGrid_query (&router->hg, &req->from_hg_result, coord, req->max_walk_distance);
                    }
                    hg_result = &req->from_hg_result;
                }

                #ifdef RRRR_INFO
                printf ("Hashgrid results:\n");
                #endif

                HashGridResult_reset(hg_result);
                stop_index = HashGridResult_next_filtered(hg_result, &distance);

                while (stop_index != HASHGRID_NONE) {
                    rtime_t extra_walktime = SEC_TO_RTIME((uint32_t)((distance * RRRR_WALK_COMP) / req->walk_speed));

                    if (req->arrive_by) {
                        router->best_time[stop_index] = req->time - extra_walktime;
                        states[1][stop_index].time    = req->time - extra_walktime;
                    } else {
                        router->best_time[stop_index] = req->time + extra_walktime;
                        states[1][stop_index].time    = req->time + extra_walktime;
                    }

                     /*  the rest of these should be unnecessary */
                    states[1][stop_index].ride_from  = NONE;
                    states[1][stop_index].back_route = NONE;
                    states[1][stop_index].back_trip  = NONE;
                    states[1][stop_index].board_time = UNREACHED;

                    bitset_set(router->updated_stops, stop_index);

                    if (distance < best_distance) {
                        best_distance = distance;
                        best_stop_index = stop_index;
                    }

                    #ifdef RRRR_INFO
                    printf ("%d %s %s (%.0fm)\n", stop_index, tdata_stop_id_for_index(router->tdata, stop_index), tdata_stop_name_for_index(router->tdata, stop_index), distance);
                    #endif

                    /* get the next potential start stop */
                    stop_index = HashGridResult_next_filtered(hg_result, &distance);
                }

                router->origin = best_stop_index;
            } else {
                router->best_time[router->origin] = req->time;
                states[1][router->origin].time = req->time;

                 /*  the rest of these should be unnecessary */
                states[1][router->origin].ride_from  = NONE;
                states[1][router->origin].back_route = NONE;
                states[1][router->origin].back_trip  = NONE;
                states[1][router->origin].board_time = UNREACHED;

                bitset_set(router->updated_stops, router->origin);
            }

            if ((!req->arrive_by ? req->to == NONE : req->from == NONE)) {
                if (req->arrive_by) {
                    if (req->from_hg_result.hg == NULL) {
                        coord_t coord;
                        coord_from_latlon (&coord, &req->from_latlon);
                        HashGrid_query (&router->hg, &req->from_hg_result, coord, req->max_walk_distance);
                    }
                    HashGridResult_reset (&req->from_hg_result);
                    router->target = HashGridResult_closest (&req->from_hg_result);
                } else {
                    if (req->to_hg_result.hg == NULL ) {
                        coord_t coord;
                        coord_from_latlon (&coord, &req->to_latlon);
                        HashGrid_query (&router->hg, &req->to_hg_result, coord, req->max_walk_distance);
                    }
                    HashGridResult_reset (&req->to_hg_result);
                    router->target = HashGridResult_closest (&req->to_hg_result);
                }
            }
             /*  TODO check for router->origin == NONE before */
            if (router->origin == NONE) {
                fprintf (stderr, "Router origin is unset.\n");
                return false;
            }

             /*  TODO eliminate this now that we have rtimes in requests */
            states[0][router->origin].time = req->time;


            /* End Case 2*/
        } else
        #endif

        {
            /* Hack to communicate the origin time to itinerary renderer.
             * It would be better to just include rtime_t in request structs. */
            /* TODO eliminate this now that we have rtimes in requests */
            router->states[0 * router->tdata->n_stops + router->origin].time = req->time;
            bitset_clear(router->updated_stops);

            /* This is inefficient, as it depends on iterating over
             * a bitset with only one bit true.
             */
            bitset_set(router->updated_stops, router->origin);

            /* We will use round 1 to hold the initial state for round 0.
             * Round 1 must then be re-initialized before use.
             */
            router->best_time[router->origin] = req->time;
            router->states[1 * router->tdata->n_stops + router->origin].time    = req->time;
            /* the rest of these should be unnecessary */
            router->states[1 * router->tdata->n_stops + router->origin].ride_from  = NONE;
            router->states[1 * router->tdata->n_stops + router->origin].back_route = NONE;
            router->states[1 * router->tdata->n_stops + router->origin].back_trip  = NONE;
            router->states[1 * router->tdata->n_stops + router->origin].board_time = UNREACHED;

            /* Don't do apply transfers when we are explicitly routing from
             * a lat/lon */

       }
       /* Remove the banned stops from the bitset (do we really want to do
        * this here? this could only remove the origin stop.) */
       unflag_banned_stops(router, req);

       /* Apply transfers to initial state, which also initializes the updated
        * routes bitset. */
       apply_transfers(router, req, 1, true);

       /* De overweging om bij een alternatieve eerdere halte uit te stappen
        * en te gaan lopen baseren op het feit dat een andere, niet dezelfde
        * back_route wordt gebruikt
        */

       /*  dump_results(router); */
    }

    {
        uint8_t i_round;

        /* apply upper bounds (speeds up second and third reversed searches) */
        uint32_t n_rounds = req->max_transfers + 1;
        if (n_rounds > RRRR_DEFAULT_MAX_ROUNDS)
            n_rounds = RRRR_DEFAULT_MAX_ROUNDS;

        /*  Iterate over rounds. In round N, we have made N transfers. */
        for (i_round = 0; i_round < n_rounds; ++i_round) {   /*  < n_rounds to apply upper bound on transfers... */
            router_round(router, req, i_round);
        }  /*  end for (round) */
    }

    return true;
}

void router_round(router_t *router, router_request_t *req, uint8_t round) {
     /*  TODO restrict pointers? */
    uint32_t i_route;
    uint8_t last_round = (round == 0) ? 1 : round - 1;

    #ifdef RRRR_INFO
    printf("round %d\n", round);
    #endif
     /* Iterate over all routes which contain a stop that was updated in the last round. */
    for (i_route  = bitset_next_set_bit (router->updated_routes, 0);
         i_route != BITSET_NONE;
         i_route  = bitset_next_set_bit (router->updated_routes, i_route + 1)) {
        route_t *this_route = &(router->tdata->routes[i_route]);

        #ifdef FEATURE_AGENCY_FILTER
        if (req->agency != AGENCY_UNFILTERED && req->agency != route.agency_index) continue;
        #endif

        bool route_overlap = this_route->min_time < this_route->max_time - RTIME_ONE_DAY;
        /*
        if (route_overlap) printf ("min time %d max time %d overlap %d \n", route.min_time, route.max_time, route_overlap);
        printf ("route %d has min_time %d and max_time %d. \n", i_route, route.min_time, route.max_time);
        printf ("  actual first time: %d \n", tdata_depart(router->tdata, i_route, 0, 0));
        printf ("  actual last time:  %d \n", tdata_arrive(router->tdata, i_route, route.n_trips - 1, route.n_stops - 1));
        */
        #ifdef RRRR_INFO
        printf("  route %d: %s;%s\n", i_route, tdata_shortname_for_route(router->tdata, i_route),tdata_headsign_for_route(router->tdata, i_route));
        #endif
        #ifdef RRRR_TRIP
        tdata_dump_route(router->tdata, i_route, NONE);
        #endif
         /*  For each stop in this route, its global stop index. */
        uint32_t *route_stops = tdata_stops_for_route(router->tdata, i_route);
        uint8_t  *route_stop_attributes = tdata_stop_attributes_for_route(router->tdata, i_route);
        trip_t   *route_trips = tdata_trips_for_route(router->tdata, i_route);  /*  TODO use to avoid calculating at every stop */
        calendar_t *trip_masks  = tdata_trip_masks_for_route(router->tdata, i_route);
        uint32_t      trip = NONE;              /*  trip index within the route. NONE means not yet boarded. */
        uint32_t      board_stop = 0;           /*  stop index where that trip was boarded */
        uint32_t      board_route_stop = 0;     /*  route stop index where that trip was boarded */
        rtime_t       board_time = 0;           /*  time when that trip was boarded */
        serviceday_t *board_serviceday = NULL;  /*  Service day on which that trip was boarded */
        /*
            Iterate over stop indexes within the route. Each one corresponds to a global stop index.
            Note that the stop times array should be accessed with [trip][route_stop] not [trip][stop].
            The iteration variable is signed to allow ending the iteration at the beginning of the route.
        */

        /* We are descrementing to 0, if we want to test for >= 0 we must keep the variable signed */
        int32_t i_route_stop;
        for (i_route_stop = (req->arrive_by ? this_route->n_stops - 1 : 0);
                             req->arrive_by ? i_route_stop >= 0 : i_route_stop < this_route->n_stops;
                             req->arrive_by ? --i_route_stop : ++i_route_stop ) {

            uint32_t i_banned_stop_hard;

            uint32_t stop = route_stops[i_route_stop];
            rtime_t prev_time;
            bool attempt_board = false;
            #ifdef RRRR_INFO
            printf("    stop %2d [%d] %s %s\n", i_route_stop, stop,
                timetext(router->best_time[stop]), tdata_stop_name_for_index (router->tdata, stop));
            #endif

            /* If a stop in in banned_stop_hard, we do not want to transit through this station
             * we reset the current trip to NONE and skip the currect stop.
             * This effectively splits the route in two, and forces a re-board afterwards.
             */
            for (i_banned_stop_hard = 0; i_banned_stop_hard < req->n_banned_stops_hard; i_banned_stop_hard++) {
                if (stop == req->banned_stop_hard[i_banned_stop_hard]) {
                    trip = NONE;
                    continue;
                }
            }

            /*
                If we are not already on a trip, or if we might be able to board a better trip on
                this route at this location, indicate that we want to search for a trip.
            */
            prev_time = router->states[last_round * router->tdata->n_stops + stop].walk_time;
            if (prev_time != UNREACHED) {  /*  Only board at placed that have been reached. */
                if (trip == NONE || req->via == stop) {
                    attempt_board = true;
                } else if (trip != NONE && req->via != NONE && req->via == board_stop) {
                    attempt_board = false;
                } else {
                     /*  removed xfer slack for simplicity */
                     /*  is this repetitively triggering re-boarding searches along a single route? */
                    rtime_t trip_time = tdata_stoptime (router->tdata, i_route, trip, i_route_stop, req->arrive_by, board_serviceday);
                    if (trip_time == UNREACHED) attempt_board = false;
                    else if (req->arrive_by ? prev_time > trip_time
                                            : prev_time < trip_time) {
                        attempt_board = true;
                        #ifdef RRRR_INFO
                        printf ("    [reboarding here] trip = %s\n", timetext(trip_time));
                        #endif
                    }
                }
            }

            if (!(route_stop_attributes[i_route_stop] & rsa_boarding))  /* Boarding not allowed */
                if (req->arrive_by ? trip != NONE : attempt_board)  /* and we're attempting to board */
                    continue;  /* Boarding not allowed and attemping to board */
            if (!(route_stop_attributes[i_route_stop] & rsa_alighting))  /* Alighting not allowed */
                if (req->arrive_by ? attempt_board : trip != NONE)  /* and we're seeking to alight */
                    continue;  /* Alighting not allowed and attemping to alight */

            /* If we have not yet boarded a trip on this route, see if we can board one.
                Also handle the case where we hit a stop with an existing better arrival time. */
             /*  TODO: check if this is the last stop -- no point boarding there or marking routes */
            if (attempt_board) {
                #ifdef RRRR_INFO
                printf ("    attempting boarding at stop %d\n", stop);
                #endif
                #ifdef RRRR_TDATA
                tdata_dump_route(router->tdata, i_route, NONE);
                #endif
                /* Scan all trips to find the soonest trip that can be boarded, if any.
                    Real-time updates can ruin FIFO ordering of trips within routes.
                    Scanning through the whole list of trips reduces speed by ~20 percent over binary search. */
                uint32_t best_trip = NONE;
                rtime_t  best_time = req->arrive_by ? 0 : UINT16_MAX;
                serviceday_t *best_serviceday = NULL;
                serviceday_t *serviceday;
                /* Search trips within days. The loop nesting could also be inverted. */
                for (serviceday = router->servicedays; serviceday <= router->servicedays + 2; ++serviceday) {
                    uint32_t i_trip_offset;

                    /* Check that this route still has any trips running on this day. */
                    if (req->arrive_by ? prev_time < serviceday->midnight + this_route->min_time
                                       : prev_time > serviceday->midnight + this_route->max_time) continue;

                    /* Check whether there's any chance of improvement by scanning additional days. */
                    /* Note that day list is reversed for arrive-by searches. */
                    if (best_trip != NONE && ! route_overlap) break;

                    for (i_trip_offset = 0; i_trip_offset < this_route->n_trips; ++i_trip_offset) {
                        uint32_t i_banned_trip;
                        rtime_t time;

                        #ifdef RRRR_DEBUG
                        printBits(4, & (trip_masks[i_trip_offset]));
                        printBits(4, & (serviceday->mask));
                        printf("\n");
                        #endif

                        /* skip this trip if it is banned */
                        for (i_banned_trip = 0; i_banned_trip < req->n_banned_trips; i_banned_trip++)
                            if ( req->banned_trip_route[i_banned_trip]  == i_route &&
                                 req->banned_trip_offset[i_banned_trip] == i_trip_offset) continue;

                        /* skip this trip if it is not running on the current service day */
                        if ( ! (serviceday->mask & trip_masks[i_trip_offset])) continue;
                        /* skip this trip if it doesn't have all our required attributes */
                        if ( ! ((req->trip_attributes & route_trips[i_trip_offset].trip_attributes) == req->trip_attributes)) continue;

                        /* consider the arrival or departure time on the current service day */
                        time = tdata_stoptime (router->tdata, i_route, i_trip_offset, i_route_stop, req->arrive_by, serviceday);

                        #ifdef RRRR_TRIP
                        printf("    board option %d at %s \n", i_trip_offset, "");
                        #endif
                        if (time == UNREACHED) continue;  /*  rtime overflow due to long overnight trips on day 2 */
                        /* Mark trip for boarding if it improves on the last round's post-walk time at this stop.
                         * Note: we should /not/ be comparing to the current best known time at this stop, because
                         * it may have been updated in this round by another trip (in the pre-walk transit phase).
                         */
                        if (req->arrive_by ? time <= prev_time && time > best_time
                                           : time >= prev_time && time < best_time) {
                            best_trip = i_trip_offset;
                            best_time = time;
                            best_serviceday = serviceday;
                        }
                    }  /*  end for (trips within this route) */
                }  /*  end for (service days: yesterday, today, tomorrow) */
                if (best_trip != NONE) {
                    #ifdef RRRR_INFO
                    printf("    boarding trip %d at %s \n", best_trip, timetext(best_time));
                    #endif
                    if ((req->arrive_by ? best_time > req->time : best_time < req->time) && req->from != ONBOARD) {
                        printf("ERROR: boarded before start time, trip %d stop %d \n", best_trip, stop);
                    } else {
                         /*  use a router_state struct for all this? */
                        board_time = best_time;
                        board_stop = stop;
                        board_route_stop = i_route_stop;
                        board_serviceday = best_serviceday;
                        trip = best_trip;
                    }
                } else {
                    #ifdef RRRR_TRIP
printf("    no suitable trip to board.\n");
#endif
                }
                continue;  /*  to the next stop in the route */
            } else if (trip != NONE) {  /*  We have already boarded a trip along this route. */
                rtime_t time = tdata_stoptime (router->tdata, i_route, trip, i_route_stop, !req->arrive_by, board_serviceday);
                if (time == UNREACHED) continue;  /*  overflow due to long overnight trips on day 2 */
                #ifdef RRRR_TRIP
printf("    on board trip %d considering time %s \n", trip, timetext(time));
#endif
                 /*  Target pruning, sec. 3.1 of RAPTOR paper. */
                if ((router->best_time[router->target] != UNREACHED) &&
                    (req->arrive_by ? time < router->best_time[router->target]
                                    : time > router->best_time[router->target])) {
                    #ifdef RRRR_TRIP
printf("    (target pruning)\n");
#endif
                     /*  We cannot break out of this route entirely, because re-boarding may occur at a later stop. */
                    continue;
                }
                if ((req->time_cutoff != UNREACHED) &&
                    (req->arrive_by ? time < req->time_cutoff
                                    : time > req->time_cutoff)) {
                    continue;
                }
                 /*  Do we need best_time at all? yes, because the best time may not have been found in the previous round. */
                #if 0
                bool improved = (router->best_time[stop] == UNREACHED) ||
                                (req->arrive_by ? time > router->best_time[stop]
                                                : time < router->best_time[stop]);
                #endif
                if (!((router->best_time[stop] == UNREACHED) ||
                       (req->arrive_by ? time > router->best_time[stop]
                                       : time < router->best_time[stop]))) {
                    #ifdef RRRR_INFO
                    printf("    (no improvement)\n");
                    #endif
                    continue;  /*  the current trip does not improve on the best time at this stop */
                }
                if (time > RTIME_THREE_DAYS) {
                    /* Reserve all time past three days for special values like UNREACHED. */
                } else if (req->arrive_by ? time > req->time : time < req->time) {
                    /* Wrapping/overflow. This happens due to overnight trips on day 2. Prune them. */
                     /*  printf("ERROR: setting state to time before start time. route %d trip %d stop %d \n", i_route, trip, stop); */
                } else {  /*  TODO should alighting handled here? if ((route_stop_attributes[i_route_stop] & rsa_alighting) == rsa_alighting) */
                    router_state_t *this_state = &(router->states[round * router->tdata->n_stops + stop]);
                    #ifdef RRRR_INFO
                    printf("    setting stop to %s \n", timetext(time));
                    #endif
                    router->best_time[stop]     = time;
                    this_state->time            = time;
                    this_state->back_route      = i_route;
                    this_state->back_trip       = trip;
                    this_state->ride_from       = board_stop;
                    this_state->board_time      = board_time;
                    this_state->back_route_stop = board_route_stop;
                    this_state->route_stop      = i_route_stop;

                    if (req->arrive_by) {
                        if (board_time < time) printf ("board time non-decreasing\n");
                    } else {
                        if (board_time > time) printf ("board time non-increasing\n");
                    }
                    bitset_set(router->updated_stops, stop);    /*  mark stop for next round. */
                }
            }
        }  /*  end for (stop) */
    }  /*  end for (route) */
     /*  Remove the banned stops from the bitset, so no transfers will happen there. */
    unflag_banned_stops(router, req);
    /* Also updates the list of routes for next round based on stops that were touched in this round. */
    apply_transfers(router, req, round, true);
     /*  exit(0); */
    /* Initialize the stops in round 1 that were used as starting points for round 0. */
     /*  TODO: also must be done for the hashgrid */
     /*  if (round == 0) initialize_transfers (router, 1, router->origin); */
    if (round == 0) initialize_transfers_full (router, 1);

     /*  TODO add arrival hashgrid timings */

    /* dump_results(router);  DEBUG */

}


