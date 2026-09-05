#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>

#include "rrd_strtod.h"
#include "rrd_tool.h"
#include "rrd_graph.h"
#include "rrd_client.h"

#define conv_if(VV,VVV) \
    if (strcmp(#VV, string) == 0) return VVV;

#define LOCALTIME_R(a,b,c) (c ? gmtime_r(a,b) : localtime_r(a,b))

const char default_timestamp_fmt[] = "%Y-%m-%d %H:%M:%S";
const char default_duration_fmt[] = "%H:%02m:%02s";

struct rrd_hl_map *
rrd_hl_map_new(void)
{
    return calloc(1, sizeof(struct rrd_hl_map));
}

void
rrd_hl_map_free(struct rrd_hl_map *map)
{
    struct rrd_hl_map_entry *entry;
    struct rrd_hl_map_entry *next;

    if (map == NULL)
        return;

    for (entry = map->head; entry != NULL; entry = next) {
        next = entry->next;
        free(entry->key);
        free(entry);
    }

    free(map);
}

int
rrd_hl_map_get(
    const struct rrd_hl_map *map,
    const char *key,
    long *value)
{
    const struct rrd_hl_map_entry *entry;

    if (map == NULL)
        return 0;

    for (entry = map->head; entry != NULL; entry = entry->next) {
        if (strcmp(entry->key, key) == 0) {
            if (value != NULL)
                *value = entry->value;

            return 1;
        }
    }

    return 0;
}

int
rrd_hl_map_put(
    struct rrd_hl_map *map,
    char *key,
    long value)
{
    struct rrd_hl_map_entry *entry;

    if (map == NULL || key == NULL)
        return -1;

    for (entry = map->head; entry != NULL; entry = entry->next) {
        if (strcmp(entry->key, key) == 0) {
            free(key);
            entry->value = value;
            return 0;
        }
    }

    entry = malloc(sizeof(*entry));
    if (entry == NULL)
        return -1;

    entry->key = key;
    entry->value = value;
    entry->next = map->head;
    map->head = entry;

    return 0;
}

enum gf_en gf_conv(
    const char *string)
{
    conv_if(PRINT, GF_PRINT);
    conv_if(GPRINT, GF_GPRINT);
    conv_if(COMMENT, GF_COMMENT);
    conv_if(HRULE, GF_HRULE);
    conv_if(VRULE, GF_VRULE);
    conv_if(LINE, GF_LINE);
    conv_if(AREA, GF_AREA);
    conv_if(STACK, GF_STACK);
    conv_if(TICK, GF_TICK);
    conv_if(TEXTALIGN, GF_TEXTALIGN);
    conv_if(DEF, GF_DEF);
    conv_if(CDEF, GF_CDEF);
    conv_if(VDEF, GF_VDEF);
    conv_if(XPORT, GF_XPORT);
    conv_if(SHIFT, GF_SHIFT);

    return (enum gf_en) (-1);
}

#undef conv_if

int im_free(
    image_desc_t *im)
{
    unsigned long i, ii;

    if (im == NULL)
        return 0;

    free(im->graphfile);

    if (im->daemon_addr != NULL)
        free(im->daemon_addr);

    if (im->gdef_map) {
         rrd_hl_map_free(im->gdef_map);
    }

    if (im->rrd_map) {
         rrd_hl_map_free(im->rrd_map);
    }

    for (i = 0; i < (unsigned) im->gdes_c; i++) {
        if (im->gdes[i].data_first) {
            /*
             * Careful here, because a single pointer
             * can occur several times.
             */
            free(im->gdes[i].data);

            if (im->gdes[i].ds_namv) {
                for (ii = 0; ii < im->gdes[i].ds_cnt; ii++)
                    free(im->gdes[i].ds_namv[ii]);

                free(im->gdes[i].ds_namv);
            }
        }

        if (im->gdes[i].p_dashes != NULL)
            free(im->gdes[i].p_dashes);

        free(im->gdes[i].p_data);
        free(im->gdes[i].rpnp);
    }

    free(im->gdes);

    if (im->rendered_image)
        free(im->rendered_image);

    if (im->ylegend)
        free(im->ylegend);

    if (im->title)
        free(im->title);

    if (im->watermark)
        free(im->watermark);

    if (im->xlab_form)
        free(im->xlab_form);

    if (im->second_axis_legend)
        free(im->second_axis_legend);

    if (im->second_axis_format)
        free(im->second_axis_format);

    if (im->primary_axis_format)
        free(im->primary_axis_format);

    return 0;
}

int rrd_reduce_data(
    enum cf_en cf,      /* which consolidation function ? */
    unsigned long cur_step, /* step the data currently is in */
    time_t *start,      /* start, end and step as requested ... */
    time_t *end,        /* ... by the application will be   ... */
    unsigned long *step,    /* ... adjusted to represent reality    */
    unsigned long *ds_cnt,  /* number of data sources in file */
    rrd_value_t **data)
{                       /* two dimensional array containing the data */
    int       i, reduce_factor = ceil((double) (*step) / (double) cur_step);
    unsigned long col, dst_row, row_cnt, start_offset, end_offset, skiprows =
        0;
    rrd_value_t *srcptr, *dstptr;

    (*step) = cur_step * reduce_factor; /* set new step size for reduced data */
    dstptr = *data;
    srcptr = *data;
    row_cnt = ((*end) - (*start)) / cur_step;

#ifdef DEBUG
#define DEBUG_REDUCE
#endif
#ifdef DEBUG_REDUCE
    printf("Reducing %lu rows with factor %i time %lu to %lu, step %lu\n",
           row_cnt, reduce_factor, *start, *end, cur_step);
    for (col = 0; col < row_cnt; col++) {
        printf("time %10lu: ", *start + (col + 1) * cur_step);
        for (i = 0; i < *ds_cnt; i++)
            printf(" %8.2e", srcptr[*ds_cnt * col + i]);
        printf("\n");
    }
#endif

    /* We have to combine [reduce_factor] rows of the source
     ** into one row for the destination.  Doing this we also
     ** need to take care to combine the correct rows.  First
     ** alter the start and end time so that they are multiples
     ** of the new step time.  We cannot reduce the amount of
     ** time so we have to move the end towards the future and
     ** the start towards the past.
     */
    end_offset = (*end) % (*step);
    start_offset = (*start) % (*step);

    /* If there is a start offset (which cannot be more than
     ** one destination row), skip the appropriate number of
     ** source rows and one destination row.  The appropriate
     ** number is what we do know (start_offset/cur_step) of
     ** the new interval (*step/cur_step aka reduce_factor).
     */
#ifdef DEBUG_REDUCE
    printf("start_offset: %lu  end_offset: %lu\n", start_offset, end_offset);
    printf("row_cnt before:  %lu\n", row_cnt);
#endif
    if (start_offset) {
        (*start) = (*start) - start_offset;
        skiprows = reduce_factor - start_offset / cur_step;
        srcptr += skiprows * *ds_cnt;
        for (col = 0; col < (*ds_cnt); col++)
            *dstptr++ = DNAN;
        row_cnt -= skiprows;
    }
#ifdef DEBUG_REDUCE
    printf("row_cnt between: %lu\n", row_cnt);
#endif

    /* At the end we have some rows that are not going to be
     ** used, the amount is end_offset/cur_step
     */
    if (end_offset) {
        (*end) = (*end) - end_offset + (*step);
        skiprows = end_offset / cur_step;
        row_cnt -= skiprows;
    }
#ifdef DEBUG_REDUCE
    printf("row_cnt after:   %lu\n", row_cnt);
#endif

/* Sanity check: row_cnt should be multiple of reduce_factor */
/* if this gets triggered, something is REALLY WRONG ... we die immediately */

    if (row_cnt % reduce_factor) {
        rrd_set_error("SANITY CHECK: %lu rows cannot be reduced by %i \n",
                      row_cnt, reduce_factor);
        return 0;
    }

    /* Now combine reduce_factor intervals at a time
     ** into one interval for the destination.
     */

    for (dst_row = 0; (long int) row_cnt >= reduce_factor; dst_row++) {
        for (col = 0; col < (*ds_cnt); col++) {
            rrd_value_t newval = DNAN;
            unsigned long validval = 0;

            for (i = 0; i < reduce_factor; i++) {
                if (isnan(srcptr[i * (*ds_cnt) + col])) {
                    continue;
                }
                validval++;
                if (isnan(newval))
                    newval = srcptr[i * (*ds_cnt) + col];
                else {
                    switch (cf) {
                    case CF_HWPREDICT:
                    case CF_MHWPREDICT:
                    case CF_DEVSEASONAL:
                    case CF_DEVPREDICT:
                    case CF_SEASONAL:
                    case CF_AVERAGE:
                        newval += srcptr[i * (*ds_cnt) + col];
                        break;
                    case CF_MINIMUM:
                        newval = min(newval, srcptr[i * (*ds_cnt) + col]);
                        break;
                    case CF_FAILURES:
                        /* an interval contains a failure if any subintervals contained a failure */
                    case CF_MAXIMUM:
                        newval = max(newval, srcptr[i * (*ds_cnt) + col]);
                        break;
                    case CF_LAST:
                        newval = srcptr[i * (*ds_cnt) + col];
                        break;
                    }
                }
            }
            if (validval == 0) {
                newval = DNAN;
            } else {
                switch (cf) {
                case CF_HWPREDICT:
                case CF_MHWPREDICT:
                case CF_DEVSEASONAL:
                case CF_DEVPREDICT:
                case CF_SEASONAL:
                case CF_AVERAGE:
                    newval /= validval;
                    break;
                case CF_MINIMUM:
                case CF_FAILURES:
                case CF_MAXIMUM:
                case CF_LAST:
                    break;
                }
            }
            *dstptr++ = newval;
        }
        srcptr += (*ds_cnt) * reduce_factor;
        row_cnt -= reduce_factor;
    }
    /* If we had to alter the endtime, we didn't have enough
     ** source rows to fill the last row. Fill it with NaN.
     */
    if (end_offset)
        for (col = 0; col < (*ds_cnt); col++)
            *dstptr++ = DNAN;
#ifdef DEBUG_REDUCE
    row_cnt = ((*end) - (*start)) / *step;
    srcptr = *data;
    printf("Done reducing. Currently %lu rows, time %lu to %lu, step %lu\n",
           row_cnt, *start, *end, *step);
    for (col = 0; col < row_cnt; col++) {
        printf("time %10lu: ", *start + (col + 1) * (*step));
        for (i = 0; i < *ds_cnt; i++)
            printf(" %8.2e", srcptr[*ds_cnt * col + i]);
        printf("\n");
    }
#endif
    return 1;
}


/* get the data required for the graphs from the
   relevant rrds ... */

int data_fetch(
    image_desc_t *im)
{
    int       i, ii;

    /* pull the data from the rrd files ... */
    for (i = 0; i < (int) im->gdes_c; i++) {
        /* only GF_DEF elements fetch data */
        if (im->gdes[i].gf != GF_DEF)
            continue;

        /* do we have it already ? */
        long value;
        char *key = gdes_fetch_key(im->gdes[i]);
        int ok = rrd_hl_map_get(im->rrd_map, key, &value);
        free(key);
        if (ok) {
            ii = (int) value;
            im->gdes[i].start = im->gdes[ii].start;
            im->gdes[i].end = im->gdes[ii].end;
            im->gdes[i].step = im->gdes[ii].step;
            im->gdes[i].ds_cnt = im->gdes[ii].ds_cnt;
            im->gdes[i].ds_namv = im->gdes[ii].ds_namv;
            im->gdes[i].data = im->gdes[ii].data;
            im->gdes[i].data_first = 0;
        } else {
            unsigned long ft_step = im->gdes[i].step;   /* ft_step will record what we got from fetch */
            const char *rrd_daemon;
            int       status;

            if (im->gdes[i].daemon[0] != 0)
                rrd_daemon = im->gdes[i].daemon;
            else
                rrd_daemon = im->daemon_addr;

            /* "daemon" may be NULL. ENV_RRDCACHED_ADDRESS is evaluated in that
             * case. If "daemon" holds the same value as in the previous
             * iteration, no actual new connection is established - the
             * existing connection is re-used. */
            rrdc_connect(rrd_daemon);

            /* If connecting was successful, use the daemon to query the data.
             * If there is no connection, for example because no daemon address
             * was specified, (try to) use the local file directly. */
            if (rrdc_is_connected(rrd_daemon)) {
                status = rrdc_fetch(im->gdes[i].rrd,
                                    cf_to_string(im->gdes[i].cf),
                                    &im->gdes[i].start,
                                    &im->gdes[i].end,
                                    &ft_step,
                                    &im->gdes[i].ds_cnt,
                                    &im->gdes[i].ds_namv, &im->gdes[i].data);
                if (status != 0) {
                    if (im->extra_flags & ALLOW_MISSING_DS) {
                        rrd_clear_error();
                        if (rrd_fetch_empty(&im->gdes[i].start,
                                            &im->gdes[i].end,
                                            &ft_step,
                                            &im->gdes[i].ds_cnt,
                                            im->gdes[i].ds_nam,
                                            &im->gdes[i].ds_namv,
                                            &im->gdes[i].data) == -1)
                            return -1;
                    } else
                        return (status);
                }
            } else {
                if ((rrd_fetch_fn(im->gdes[i].rrd,
                                  im->gdes[i].cf,
                                  &im->gdes[i].start,
                                  &im->gdes[i].end,
                                  &ft_step,
                                  &im->gdes[i].ds_cnt,
                                  &im->gdes[i].ds_namv,
                                  &im->gdes[i].data)) == -1) {
                    if (im->extra_flags & ALLOW_MISSING_DS) {
                        /* Unable to fetch data, assume fake data */
                        rrd_clear_error();
                        if (rrd_fetch_empty(&im->gdes[i].start,
                                            &im->gdes[i].end,
                                            &ft_step,
                                            &im->gdes[i].ds_cnt,
                                            im->gdes[i].ds_nam,
                                            &im->gdes[i].ds_namv,
                                            &im->gdes[i].data) == -1)
                            return -1;
                    } else
                        return -1;
                }
            }
            im->gdes[i].data_first = 1;

            /* must reduce to at least im->step
               otherwise we end up with more data than we can handle in the
               chart and visibility of data will be random */
            im->gdes[i].step = max(im->gdes[i].step, im->step);
            if (ft_step < im->gdes[i].step) {

                if (!rrd_reduce_data
                    (im->gdes[i].cf_reduce_set ? im->gdes[i].cf_reduce : im->
                     gdes[i].cf, ft_step, &im->gdes[i].start,
                     &im->gdes[i].end, &im->gdes[i].step, &im->gdes[i].ds_cnt,
                     &im->gdes[i].data)) {
                    return -1;
                }
            } else {
                im->gdes[i].step = ft_step;
            }
        }

        /* lets see if the required data source is really there */
        for (ii = 0; ii < (int) im->gdes[i].ds_cnt; ii++) {
            if (strcmp(im->gdes[i].ds_namv[ii], im->gdes[i].ds_nam) == 0) {
                im->gdes[i].ds = ii;
            }
        }
        if ((im->gdes[i].ds == -1) && !(im->extra_flags & ALLOW_MISSING_DS)) {
            rrd_set_error("No DS called '%s' in '%s'",
                          im->gdes[i].ds_nam, im->gdes[i].rrd);
            return -1;
        }
        // remember that we already got this one
        rrd_hl_map_put(im->rrd_map,
               gdes_fetch_key(im->gdes[i]),
               i);
    }
    return 0;
}

/* evaluate the expressions in the CDEF functions */

/*************************************************************
 * CDEF stuff
 *************************************************************/


/* find the greatest common divisor for all the numbers
   in the 0 terminated num array */
long rrd_lcd(
    long *num)
{
    long      rest;
    int       i;

    for (i = 0; num[i + 1] != 0; i++) {
        do {
            rest = num[i] % num[i + 1];
            num[i] = num[i + 1];
            num[i + 1] = rest;
        } while (rest != 0);
        num[i + 1] = num[i];
    }
/*    return i==0?num[i]:num[i-1]; */
    return num[i];
}


/* run the rpn calculator on all the VDEF and CDEF arguments */
int data_calc(
    image_desc_t *im)
{

    int       gdi;
    int       dataidx;
    long     *steparray, rpi;
    long     *steparray_tmp;    /* temp variable for realloc() */
    int       stepcnt;
    time_t    now;
    rpnstack_t rpnstack;
    rpnp_t   *rpnp;

    rpnstack_init(&rpnstack);

    for (gdi = 0; gdi < im->gdes_c; gdi++) {
        /* Look for GF_VDEF and GF_CDEF in the same loop,
         * so CDEFs can use VDEFs and vice versa
         */
        switch (im->gdes[gdi].gf) {
        case GF_XPORT:
            break;
        case GF_SHIFT:{
            graph_desc_t *vdp = &im->gdes[im->gdes[gdi].vidx];

            /* remove current shift */
            vdp->start -= vdp->shift;
            vdp->end -= vdp->shift;

            /* vdef */
            if (im->gdes[gdi].shidx >= 0)
                vdp->shift = im->gdes[im->gdes[gdi].shidx].vf.val;
            /* constant */
            else
                vdp->shift = im->gdes[gdi].shval;

            /* normalize shift to multiple of consolidated step */
            vdp->shift = (vdp->shift / (long) vdp->step) * (long) vdp->step;

            /* apply shift */
            vdp->start += vdp->shift;
            vdp->end += vdp->shift;
            break;
        }
        case GF_VDEF:
            /* A VDEF has no DS.  This also signals other parts
             * of rrdtool that this is a VDEF value, not a CDEF.
             */
            im->gdes[gdi].ds_cnt = 0;
            if (vdef_calc(im, gdi)) {
                rrd_set_error("Error processing VDEF '%s'",
                              im->gdes[gdi].vname);
                rpnstack_free(&rpnstack);
                return -1;
            }
            break;
        case GF_CDEF:
            im->gdes[gdi].ds_cnt = 1;
            im->gdes[gdi].ds = 0;
            im->gdes[gdi].data_first = 1;
            im->gdes[gdi].start = 0;
            im->gdes[gdi].end = 0;
            steparray = NULL;
            stepcnt = 0;
            dataidx = -1;
            rpnp = im->gdes[gdi].rpnp;

            /* Find the variables in the expression.
             * - VDEF variables are substituted by their values
             *   and the opcode is changed into OP_NUMBER.
             * - CDEF variables are analyzed for their step size,
             *   the lowest common denominator of all the step
             *   sizes of the data sources involved is calculated
             *   and the resulting number is the step size for the
             *   resulting data source.
             */
            for (rpi = 0; im->gdes[gdi].rpnp[rpi].op != OP_END; rpi++) {
                if (im->gdes[gdi].rpnp[rpi].op == OP_VARIABLE ||
                    im->gdes[gdi].rpnp[rpi].op == OP_PREV_OTHER) {
                    long      ptr = im->gdes[gdi].rpnp[rpi].ptr;

                    if (im->gdes[ptr].ds_cnt == 0) {    /* this is a VDEF data source */
#if 0
                        printf
                            ("DEBUG: inside CDEF '%s' processing VDEF '%s'\n",
                             im->gdes[gdi].vname, im->gdes[ptr].vname);
                        printf("DEBUG: value from vdef is %f\n",
                               im->gdes[ptr].vf.val);
#endif
                        im->gdes[gdi].rpnp[rpi].val = im->gdes[ptr].vf.val;
                        im->gdes[gdi].rpnp[rpi].op = OP_NUMBER;
                    } else {    /* normal variables and PREF(variables) */

                        /* add one entry to the array that keeps track of the step sizes of the
                         * data sources going into the CDEF. */
                        if ((steparray_tmp =
                             (long *) rrd_realloc(steparray,
                                                  (++stepcnt +
                                                   1) *
                                                  sizeof(*steparray))) ==
                            NULL) {
                            rrd_set_error("realloc steparray");
                            rpnstack_free(&rpnstack);
                            return -1;
                        };
                        steparray = steparray_tmp;

                        steparray[stepcnt - 1] = im->gdes[ptr].step;

                        /* adjust start and end of cdef (gdi) so
                         * that it runs from the latest start point
                         * to the earliest endpoint of any of the
                         * rras involved (ptr)
                         */

                        if (im->gdes[gdi].start < im->gdes[ptr].start)
                            im->gdes[gdi].start = im->gdes[ptr].start;

                        if (im->gdes[gdi].end == 0 ||
                            im->gdes[gdi].end > im->gdes[ptr].end)
                            im->gdes[gdi].end = im->gdes[ptr].end;

                        /* store pointer to the first element of
                         * the rra providing data for variable,
                         * further save step size and data source
                         * count of this rra
                         */
                        im->gdes[gdi].rpnp[rpi].data =
                            im->gdes[ptr].data + im->gdes[ptr].ds;
                        im->gdes[gdi].rpnp[rpi].step = im->gdes[ptr].step;
                        im->gdes[gdi].rpnp[rpi].ds_cnt = im->gdes[ptr].ds_cnt;

                        /* backoff the *.data ptr; this is done so
                         * rpncalc() function doesn't have to treat
                         * the first case differently
                         */
                    }   /* if ds_cnt != 0 */
                }       /* if OP_VARIABLE */
            }           /* loop through all rpi */

            /* move the data pointers to the correct period */
            for (rpi = 0; im->gdes[gdi].rpnp[rpi].op != OP_END; rpi++) {
                if (im->gdes[gdi].rpnp[rpi].op == OP_VARIABLE ||
                    im->gdes[gdi].rpnp[rpi].op == OP_PREV_OTHER) {
                    long      ptr = im->gdes[gdi].rpnp[rpi].ptr;
                    long      diff =
                        im->gdes[gdi].start - im->gdes[ptr].start;

                    if (diff > 0)
                        im->gdes[gdi].rpnp[rpi].data +=
                            (diff / im->gdes[ptr].step) *
                            im->gdes[ptr].ds_cnt;
                }
            }

            if (steparray == NULL) {
                rrd_set_error("rpn expressions without DEF"
                              " or CDEF variables are not supported");
                rpnstack_free(&rpnstack);
                return -1;
            }
            steparray[stepcnt] = 0;
            /* Now find the resulting step.  All steps in all
             * used RRAs have to be visited
             */
            im->gdes[gdi].step = rrd_lcd(steparray);
            free(steparray);

            if ((im->gdes[gdi].data = (rrd_value_t *)
                 malloc(((im->gdes[gdi].end - im->gdes[gdi].start)
                         / im->gdes[gdi].step)
                        * sizeof(double))) == NULL) {
                rrd_set_error("malloc im->gdes[gdi].data");
                rpnstack_free(&rpnstack);
                return -1;
            }

            /* Step through the new cdef results array and
             * calculate the values
             */
            for (now = im->gdes[gdi].start + im->gdes[gdi].step;
                 now <= im->gdes[gdi].end; now += im->gdes[gdi].step) {

                /* 3rd arg of rpn_calc is for OP_VARIABLE lookups;
                 * in this case we are advancing by timesteps;
                 * we use the fact that time_t is a synonym for long
                 */
                if (rpn_calc(rpnp, &rpnstack, (long) now,
                             im->gdes[gdi].data, ++dataidx,
                             im->gdes[gdi].step) == -1) {
                    /* rpn_calc sets the error string */
                    rpnstack_free(&rpnstack);
                    rpnp_freeextra(rpnp);
                    return -1;
                }
            }           /* enumerate over time steps within a CDEF */
            rpnp_freeextra(rpnp);

            break;
        default:
            continue;
        }
    }                   /* enumerate over CDEFs */
    rpnstack_free(&rpnstack);
    return 0;
}

struct gfx_color_t gfx_hex_to_col(
    long unsigned int color)
{
    struct gfx_color_t gfx_color;

    gfx_color.red = 1.0 / 255.0 * ((color & 0xff000000) >> (3 * 8));
    gfx_color.green = 1.0 / 255.0 * ((color & 0x00ff0000) >> (2 * 8));
    gfx_color.blue = 1.0 / 255.0 * ((color & 0x0000ff00) >> (1 * 8));
    gfx_color.alpha = 1.0 / 255.0 * (color & 0x000000ff);
    return gfx_color;
}
int gdes_alloc(
    image_desc_t *im)
{

    im->gdes_c++;
    if ((im->gdes = (graph_desc_t *)
         rrd_realloc(im->gdes, (im->gdes_c)
                     * sizeof(graph_desc_t))) == NULL) {
        rrd_set_error("realloc graph_descs");
        return -1;
    }

    /* set to zero */
    memset(&(im->gdes[im->gdes_c - 1]), 0, sizeof(graph_desc_t));

    im->gdes[im->gdes_c - 1].step = im->step;
    im->gdes[im->gdes_c - 1].step_orig = im->step;
    im->gdes[im->gdes_c - 1].stack = 0;
    im->gdes[im->gdes_c - 1].skipscale = 0;
    im->gdes[im->gdes_c - 1].linewidth = 0;
    im->gdes[im->gdes_c - 1].debug = 0;
    im->gdes[im->gdes_c - 1].start = im->start;
    im->gdes[im->gdes_c - 1].start_orig = im->start;
    im->gdes[im->gdes_c - 1].end = im->end;
    im->gdes[im->gdes_c - 1].end_orig = im->end;
    im->gdes[im->gdes_c - 1].vname[0] = '\0';
    im->gdes[im->gdes_c - 1].data = NULL;
    im->gdes[im->gdes_c - 1].ds_namv = NULL;
    im->gdes[im->gdes_c - 1].data_first = 0;
    im->gdes[im->gdes_c - 1].p_data = NULL;
    im->gdes[im->gdes_c - 1].rpnp = NULL;
    im->gdes[im->gdes_c - 1].p_dashes = NULL;
    im->gdes[im->gdes_c - 1].shift = 0.0;
    im->gdes[im->gdes_c - 1].dash = 0;
    im->gdes[im->gdes_c - 1].ndash = 0;
    im->gdes[im->gdes_c - 1].offset = 0;
    im->gdes[im->gdes_c - 1].col.red = 0.0;
    im->gdes[im->gdes_c - 1].col.green = 0.0;
    im->gdes[im->gdes_c - 1].col.blue = 0.0;
    im->gdes[im->gdes_c - 1].col.alpha = 0.0;
    im->gdes[im->gdes_c - 1].col2.red = DNAN;
    im->gdes[im->gdes_c - 1].col2.green = DNAN;
    im->gdes[im->gdes_c - 1].col2.blue = DNAN;
    im->gdes[im->gdes_c - 1].col2.alpha = 0.0;
    im->gdes[im->gdes_c - 1].gradheight = 50.0;
    im->gdes[im->gdes_c - 1].legend[0] = '\0';
    im->gdes[im->gdes_c - 1].format[0] = '\0';
    im->gdes[im->gdes_c - 1].strftm = 0;
    im->gdes[im->gdes_c - 1].vformatter = VALUE_FORMATTER_NUMERIC;
    im->gdes[im->gdes_c - 1].rrd[0] = '\0';
    im->gdes[im->gdes_c - 1].ds = -1;
    im->gdes[im->gdes_c - 1].cf_reduce = CF_AVERAGE;
    im->gdes[im->gdes_c - 1].cf_reduce_set = 0;
    im->gdes[im->gdes_c - 1].cf = CF_AVERAGE;
    im->gdes[im->gdes_c - 1].yrule = DNAN;
    im->gdes[im->gdes_c - 1].xrule = 0;
    im->gdes[im->gdes_c - 1].daemon[0] = 0;
    return 0;
}

void rrd_graph_init(
    image_desc_t *im,
    enum image_init_en init_mode)
{
    /* zero the whole structure first */
    memset(im, 0, sizeof(image_desc_t));

#ifdef HAVE_TZSET
    tzset();
#endif

    im->gdef_map = rrd_hl_map_new();

    /*
     * Key is allocated by malloc() in sprintf_alloc(),
     * therefore free() must be used here.
     */
    im->rrd_map = rrd_hl_map_new();

    im->graph_type = GTYPE_TIME;
    im->base = 1000;
    im->daemon_addr = NULL;
    im->draw_x_grid = 1;
    im->draw_y_grid = 1;
    im->draw_3d_border = 2;
    im->dynamic_labels = 0;
    im->extra_flags = 0;
    im->forceleftspace = 0;
    im->gdes_c = 0;
    im->gdes = NULL;
    im->grid_dash_off = 1;
    im->grid_dash_on = 1;
    im->gridfit = 1;
    im->grinfo = (rrd_info_t *) NULL;
    im->grinfo_current = (rrd_info_t *) NULL;
    im->imgformat = IF_PNG;
    im->imginfo = NULL;
    im->lazy = 0;
    im->legenddirection = TOP_DOWN;
    im->legendheight = 0;
    im->legendposition = SOUTH;
    im->legendwidth = 0;
    im->logarithmic = 0;
    im->maxval = DNAN;
    im->minval = DNAN;
    im->magfact = 1;
    im->prt_c = 0;
    im->rigid = 0;
    im->allow_shrink = 0;
    im->rendered_image_size = 0;
    im->rendered_image = NULL;
    im->slopemode = 0;
    im->step = 0;
    im->symbol = ' ';
    im->tabwidth = 40.0;
    im->title = NULL;
    im->unitsexponent = 9999;
    im->unitslength = 6;
    im->viewfactor = 1.0;
    im->watermark = NULL;
    im->xlab_form = NULL;
    im->with_markup = 0;
    im->ximg = 0;
    im->xlab_user.minsec = -1.0;
    im->xorigin = 0;
    im->xOriginLegend = 0;
    im->xOriginLegendY = 0;
    im->xOriginLegendY2 = 0;
    im->xOriginTitle = 0;
    im->xsize = 400;
    im->ygridstep = DNAN;
    im->yimg = 0;
    im->ylegend = NULL;
    im->ylegend_angle = RRDGRAPH_YLEGEND_ANGLE;
    im->second_axis_scale = 0;
    im->second_axis_shift = 0;
    im->second_axis_legend = NULL;
    im->second_axis_legend_angle = RRDGRAPH_YLEGEND_ANGLE;
    im->second_axis_format = NULL;
    im->second_axis_formatter = VALUE_FORMATTER_NUMERIC;
    im->second_axis_range_min = DNAN;
    im->second_axis_range_max = DNAN;
    im->primary_axis_format = NULL;
    im->primary_axis_formatter = VALUE_FORMATTER_NUMERIC;
    im->yorigin = 0;
    im->yOriginLegend = 0;
    im->yOriginLegendY = 0;
    im->yOriginLegendY2 = 0;
    im->yOriginTitle = 0;
    im->ysize = 100;
    im->zoom = 1;
    im->init_mode = init_mode;
    im->last_tabwidth = -1;
}

int vdef_parse(
    struct graph_desc_t
    *gdes,
    const char *const str)
{
    /* A VDEF currently is either "func" or "param,func"
     * so the parsing is rather simple.  Change if needed.
     */
    double    param;
    char      func[30] = { 0 }, double_str[41] = { 0 };
    int       n;

    n = 0;
    sscanf(str, "%40[0-9.e+-],%29[A-Z]%n", double_str, func, &n);
    if (rrd_strtodbl(double_str, NULL, &param, NULL) != 2) {
        n = 0;
        sscanf(str, "%29[A-Z]%n", func, &n);
        if (n == (int) strlen(str)) {   /* matched */
            param = DNAN;
        } else {
            rrd_set_error
                ("Unknown function string '%s' in VDEF '%s'",
                 str, gdes->vname);
            return -1;
        }
    }
    if (!strcmp("PERCENT", func))
        gdes->vf.op = VDEF_PERCENT;
    else if (!strcmp("PERCENTNAN", func))
        gdes->vf.op = VDEF_PERCENTNAN;
    else if (!strcmp("MAXIMUM", func))
        gdes->vf.op = VDEF_MAXIMUM;
    else if (!strcmp("AVERAGE", func))
        gdes->vf.op = VDEF_AVERAGE;
    else if (!strcmp("STDEV", func))
        gdes->vf.op = VDEF_STDEV;
    else if (!strcmp("MINIMUM", func))
        gdes->vf.op = VDEF_MINIMUM;
    else if (!strcmp("TOTAL", func))
        gdes->vf.op = VDEF_TOTAL;
    else if (!strcmp("FIRST", func))
        gdes->vf.op = VDEF_FIRST;
    else if (!strcmp("LAST", func))
        gdes->vf.op = VDEF_LAST;
    else if (!strcmp("LSLSLOPE", func))
        gdes->vf.op = VDEF_LSLSLOPE;
    else if (!strcmp("LSLINT", func))
        gdes->vf.op = VDEF_LSLINT;
    else if (!strcmp("LSLCORREL", func))
        gdes->vf.op = VDEF_LSLCORREL;
    else {
        rrd_set_error
            ("Unknown function '%s' in VDEF '%s'\n", func, gdes->vname);
        return -1;
    };
    switch (gdes->vf.op) {
    case VDEF_PERCENT:
    case VDEF_PERCENTNAN:
        if (isnan(param)) { /* no parameter given */
            rrd_set_error
                ("Function '%s' needs parameter in VDEF '%s'\n",
                 func, gdes->vname);
            return -1;
        };
        if (param >= 0.0 && param <= 100.0) {
            gdes->vf.param = param;
            gdes->vf.val = DNAN;    /* undefined */
            gdes->vf.when = 0;  /* undefined */
            gdes->vf.never = 1;
        } else {
            rrd_set_error
                ("Parameter '%f' out of range in VDEF '%s'\n",
                 param, gdes->vname);
            return -1;
        };
        break;
    case VDEF_MAXIMUM:
    case VDEF_AVERAGE:
    case VDEF_STDEV:
    case VDEF_MINIMUM:
    case VDEF_TOTAL:
    case VDEF_FIRST:
    case VDEF_LAST:
    case VDEF_LSLSLOPE:
    case VDEF_LSLINT:
    case VDEF_LSLCORREL:
        if (isnan(param)) {
            gdes->vf.param = DNAN;
            gdes->vf.val = DNAN;
            gdes->vf.when = 0;
            gdes->vf.never = 1;
        } else {
            rrd_set_error
                ("Function '%s' needs no parameter in VDEF '%s'\n",
                 func, gdes->vname);
            return -1;
        };
        break;
    };
    return 0;
}


int vdef_calc(
    image_desc_t *im,
    int gdi)
{
    graph_desc_t *src, *dst;
    rrd_value_t *data;
    long      step, steps;

    dst = &im->gdes[gdi];
    src = &im->gdes[dst->vidx];
    data = src->data + src->ds;

    steps = (src->end - src->start) / src->step;
#if 0
    printf
        ("DEBUG: start == %lu, end == %lu, %lu steps\n",
         src->start, src->end, steps);
#endif
    switch (dst->vf.op) {
    case VDEF_PERCENT:{
        rrd_value_t *array;
        int       field;

        if (steps == 0) {
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
            break;
        }
        if ((array = (rrd_value_t *) malloc(steps * sizeof(double))) == NULL) {
            rrd_set_error("malloc VDEV_PERCENT");
            return -1;
        }
        for (step = 0; step < steps; step++) {
            array[step] = data[step * src->ds_cnt];
        }
        qsort(array, step, sizeof(double), vdef_percent_compar);
        field = round((dst->vf.param * (double) (steps - 1)) / 100.0);
        dst->vf.val = array[field];
        dst->vf.when = 0;   /* no time component */
        dst->vf.never = 1;
        free(array);
#if 0
        for (step = 0; step < steps; step++)
            printf("DEBUG: %3li:%10.2f %c\n",
                   step, array[step], step == field ? '*' : ' ');
#endif
    }
        break;
    case VDEF_PERCENTNAN:{
        rrd_value_t *array;
        int       field;

        /* count number of "valid" values */
        int       nancount = 0;

        for (step = 0; step < steps; step++) {
            if (!isnan(data[step * src->ds_cnt])) {
                nancount++;
            }
        }
        /* and allocate it */
        if (nancount == 0) {
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
            break;
        }
        if ((array =
             (rrd_value_t *) malloc(nancount * sizeof(double))) == NULL) {
            rrd_set_error("malloc VDEV_PERCENT");
            return -1;
        }
        /* and fill it in */
        field = 0;
        for (step = 0; step < steps; step++) {
            if (!isnan(data[step * src->ds_cnt])) {
                array[field] = data[step * src->ds_cnt];
                field++;
            }
        }
        qsort(array, nancount, sizeof(double), vdef_percent_compar);
        field = round(dst->vf.param * (double) (nancount - 1) / 100.0);
        dst->vf.val = array[field];
        dst->vf.when = 0;   /* no time component */
        dst->vf.never = 1;
        free(array);
    }
        break;
    case VDEF_MAXIMUM:
        step = 0;
        while (step != steps && isnan(data[step * src->ds_cnt]))
            step++;
        if (step == steps) {
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
        } else {
            dst->vf.val = data[step * src->ds_cnt];
            dst->vf.when = src->start + (step + 1) * src->step;
            dst->vf.never = 0;
        }
        while (step != steps) {
            if (finite(data[step * src->ds_cnt])) {
                if (data[step * src->ds_cnt] > dst->vf.val) {
                    dst->vf.val = data[step * src->ds_cnt];
                    dst->vf.when = src->start + (step + 1) * src->step;
                    dst->vf.never = 0;
                }
            }
            step++;
        }
        break;
    case VDEF_TOTAL:
    case VDEF_STDEV:
    case VDEF_AVERAGE:{
        int       cnt = 0;
        double    sum = 0.0;
        double    average = 0.0;

        for (step = 0; step < steps; step++) {
            if (finite(data[step * src->ds_cnt])) {
                sum += data[step * src->ds_cnt];
                cnt++;
            };
        }
        if (cnt) {
            if (dst->vf.op == VDEF_TOTAL) {
                dst->vf.val = sum * src->step;
                dst->vf.when = 0;   /* no time component */
                dst->vf.never = 1;
            } else if (dst->vf.op == VDEF_AVERAGE) {
                dst->vf.val = sum / cnt;
                dst->vf.when = 0;   /* no time component */
                dst->vf.never = 1;
            } else {
                average = sum / cnt;
                sum = 0.0;
                for (step = 0; step < steps; step++) {
                    if (finite(data[step * src->ds_cnt])) {
                        sum += pow((data[step * src->ds_cnt] - average), 2.0);
                    };
                }
                dst->vf.val = pow(sum / cnt, 0.5);
                dst->vf.when = 0;   /* no time component */
                dst->vf.never = 1;
            };
        } else {
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
        }
    }
        break;
    case VDEF_MINIMUM:
        step = 0;
        while (step != steps && isnan(data[step * src->ds_cnt]))
            step++;
        if (step == steps) {
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
        } else {
            dst->vf.val = data[step * src->ds_cnt];
            dst->vf.when = src->start + (step + 1) * src->step;
            dst->vf.never = 0;
        }
        while (step != steps) {
            if (finite(data[step * src->ds_cnt])) {
                if (data[step * src->ds_cnt] < dst->vf.val) {
                    dst->vf.val = data[step * src->ds_cnt];
                    dst->vf.when = src->start + (step + 1) * src->step;
                    dst->vf.never = 0;
                }
            }
            step++;
        }
        break;
    case VDEF_FIRST:
        /* The time value returned here is one step before the
         * actual time value.  This is the start of the first
         * non-NaN interval.
         */
        step = 0;
        while (step != steps && isnan(data[step * src->ds_cnt]))
            step++;
        if (step == steps) {    /* all entries were NaN */
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
        } else {
            dst->vf.val = data[step * src->ds_cnt];
            dst->vf.when = src->start + step * src->step;
            dst->vf.never = 0;
        }
        break;
    case VDEF_LAST:
        /* The time value returned here is the
         * actual time value.  This is the end of the last
         * non-NaN interval.
         */
        step = steps - 1;
        while (step >= 0 && isnan(data[step * src->ds_cnt]))
            step--;
        if (step < 0) { /* all entries were NaN */
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
        } else {
            dst->vf.val = data[step * src->ds_cnt];
            dst->vf.when = src->start + (step + 1) * src->step;
            dst->vf.never = 0;
        }
        break;
    case VDEF_LSLSLOPE:
    case VDEF_LSLINT:
    case VDEF_LSLCORREL:{
        /* Bestfit line by linear least squares method */

        int       cnt = 0;
        double    SUMx, SUMy, SUMxy, SUMxx, SUMyy, slope, y_intercept, correl;

        SUMx = 0;
        SUMy = 0;
        SUMxy = 0;
        SUMxx = 0;
        SUMyy = 0;
        for (step = 0; step < steps; step++) {
            if (finite(data[step * src->ds_cnt])) {
                cnt++;
                SUMx += step;
                SUMxx += step * step;
                SUMxy += step * data[step * src->ds_cnt];
                SUMy += data[step * src->ds_cnt];
                SUMyy += data[step * src->ds_cnt] * data[step * src->ds_cnt];
            };
        }

        slope = (SUMx * SUMy - cnt * SUMxy) / (SUMx * SUMx - cnt * SUMxx);
        y_intercept = (SUMy - slope * SUMx) / cnt;
        correl =
            (SUMxy -
             (SUMx * SUMy) / cnt) /
            sqrt((SUMxx -
                  (SUMx * SUMx) / cnt) * (SUMyy - (SUMy * SUMy) / cnt));
        if (cnt) {
            if (dst->vf.op == VDEF_LSLSLOPE) {
                dst->vf.val = slope;
                dst->vf.when = 0;
                dst->vf.never = 1;
            } else if (dst->vf.op == VDEF_LSLINT) {
                dst->vf.val = y_intercept;
                dst->vf.when = 0;
                dst->vf.never = 1;
            } else if (dst->vf.op == VDEF_LSLCORREL) {
                dst->vf.val = correl;
                dst->vf.when = 0;
                dst->vf.never = 1;
            };
        } else {
            dst->vf.val = DNAN;
            dst->vf.when = 0;
            dst->vf.never = 1;
        }
    }
        break;
    }
    return 0;
}

/* NaN < -INF < finite_values < INF */
int vdef_percent_compar(
    const void
    *a,
    const void
    *b)
{
    /* Equality is not returned; this doesn't hurt except
     * (maybe) for a little performance.
     */

    /* First catch NaN values. They are smallest */
    if (isnan(*(double *) a))
        return -1;
    if (isnan(*(double *) b))
        return 1;
    /* NaN doesn't reach this part so INF and -INF are extremes.
     * The sign from isinf() is compatible with the sign we return
     */
    if (isinf(*(double *) a))
        return isinf(*(double *) a);
    if (isinf(*(double *) b))
        return isinf(*(double *) b);
    /* If we reach this, both values must be finite */
    if (*(double *) a < *(double *) b)
        return -1;
    else
        return 1;
}

void grinfo_push(
    image_desc_t *im,
    char *key,
    rrd_info_type_t type,
    rrd_infoval_t value)
{
    im->grinfo_current = rrd_info_push(im->grinfo_current, key, type, value);
    if (im->grinfo == NULL) {
        im->grinfo = im->grinfo_current;
    }
}


void time_clean(
    char *result,
    char *format)
{
    int       j, jj;

/*     Handling based on
       - ANSI C99 Specifications                         http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1124.pdf
       - Single UNIX Specification version 2             http://www.opengroup.org/onlinepubs/007908799/xsh/strftime.html
       - POSIX:2001/Single UNIX Specification version 3  http://www.opengroup.org/onlinepubs/009695399/functions/strftime.html
       - POSIX:2008 Specifications                       http://www.opengroup.org/onlinepubs/9699919799/functions/strftime.html
       Specifications tells
       "If a conversion specifier is not one of the above, the behavior is undefined."

      C99 tells
       "A conversion specifier consists of a % character, possibly followed by an E or O modifier character (described below), followed by a character that determines the behavior of the conversion specifier.

      POSIX:2001 tells
      "A conversion specification consists of a '%' character, possibly followed by an E or O modifier, and a terminating conversion specifier character that determines the conversion specification's behavior."

      POSIX:2008 introduce more complex behavior that are not handled here.

      According to this, this code will replace:
      - % followed by @ by a %@
      - % followed by   by a %SPACE
      - % followed by . by a %.
      - % followed by % by a %
      - % followed by t by a TAB
      - % followed by E then anything by '-'
      - % followed by O then anything by '-'
      - % followed by anything else by at least one '-'. More characters may be added to better fit expected output length
*/

    jj = 0;
    for (j = 0; (j < FMT_LEG_LEN - 1) && (jj < FMT_LEG_LEN); j++) { /* we don't need to parse the last char */
        if (format[j] == '%') {
            if ((format[j + 1] == 'E') || (format[j + 1] == 'O')) {
                result[jj++] = '-';
                j += 2; /* We skip next 2 following char */
            } else if ((format[j + 1] == 'C') || (format[j + 1] == 'd') ||
                       (format[j + 1] == 'g') || (format[j + 1] == 'H') ||
                       (format[j + 1] == 'I') || (format[j + 1] == 'm') ||
                       (format[j + 1] == 'M') || (format[j + 1] == 'S') ||
                       (format[j + 1] == 'U') || (format[j + 1] == 'V') ||
                       (format[j + 1] == 'W') || (format[j + 1] == 'y')) {
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN) {
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 'j') {
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN - 1) {
                    result[jj++] = '-';
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if ((format[j + 1] == 'G') || (format[j + 1] == 'Y')) {
                /* Assuming Year on 4 digit */
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN - 2) {
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 'R') {
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN - 3) {
                    result[jj++] = '-';
                    result[jj++] = ':';
                    result[jj++] = '-';
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 'T') {
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN - 6) {
                    result[jj++] = '-';
                    result[jj++] = ':';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = ':';
                    result[jj++] = '-';
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 'F') {
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN - 8) {
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 'D') {
                result[jj++] = '-';
                if (jj < FMT_LEG_LEN - 6) {
                    result[jj++] = '-';
                    result[jj++] = '/';
                    result[jj++] = '-';
                    result[jj++] = '-';
                    result[jj++] = '/';
                    result[jj++] = '-';
                    result[jj++] = '-';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 'n') {
                result[jj++] = '\r';
                result[jj++] = '\n';
                j++;    /* We skip the following char */
            } else if (format[j + 1] == 't') {
                result[jj++] = '\t';
                j++;    /* We skip the following char */
            } else if (format[j + 1] == '%') {
                result[jj++] = '%';
                j++;    /* We skip the following char */
            } else if (format[j + 1] == ' ') {
                if (jj < FMT_LEG_LEN - 1) {
                    result[jj++] = '%';
                    result[jj++] = ' ';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == '.') {
                if (jj < FMT_LEG_LEN - 1) {
                    result[jj++] = '%';
                    result[jj++] = '.';
                }
                j++;    /* We skip the following char */
            } else if (format[j + 1] == '@') {
                if (jj < FMT_LEG_LEN - 1) {
                    result[jj++] = '%';
                    result[jj++] = '@';
                }
                j++;    /* We skip the following char */
            } else {
                result[jj++] = '-';
                j++;    /* We skip the following char */
            }
        } else {
            result[jj++] = format[j];
        }
    }
    result[jj] = '\0';  /* We must force the end of the string */
}
void auto_scale(
    image_desc_t *im,   /* image description */
    double *value,
    char **symb_ptr,
    double *magfact)
{

    char     *symbol[] = { "a", /* 10e-18 Atto */
        "f",            /* 10e-15 Femto */
        "p",            /* 10e-12 Pico */
        "n",            /* 10e-9  Nano */
        "u",            /* 10e-6  Micro */
        "m",            /* 10e-3  Milli */
        " ",            /* Base */
        "k",            /* 10e3   Kilo */
        "M",            /* 10e6   Mega */
        "G",            /* 10e9   Giga */
        "T",            /* 10e12  Tera */
        "P",            /* 10e15  Peta */
        "E"
    };                  /* 10e18  Exa */

    int       symbcenter = 6;
    int       sindex;

    if (*value == 0.0 || isnan(*value)) {
        sindex = 0;
        *magfact = 1.0;
    } else {
        sindex = floor(log(fabs(*value)) / log((double) im->base));
        *magfact = pow((double) im->base, (double) sindex);
        (*value) /= (*magfact);
    }
    if (sindex <= symbcenter && sindex >= -symbcenter) {
        (*symb_ptr) = symbol[sindex + symbcenter];
    } else {
        (*symb_ptr) = "?";
    }
}
static int strfduration(
    char *const dest,
    const size_t destlen,
    const char *const fmt,
    const double duration)
{
    char     *d = dest, *const dbound = dest + destlen;
    const char *f;
    int       wlen = 0;
    double    seconds = fabs(duration) / 1000.0,
        minutes = seconds / 60.0,
        hours = minutes / 60.0, days = hours / 24.0, weeks = days / 7.0;

#define STORC(chr) do { \
    if (wlen == INT_MAX) return -1; \
    wlen++; \
    if (d < dbound) \
        *d++ = (chr); \
} while(0);

#define STORPF(valArg) do { \
    double pval = trunc((valArg) * pow(10.0, precision)) / pow(10.0, precision); \
    char *tmpfmt; \
    ptrdiff_t avail = dbound - d; \
    int r; \
\
    if (avail < 0 || (uintmax_t) avail > SIZE_MAX) return -1; \
\
    tmpfmt = sprintf_alloc("%%%s%d.%df", \
                              zpad ? "0" : "", \
                                width, \
                                   precision); \
    if (!tmpfmt) return -1; \
\
    r = snprintf(d, avail, tmpfmt, pval); \
    free(tmpfmt); \
    if (r < 0) return -1; \
    d += min(avail, r); \
    if (INT_MAX-r < wlen) return -1; \
    wlen += r; \
} while(0);

    if (duration < 0)
        STORC('-')
            for (f = fmt; *f; f++) {
            if (*f != '%') {
                STORC(*f)
            } else {
                int       zpad, width = 0, precision = 0;

                f++;

                if ((zpad = *f == '0'))
                    f++;

                if (isdigit(*f)) {
                    int       nread;

                    sscanf(f, "%d%n", &width, &nread);
                    f += nread;
                }

                if (*f == '.') {
                    int       nread;

                    f++;
                    if (1 == sscanf(f, "%d%n", &precision, &nread)) {
                        if (precision < 0) {
                            rrd_set_error("Wrong duration format");
                            return -1;
                        }
                        f += nread;
                    }
                }

                switch (*f) {
                case '%':
                    STORC('%')
                        break;
                case 'W':
                    STORPF(weeks)
                        break;
                case 'd':
                    STORPF(days - trunc(weeks) * 7.0)
                        break;
                case 'D':
                    STORPF(days)
                        break;
                case 'h':
                    STORPF(hours - trunc(days) * 24.0)
                        break;
                case 'H':
                    STORPF(hours)
                        break;
                case 'm':
                    STORPF(minutes - trunc(hours) * 60.0)
                        break;
                case 'M':
                    STORPF(minutes)
                        break;
                case 's':
                    STORPF(seconds - trunc(minutes) * 60.0)
                        break;
                case 'S':
                    STORPF(seconds)
                        break;
                case 'f':
                    STORPF(fabs(duration) - trunc(seconds) * 1000.0)
                        break;
                default:
                    rrd_set_error("Wrong duration format");
                    return -1;
                }
            }
        }

    STORC('\0')
        if (destlen > 0)
        *(dbound - 1) = '\0';

    return wlen - 1;

#undef STORC
#undef STORPF
}

static int timestamp_to_tm(
    struct tm *tm,
    double timestamp)
{
    time_t    ts;

    if (timestamp < LLONG_MIN || timestamp > LLONG_MAX)
        return 1;

    ts = (long long int) timestamp;

    if (ts != (long long int) timestamp)
        return 1;

    gmtime_r(&ts, tm);

    return 0;
}


static int parse_float_format(
    const char **fmt)
{
    const char *p = *fmt;

    if (*p++ != '%')
        return 0;

    /*
     * Original FLOAT_STRING:
     *
     * %[-+ 0#]?[0-9]*(?:[.][0-9]+)?l[eEfFgG]
     */
    if (*p == '-' || *p == '+' || *p == ' ' ||
        *p == '0' || *p == '#')
        p++;

    while (*p >= '0' && *p <= '9')
        p++;

    if (*p == '.') {
        p++;

        /* precision must contain at least one digit */
        if (*p < '0' || *p > '9')
            return 0;

        while (*p >= '0' && *p <= '9')
            p++;
    }

    if (*p++ != 'l')
        return 0;

    switch (*p) {
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
        p++;
        break;

    default:
        return 0;
    }

    *fmt = p;
    return 1;
}


int
bad_format_print(
    char *fmt)
{
    const char *p = fmt;
    int have_float = 0;
    int have_string = 0;

    while (*p != '\0') {
        if (*p != '%') {
            p++;
            continue;
        }

        /* %% belongs to SAFE_STRING */
        if (p[1] == '%') {
            p += 2;
            continue;
        }

        if (!have_float) {
            if (!parse_float_format(&p))
                goto bad_format;

            have_float = 1;
            continue;
        }

        /*
         * After the float conversion exactly one optional
         * %s or %S is permitted.
         */
        if (!have_string && (p[1] == 's' || p[1] == 'S')) {
            have_string = 1;
            p += 2;
            continue;
        }

        goto bad_format;
    }

    if (!have_float)
        goto bad_format;

    return 0;

bad_format:
    rrd_set_error("invalid format string '%s'", fmt);
    return 1;
}

int print_calc(
    image_desc_t *im)
{
    long      i, ii, validsteps;
    double    printval;
    struct tm tmvdef;
    int       graphelement = 0;
    long      vidx;
    int       max_ii;
    double    magfact = -1;
    char     *si_symb = "";
    char     *percent_s;
    int       prline_cnt = 0;

    /* wow initializing tmvdef is quite a task :-) */
    time_t    now = time(NULL);

    LOCALTIME_R(&now, &tmvdef, im->extra_flags & FORCE_UTC_TIME);
    for (i = 0; i < im->gdes_c; i++) {
        vidx = im->gdes[i].vidx;
        switch (im->gdes[i].gf) {
        case GF_PRINT:
        case GF_GPRINT:
            /* PRINT and GPRINT can now print VDEF generated values.
             * There's no need to do any calculations on them as these
             * calculations were already made.
             */
            if (im->gdes[vidx].gf == GF_VDEF) { /* simply use vals */
                printval = im->gdes[vidx].vf.val;
                LOCALTIME_R(&im->gdes[vidx].vf.when, &tmvdef, im->extra_flags & FORCE_UTC_TIME);
            } else {    /* need to calculate max,min,avg etcetera */
                max_ii = ((im->gdes[vidx].end - im->gdes[vidx].start)
                          / im->gdes[vidx].step * im->gdes[vidx].ds_cnt);
                printval = DNAN;
                validsteps = 0;
                for (ii = im->gdes[vidx].ds;
                     ii < max_ii; ii += im->gdes[vidx].ds_cnt) {
                    if (!finite(im->gdes[vidx].data[ii]))
                        continue;
                    if (isnan(printval)) {
                        printval = im->gdes[vidx].data[ii];
                        validsteps++;
                        continue;
                    }

                    switch (im->gdes[i].cf) {
                    case CF_HWPREDICT:
                    case CF_MHWPREDICT:
                    case CF_DEVPREDICT:
                    case CF_DEVSEASONAL:
                    case CF_SEASONAL:
                    case CF_AVERAGE:
                        validsteps++;
                        printval += im->gdes[vidx].data[ii];
                        break;
                    case CF_MINIMUM:
                        printval = min(printval, im->gdes[vidx].data[ii]);
                        break;
                    case CF_FAILURES:
                    case CF_MAXIMUM:
                        printval = max(printval, im->gdes[vidx].data[ii]);
                        break;
                    case CF_LAST:
                        printval = im->gdes[vidx].data[ii];
                    }
                }
                if (im->gdes[i].cf == CF_AVERAGE || im->gdes[i].cf > CF_LAST) {
                    if (validsteps > 1) {
                        printval = (printval / validsteps);
                    }
                }
            }           /* prepare printval */

            if (!im->gdes[i].strftm
                && im->gdes[i].vformatter == VALUE_FORMATTER_NUMERIC) {
                if ((percent_s = strstr(im->gdes[i].format, "%S")) != NULL) {
                    /* Magfact is set to -1 upon entry to print_calc.  If it
                     * is still less than 0, then we need to run auto_scale.
                     * Otherwise, put the value into the correct units.  If
                     * the value is 0, then do not set the symbol or magnification
                     * so next the calculation will be performed again. */
                    if (magfact < 0.0) {
                        auto_scale(im, &printval, &si_symb, &magfact);
                        if (printval == 0.0)
                            magfact = -1.0;
                    } else {
                        printval /= magfact;
                    }
                    *(++percent_s) = 's';
                } else if (strstr(im->gdes[i].format, "%s") != NULL) {
                    auto_scale(im, &printval, &si_symb, &magfact);
                }
            }

            if (im->gdes[i].gf == GF_PRINT) {
                rrd_infoval_t prline;

                if (im->gdes[i].strftm) {
                    prline.u_str =
                        (char *) malloc((FMT_LEG_LEN + 2) * sizeof(char));
                    if (im->gdes[vidx].vf.never == 1) {
                        time_clean(prline.u_str, im->gdes[i].format);
                    } else {
                        strftime(prline.u_str,
                                 FMT_LEG_LEN, im->gdes[i].format, &tmvdef);
                    }
                } else {
                    struct tm tmval;

                    switch (im->gdes[i].vformatter) {
                    case VALUE_FORMATTER_NUMERIC:
                        if (bad_format_print(im->gdes[i].format)) {
                            return -1;
                        } else {
                            prline.u_str =
                                sprintf_alloc(im->gdes[i].format, printval,
                                              si_symb);
                        }
                        break;
                    case VALUE_FORMATTER_TIMESTAMP:
                        if (!isfinite(printval)
                            || timestamp_to_tm(&tmval, printval)) {
                            prline.u_str = sprintf_alloc("%.0f", printval);
                        } else {
                            const char *fmt;

                            if (im->gdes[i].format[0] == '\0')
                                fmt = default_timestamp_fmt;
                            else
                                fmt = im->gdes[i].format;
                            prline.u_str =
                                (char *) malloc(FMT_LEG_LEN * sizeof(char));
                            if (!prline.u_str)
                                return -1;
                            if (0 ==
                                strftime(prline.u_str, FMT_LEG_LEN, fmt,
                                         &tmval)) {
                                free(prline.u_str);
                                return -1;
                            }
                        }
                        break;
                    case VALUE_FORMATTER_DURATION:
                        if (!isfinite(printval)) {
                            prline.u_str = sprintf_alloc("%f", printval);
                        } else {
                            const char *fmt;

                            if (im->gdes[i].format[0] == '\0')
                                fmt = default_duration_fmt;
                            else
                                fmt = im->gdes[i].format;
                            prline.u_str =
                                (char *) malloc(FMT_LEG_LEN * sizeof(char));
                            if (!prline.u_str)
                                return -1;
                            if (0 >
                                strfduration(prline.u_str, FMT_LEG_LEN, fmt,
                                             printval)) {
                                free(prline.u_str);
                                return -1;
                            }
                        }
                        break;
                    default:
                        rrd_set_error("Unsupported print value formatter");
                        return -1;
                    }
                }

                grinfo_push(im,
                            sprintf_alloc
                            ("print[%ld]", prline_cnt++), RD_I_STR, prline);
                free(prline.u_str);
            } else {
                /* GF_GPRINT */

                if (im->gdes[i].strftm) {
                    if (im->gdes[vidx].vf.never == 1) {
                        time_clean(im->gdes[i].legend, im->gdes[i].format);
                    } else {
                        strftime(im->gdes[i].legend,
                                 FMT_LEG_LEN, im->gdes[i].format, &tmvdef);
                    }
                } else {
                    struct tm tmval;

                    switch (im->gdes[i].vformatter) {
                    case VALUE_FORMATTER_NUMERIC:
                        if (bad_format_print(im->gdes[i].format)) {
                            return -1;
                        }
                        snprintf(im->gdes[i].legend,
                                 FMT_LEG_LEN - 2,
                                 im->gdes[i].format, printval, si_symb);
                        break;
                    case VALUE_FORMATTER_TIMESTAMP:
                        if (!isfinite(printval)
                            || timestamp_to_tm(&tmval, printval)) {
                            snprintf(im->gdes[i].legend, FMT_LEG_LEN, "%.0f",
                                     printval);
                        } else {
                            const char *fmt;

                            if (im->gdes[i].format[0] == '\0')
                                fmt = default_timestamp_fmt;
                            else
                                fmt = im->gdes[i].format;
                            if (0 ==
                                strftime(im->gdes[i].legend, FMT_LEG_LEN, fmt,
                                         &tmval))
                                return -1;
                        }
                        break;
                    case VALUE_FORMATTER_DURATION:
                        if (!isfinite(printval)) {
                            snprintf(im->gdes[i].legend, FMT_LEG_LEN, "%f",
                                     printval);
                        } else {
                            const char *fmt;

                            if (im->gdes[i].format[0] == '\0')
                                fmt = default_duration_fmt;
                            else
                                fmt = im->gdes[i].format;
                            if (0 >
                                strfduration(im->gdes[i].legend, FMT_LEG_LEN,
                                             fmt, printval))
                                return -1;
                        }
                        break;
                    default:
                        rrd_set_error("Unsupported gprint value formatter");
                        return -1;
                    }
                }
                graphelement = 1;
            }
            break;
        case GF_LINE:
        case GF_AREA:
        case GF_TICK:
            graphelement = 1;
            break;
        case GF_HRULE:
            if (isnan(im->gdes[i].yrule)) { /* we must set this here or the legend printer can not decide to print the legend */
                im->gdes[i].yrule = im->gdes[vidx].vf.val;
            };
            graphelement = 1;
            break;
        case GF_VRULE:
            if (im->gdes[i].xrule == 0) {   /* again ... the legend printer needs it */
                im->gdes[i].xrule = im->gdes[vidx].vf.when;
            };
            graphelement = 1;
            break;
        case GF_COMMENT:
        case GF_TEXTALIGN:
        case GF_DEF:
        case GF_CDEF:
        case GF_VDEF:
#ifdef WITH_PIECHART
        case GF_PART:
#endif
        case GF_SHIFT:
        case GF_XPORT:
            break;
        case GF_STACK:
            rrd_set_error
                ("STACK should already be turned into LINE or AREA here");
            return -1;
            break;
        case GF_XAXIS:
        case GF_YAXIS:
            break;
        }
    }
    return graphelement;
}
