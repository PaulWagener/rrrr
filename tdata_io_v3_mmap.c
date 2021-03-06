/* Copyright 2013 Bliksem Labs.
 * See the LICENSE file at the top-level directory of this distribution and at
 * https://github.com/bliksemlabs/rrrr/
 */

#include "config.h"

#ifdef RRRR_TDATA_IO_MMAP

#include "tdata_io_v3.h"
#include "tdata.h"
#include "rrrr_types.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#define load_mmap(b, storage, type) \
    td->n_##storage = header->n_##storage; \
    td->storage = (type *) (((char *) b) + header->loc_##storage)

#define load_mmap_string(b, storage) \
    td->n_##storage = header->n_##storage; \
    td->storage##_width = *((uint32_t *) (((char *) b) + header->loc_##storage)); \
    td->storage = (char*) (((char *) b) + header->loc_##storage + sizeof(uint32_t))

/* Set the maximum drivetime of any day in tdata */
void set_max_time(tdata_t *td){
    uint32_t jp_index;
    td->max_time = 0;
    for (jp_index = 0; jp_index < td->n_journey_patterns; jp_index++){
        if (td->journey_patterns[jp_index].max_time > td->max_time) {
            td->max_time = td->journey_patterns[jp_index].max_time;
        }
    }
}

/* Map an input file into memory and reconstruct pointers to its contents. */
bool tdata_io_v3_load(tdata_t *td, char *filename) {
    struct stat st;
    tdata_header_t *header;
    int fd;

    fd = open(filename, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "The input file %s could not be found.\n", filename);
        return false;
    }

    if (stat(filename, &st) == -1) {
        fprintf(stderr, "The input file %s could not be stat.\n", filename);
        goto fail_close_fd;
    }

    td->base = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    td->size = st.st_size;
    if (td->base == MAP_FAILED) {
        fprintf(stderr, "The input file %s could not be mapped.\n", filename);
        goto fail_close_fd;
    }

    header = (tdata_header_t *) td->base;
    if( strncmp("TTABLEV3", header->version_string, 8) ) {
        fprintf(stderr, "The input file %s does not appear to be a timetable or is of the wrong version.\n", filename);
        goto fail_munmap_base;
    }

    td->calendar_start_time = header->calendar_start_time;
    td->dst_active = header->dst_active;

    load_mmap (td->base, stops, stop_t);
    load_mmap (td->base, stop_attributes, uint8_t);
    load_mmap (td->base, stop_coords, latlon_t);
    load_mmap (td->base, journey_patterns, journey_pattern_t);
    load_mmap (td->base, journey_pattern_points, spidx_t);
    load_mmap (td->base, journey_pattern_point_attributes, uint8_t);
    load_mmap (td->base, stop_times, stoptime_t);
    load_mmap (td->base, vjs, vehicle_journey_t);
    load_mmap (td->base, journey_patterns_at_stop, uint32_t);
    load_mmap (td->base, transfer_target_stops, spidx_t);
    load_mmap (td->base, transfer_dist_meters, uint8_t);
    load_mmap (td->base, vj_active, calendar_t);
    load_mmap (td->base, journey_pattern_active, calendar_t);
    load_mmap (td->base, headsigns, char);
    load_mmap (td->base, stop_names, char);
    load_mmap (td->base, stop_nameidx, uint32_t);

    load_mmap_string (td->base, platformcodes);
    load_mmap_string (td->base, stop_ids);
    load_mmap_string (td->base, vj_ids);
    load_mmap_string (td->base, agency_ids);
    load_mmap_string (td->base, agency_names);
    load_mmap_string (td->base, agency_urls);
    load_mmap_string (td->base, line_codes);
    load_mmap_string (td->base, line_ids);
    load_mmap_string (td->base, productcategories);

    /* Set the maximum drivetime of any day in tdata */
    set_max_time(td);
    /* We must close the file descriptor otherwise we will
     * leak it. Because mmap has created a reference to it
     * there will not be a problem.
     */
    close (fd);

    return true;

fail_munmap_base:
    munmap(td->base, td->size);

fail_close_fd:
    close(fd);

    return false;
}

void tdata_io_v3_close(tdata_t *td) {
    munmap(td->base, td->size);
}

#else
void tdata_io_v3_mmap_not_available();
#endif /* RRRR_TDATA_IO_MMAP */
