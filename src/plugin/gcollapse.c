/*********************************************************************
 * Program: gcollapse.c
 * Author:  Mauricio Caceres Bravo <caceres@nber.org>
 * Created: Sat May 13 18:12:26 EDT 2017
 * Updated: Tue May 16 08:54:24 EDT 2017
 * Purpose: Stata plugin to compute a faster -collapse-
 * Note:    See stata.com/plugins for more on Stata plugins
 * Version: 0.1.1
 *********************************************************************/

/**
 * @file gcollapse.c
 * @author Mauricio Caceres Bravo
 * @date 16 May 2017
 * @brief Stata plugin for a faster -collapse- implementation
 *
 * This file should only ever be called from gcollapse.ado
 *
 * @see help gcollapse
 * @see http://www.stata.com/plugins for more on Stata plugins
 */

#include <omp.h>
#include <math.h>
#include <time.h>
#include <regex.h>
#include <stdio.h>
#include <locale.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>

#include "spi/stplugin.h"
#include "spt/st_gentools.c"

#include "gtools_utils.c"
#include "gtools_hash.c"
#include "gtools_sort.c"
#include "gtools_math.c"

#define RADIX_SHIFT 16

// TODO: Add comments throughout this file (document) // 2017-05-16 08:52 EDT

// This is required to run on older systems w/o GLIBC_2.14
void * memcpy (void *dest, const void *src, size_t n);
void * memcpy (void *dest, const void *src, size_t n)
{
	return memmove(dest, src, n);
}

void sf_running_timer (clock_t *timer, const char *msg);
void sf_running_timer (clock_t *timer, const char *msg)
{
    double diff  = (double) (clock() - *timer) / CLOCKS_PER_SEC;
    sf_printf (msg);
    sf_printf ("; %.3f seconds.\n", diff);
    *timer = clock();
}

STDLL stata_call(int argc, char *argv[])
{

    setlocale (LC_ALL, "");
    clock_t timer = clock();

    // Variable setup
    // --------------

    ST_double  z;
    ST_retcode rc ;
    int i, j, k;
    size_t J, nj, nj_min, nj_max, sel;
    size_t start, end, offset, offset_bynum;

    size_t in1 = SF_in1();
    size_t in2 = SF_in2();
    size_t N   = in2 - in1 + 1;

    int byvars_k      = sf_get_vector_length("__gtools_byk");
    int collapse_from = byvars_k + 1;
    if (byvars_k < 0) {
        sf_errprintf("Failed to parse __gtools_byk\n");
        return(198);
    }

    // Hashing: Throughout the code we allocate to heap bc C may run out
    // of memory in the stack
    uint64_t *ghash1 = calloc(N, sizeof *ghash1);
    uint64_t *ghash, *ghash2;
    size_t *index = calloc(N, sizeof *index);
    size_t *info;

    // Verbose printing
    int verbose;
    ST_double verb_double ;
    if ( (rc = SF_scal_use("__gtools_verbose", &verb_double)) ) {
        return(rc) ;
    }
    else {
        verbose = (int) verb_double;
    }

    // Benchmark printing
    int benchmark;
    ST_double bench_double ;
    if ( (rc = SF_scal_use("__gtools_benchmark", &bench_double)) ) {
        return(rc) ;
    }
    else {
        benchmark = (int) bench_double;
    }

    /*********************************************************************
     *                    Parse by vars info vectors                     *
     *********************************************************************/

    int byvars_lens[byvars_k],
        byvars_mins[byvars_k],
        byvars_maxs[byvars_k];

    double byvars_lens_double[byvars_k],
           byvars_mins_double[byvars_k],
           byvars_maxs_double[byvars_k];

    if ( (rc = sf_get_vector("__gtools_byk",     byvars_lens_double))   ) return(rc);
    if ( (rc = sf_get_vector("__gtools_bymin",   byvars_mins_double))   ) return(rc);
    if ( (rc = sf_get_vector("__gtools_bymax",   byvars_maxs_double))   ) return(rc);

    for (i = 0; i < byvars_k; i++) {
        byvars_lens[i]   = (int) byvars_lens_double[i];
        byvars_mins[i]   = (int) byvars_mins_double[i];
        byvars_maxs[i]   = (int) byvars_maxs_double[i];
    }

    // Get count of numeric and string by variables
    size_t byvars_kstr = 0;
    for (i = 0; i < byvars_k; i++) {
        byvars_kstr += (byvars_lens[i] > 0);
    }
    size_t byvars_knum = byvars_k - byvars_kstr;

    // If only integers, check worst case of the bijection would not overflow
    int integers_ok;
    int byvars_minlen = mf_min_signed(byvars_lens, byvars_k);
    int byvars_maxlen = mf_max_signed(byvars_lens, byvars_k);
    if ( byvars_maxlen < 0 ) {
        if (byvars_k > 1) {
            integers_ok = 1;
            size_t worst = byvars_maxs[0] - byvars_mins[0] + 1;
            size_t range = byvars_maxs[1] - byvars_mins[1] + (1 < (byvars_k - 1));
            for (k = 1; k < byvars_k; k++) {
                if ( worst > (ULONG_MAX / range)  ) {
                    if ( verbose ) sf_printf("Group variables all intejers but bijection could fail! Won't risk it.\n");
                    integers_ok  = 0;
                    break;
                }
                else {
                    worst *= range;
                    range  = byvars_maxs[k] - byvars_mins[k] + (k < (byvars_k - 1));
                }
            }
        }
        else {
            integers_ok = 1;
        }
    }
    else integers_ok = 0;

    /*********************************************************************
     *                     Parse by vars info macros                     *
     *********************************************************************/

    ST_double __gtools_k_targets,
              __gtools_k_vars,
              __gtools_k_stats,
              __gtools_k_uniq_vars,
              __gtools_k_uniq_stats,
              __gtools_l_targets,
              __gtools_l_vars,
              __gtools_l_stats,
              __gtools_l_uniq_vars,
              __gtools_l_uniq_stats;

    if ( (rc = SF_scal_use ("__gtools_k_targets",    &__gtools_k_targets))    ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_k_vars",       &__gtools_k_vars))       ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_k_stats",      &__gtools_k_stats))      ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_k_uniq_vars",  &__gtools_k_uniq_vars))  ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_k_uniq_stats", &__gtools_k_uniq_stats)) ) return(rc);

    if ( (rc = SF_scal_use ("__gtools_l_targets",    &__gtools_l_targets))    ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_l_vars",       &__gtools_l_vars))       ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_l_stats",      &__gtools_l_stats))      ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_l_uniq_vars",  &__gtools_l_uniq_vars))  ) return(rc);
    if ( (rc = SF_scal_use ("__gtools_l_uniq_stats", &__gtools_l_uniq_stats)) ) return(rc);

    size_t l_targets    = (size_t) __gtools_l_targets + 1;
    size_t l_vars       = (size_t) __gtools_l_vars + 1;
    size_t l_stats      = (size_t) __gtools_l_stats + 1;
    size_t l_uniq_vars  = (size_t) __gtools_l_uniq_vars + 1;
    size_t l_uniq_stats = (size_t) __gtools_l_uniq_stats + 1;

    size_t k_targets    = (size_t) __gtools_k_targets;
    // size_t k_vars       = (size_t) __gtools_k_vars;
    // size_t k_stats      = (size_t) __gtools_k_stats;
    size_t k_uniq_vars  = (size_t) __gtools_k_uniq_vars;
    // size_t k_uniq_stats = (size_t) __gtools_k_uniq_stats;

    char targets    [l_targets];
    char vars       [l_vars];
    char stats      [l_stats];
    char uniq_vars  [l_uniq_vars];
    char uniq_stats [l_uniq_stats];

    // <rant>
    // Have you ever wondered why Stata globals can be up to 32
    // characters in length but locals can only be up to 31? No? Well,
    // when you are trying to copy local macros into C you run into
    // this problem: Local macros in Stata are actually global macros
    // preceded with an underscore.
    //
    // I know; mind = blown. Try this in stata
    //
    //     local a = 12
    //     di $_a, `a'
    //
    // Where is this documented? How does this make sense? Why is this
    // implemented like this? Who knows!
    //
    // </rant>

    // Read in macros with space-delimited variable, target, and statistic names
    if ( (rc = SF_macro_use ("_gtools_targets",    targets,    l_targets))    ) return(rc);
    if ( (rc = SF_macro_use ("_gtools_vars",       vars,       l_vars))       ) return(rc);
    if ( (rc = SF_macro_use ("_gtools_stats",      stats,      l_stats))      ) return(rc);
    if ( (rc = SF_macro_use ("_gtools_uniq_vars",  uniq_vars,  l_uniq_vars))  ) return(rc);
    if ( (rc = SF_macro_use ("_gtools_uniq_stats", uniq_stats, l_uniq_stats)) ) return(rc);

    // size_t targets_from = collapse_from;
    size_t targets_from = collapse_from + k_uniq_vars;
    int pos_targets[k_targets];
    double pos_targets_double[k_targets];

    size_t strlen   = byvars_maxlen > 0? byvars_maxlen + 1: 1;
    size_t str_from = targets_from + k_targets;
    int str_byvars[byvars_kstr];
    double str_byvars_double[byvars_kstr];
    char s[strlen];

    int num_byvars[byvars_knum];
    double num_byvars_double[byvars_knum];

    if ( (rc = sf_get_vector("__gtools_outpos", pos_targets_double)) ) return(rc);
    for (k = 0; k < k_targets; k++)
        pos_targets[k] = (int) pos_targets_double[k];

    if ( byvars_kstr > 0 ) {
        if ( (rc = sf_get_vector("__gtools_strpos", str_byvars_double)) ) return(rc);
        for (k = 0; k < byvars_kstr; k++)
            str_byvars[k] = (int) str_byvars_double[k];
    }

    if ( byvars_knum > 0 ) {
        if ( (rc = sf_get_vector("__gtools_numpos", num_byvars_double)) ) return(rc);
        for (k = 0; k < byvars_knum; k++)
            num_byvars[k] = (int) num_byvars_double[k];
    }

    /*********************************************************************
     *                       Hash the by variables                       *
     *********************************************************************/

    if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 1: stata parsing done");

    // Hash the data
    // -------------

    if ( integers_ok ) {

        // Construct the hash using whole numbers
        // --------------------------------------

        // If al integers are passed, try to use them as the hash by doing
        // a bijection to the whole numbers.

        if (byvars_k > 1) {
            if ( verbose ) sf_printf("Hashing %d integer by variables to whole-nubmer index.\n", byvars_k);
            if ( (rc = sf_get_varlist_bijection (ghash1, 1, byvars_k, in1, in2, byvars_mins, byvars_maxs)) ) return(rc);
        }
        else {
            if ( verbose ) sf_printf("Using sole integer by variable as hash.\n", byvars_k);
            if ( (rc = sf_get_variable_ashash (ghash1, 1, in1, in2, byvars_mins[0])) ) return(rc);
        }
        if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 2: Hashed by variables");

        // Index the hash using a radix sort
        // ---------------------------------

        // index[i] gives the position in Stata of the ith entry
        mf_radix_sort_index (ghash1, index, N, RADIX_SHIFT, 0, verbose);
        if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 3: Sorted on integer-only hash index");

        // info[j], info[j + 1] give the starting and ending position of the
        // jth group in index. So the jth group can be called by looping
        // through index[i] for i = info[j] to i < info[j + 1]
        info = mf_panelsetup (ghash1, N, &J);
        if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 4: Set up variables for main collapse loop");
    }
    else {

        // If non-integers, mix of numbers and strings, or if the bijection could fail,
        // hash the data using Jenkin's 128-bit spooky hash.
        //
        // References
        //     en.wikipedia.org/wiki/Jenkins_hash_function
        //     burtleburtle.net/bob/hash/spooky.html
        //     github.com/centaurean/spookyhash

        ghash2 = calloc(N, sizeof *ghash2);
        if (byvars_k > 1) {
            if ( verbose ) {
                if ( byvars_maxlen > 0 ) {
                    if ( byvars_minlen > 0 ) {
                        sf_printf("Using 128-bit hash to index %d string-only by variables.\n", byvars_k);
                    }
                    else {
                        sf_printf("Using 128-bit hash to index %d by variables (string and numeric).\n", byvars_k);
                    }
                }
                else {
                    sf_printf("Using 128-bit hash to index %d numeric-only by variables", byvars_k);
                }
            }
            if ( (rc = sf_get_varlist_hash (ghash1, ghash2, 1, byvars_k, in1, in2, byvars_lens)) ) return(rc);
        }
        else {
            if ( (byvars_lens[0] > 0) & verbose ) {
                sf_printf("Using 128-bit hash to index string by variable.\n", byvars_k);
            }
            else if ( verbose ) {
                sf_printf("Using 128-bit hash to index numeric by variable.\n", byvars_k);
            }
            if ( (rc = sf_get_variable_hash (ghash1, ghash2, 1, in1, in2, byvars_lens[0])) ) return(rc);
        }
        if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 2: Hashed by variables");

        // Index the hash using a radix sort
        // ---------------------------------

        // index[i] gives the position in Stata of the ith entry
        mf_radix_sort_index (ghash1, index, N, RADIX_SHIFT, 0, verbose);
        if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 3: Sorted on integer-only hash index");

        // Copy ghash2 in case you will need it
        ghash = calloc(N, sizeof *ghash);
        for (i = 0; i < N; i++) {
            ghash[i] = ghash2[index[i]];
        }
        free (ghash2);

        // info[j], info[j + 1] give the starting and ending position of the
        // jth group in index. So the jth group can be called by looping
        // through index[i] for i = info[j] to i < info[j + 1]
        info = mf_panelsetup128 (ghash1, ghash, index, N, &J);
        if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 4: Set up variables for main collapse loop");
        free (ghash);
    }
    free (ghash1);

    /*********************************************************************
     *                         Collapse the data                         *
     *********************************************************************/

    // Group size info
    // ---------------

    nj_min = info[1] - info[0];
    nj_max = info[1] - info[0];
    for (j = 1; j < J; j++) {
        if (nj_min > (info[j + 1] - info[j])) nj_min = (info[j + 1] - info[j]);
        if (nj_max < (info[j + 1] - info[j])) nj_max = (info[j + 1] - info[j]);
    }

    if ( verbose ) {
        if ( nj_min == nj_max )
            sf_printf ("N = %'lu; %'lu balanced groups of size %'lu\n", N, J, nj_min);
        else
            sf_printf ("N = %'lu; %'lu unbalanced groups of sizes %'lu to %'lu\n", N, J, nj_min, nj_max);
    }

    // Collapse the data!
    // ------------------

    size_t nmfreq[k_uniq_vars], nonmiss[k_uniq_vars], firstmiss[k_uniq_vars], lastmiss[k_uniq_vars];
    double *bynum   = calloc(byvars_knum * J, sizeof *bynum);
    short  *bymiss  = calloc(byvars_knum * J, sizeof *bymiss);
    double *output  = calloc(k_targets * J, sizeof *output);
    short  *outmiss = calloc(k_targets * J, sizeof *outmiss);
    double *buffer  = calloc(k_uniq_vars * nj_max, sizeof *output);

    char *stat[k_targets], *strstat, *ptr;
    strstat = strtok_r (stats, " ", &ptr);
    for (k = 0; k < k_targets; k++) {
        stat[k] = strstat;
        strstat = strtok_r (NULL, " ", &ptr);
    }

    for (i = 0; i < byvars_knum * J; i++)
        bymiss[i] = 0;

    for (i = 0; i < k_targets * J; i++)
        outmiss[i] = 0;

    for (k = 0; k < k_uniq_vars; k++)
        nmfreq[k] = nonmiss[k] = firstmiss[k] = lastmiss[k] = 0;

    // Read in group variables and output summary stats
    // ------------------------------------------------

    /*************
     *  Testing  *
     *************/
    double *all_buffer    = calloc(k_uniq_vars * N, sizeof *output);
    short  *all_firstmiss = calloc(k_uniq_vars * J, sizeof *output);
    short  *all_lastmiss  = calloc(k_uniq_vars * J, sizeof *output);
    size_t *all_nonmiss   = calloc(k_uniq_vars * J, sizeof *output);
    size_t offset_source, offset_buffer;

    // parallel
    size_t offsets_buffer[J];
    int nloops;
    // parallel

    for (j = 0; j < J * k_uniq_vars; j++)
        all_firstmiss[j] = all_lastmiss[j] = all_nonmiss[j] = 0;

    // #pragma omp parallel private (i, k, offset_buffer, offset_source, offset, sel, start, end, nj) shared (verbose, output, outmiss, stat, all_firstmiss, all_lastmiss, offsets_buffer, all_nonmiss, k_targets, pos_targets, k_uniq_vars, info, all_buffer, index)
    // {
    // }
    offset_buffer = offset_source = 0;
    for (j = 0; j < J; j++) {
        start  = info[j];
        end    = info[j + 1];
        nj     = end - start;
        for (i = start; i < end; i++) {
            sel = index[i] + in1;
            for (k = 0; k < k_uniq_vars; k++) {
                if ( (rc = SF_vdata(k + collapse_from, sel, &z)) ) return(rc);
                if ( SF_is_missing(z) ) {
                    if (i == start)   all_firstmiss[offset_source + k] = 1;
                    if (i == end - 1) all_lastmiss[offset_source + k]  = 1;
                }
                else {
                    all_buffer [offset_buffer + nj * k + all_nonmiss[offset_source + k]++] = z;
                }
            }
        }
        // parallel
        offsets_buffer[j] = offset_buffer;
        // parallel
        offset_buffer += nj * k_uniq_vars;
        offset_source += k_uniq_vars;
    }
    if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 5.1: Read in source variables");

    // parallel
    for (j = 0; j < J; j++)
        for (k = 0; k < k_uniq_vars; k++)
            nmfreq[k] += all_nonmiss[j * k_uniq_vars + k];
    // parallel

    // #pragma omp parallel private (k, offset_buffer, offset_source, offset, sel, start, end, nj) shared (verbose, output, outmiss, stat, all_firstmiss, all_lastmiss, offsets_buffer, all_nonmiss, k_targets, pos_targets, k_uniq_vars, info, all_buffer)
    // {
        // parallel
        // nloops        = 0;
        // offset_buffer = 0;
        // offset_source = 0;
        // offset        = 0;
        // sel           = 0;
        // start         = 0;
        // end           = 0;
        // nj            = 0;
        // k             = 0;
        // parallel

        // single thread
        // offset_buffer = info[0];
        // offset_source = offset = 0;
        // single thread
        // #pragma omp for
        for (j = 0; j < J; j++) {
            // parallel
            // ++nloops;
            offset = j * k_targets;
            offset_source = j * k_uniq_vars;
            offset_buffer = offsets_buffer[j];
            // parallel
            nj = info[j + 1] - info[j];
            for (k = 0; k < k_targets; k++) {
                sel   = offset_source + pos_targets[k];
                start = offset_buffer + nj * pos_targets[k];
                end   = all_nonmiss[sel];

                if ( mf_strcmp_wrapper (stat[k], "count") ) {
                    output[offset + k] = end;
                }
                else if ( mf_strcmp_wrapper (stat[k], "percent")  ) {
                    output[offset + k] = 100 * end;
                }
                else if ( all_firstmiss[sel] & (mf_strcmp_wrapper (stat[k], "first") ) ) {
                    outmiss[offset + k] = 1;
                }
                else if ( all_lastmiss[sel] & (mf_strcmp_wrapper (stat[k], "last") ) ) {
                    outmiss[offset + k] = 1;
                }
                else if ( mf_strcmp_wrapper (stat[k], "first") | (mf_strcmp_wrapper (stat[k], "firstnm") ) ) {
                    output[offset + k] = all_buffer[start];
                }
                else if ( mf_strcmp_wrapper (stat[k], "last") | (mf_strcmp_wrapper (stat[k], "lastnm") ) ) {
                    output[offset + k] = all_buffer[start + end - 1];
                }
                else if ( mf_strcmp_wrapper (stat[k], "sd") &  (end < 2) ) {
                    outmiss[offset + k] = 1;
                }
                else if ( end == 0 ) {
                    outmiss[offset + k] = 1;
                }
                else {
                    output[offset + k] = mf_switch_fun (stat[k], all_buffer, start, start + end);
                }
            }

            // single thread
            // offset_buffer += (info[j + 1] - info[j]) * k_uniq_vars;
            // for (k = 0; k < k_uniq_vars; k++)
            //     nmfreq[k] += all_nonmiss[offset_source + k];
            //
            // offset += k_targets;
            // offset_source += k_uniq_vars;
            // single thread
        }
        // #pragma omp critical
        // {
        //     if ( verbose ) sf_printf("\t\tThread %d processed %d groups.\n", omp_get_thread_num(), nloops);
        // }
    // }
    if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 5.2: Collapsed source variables");

    free (all_buffer);
    free (all_firstmiss);
    free (all_lastmiss);
    free (all_nonmiss);

    offset_bynum = 0;
    for (j = 0; j < J; j++) {
        start = info[j];
        for (k = 0; k < byvars_kstr; k++) {
            if ( (rc = SF_sdata(str_byvars[k], index[start] + in1, s)) ) return(rc);
            if ( (rc = SF_sstore(k + str_from, j + 1, s)) ) return(rc);
        }
        for (k = 0; k < byvars_knum; k++) {
            if ( (rc = SF_vdata(num_byvars[k], index[start] + in1, &z)) ) return(rc);
            if ( SF_is_missing(z) ) {
                bymiss[offset_bynum + k] = 1;
            }
            else {
                bynum[offset_bynum + k] = z;
            }
        }
        offset_bynum += byvars_knum;
    }

    /*************
     *  Testing  *
     *************/

    /*************
     *  Working  *
     *************
    offset = offset_bynum = 0;
    for (j = 0; j < J; j++) {
        start = info[j];
        end   = info[j + 1];
        nj    = end - start;

        for (i = start; i < end; i++) {
            for (k = 0; k < k_uniq_vars; k++) {
                if ( (rc = SF_vdata(k + collapse_from, index[i] + in1, &z)) ) return(rc);
                if ( SF_is_missing(z) ) {
                    if (i == start)   firstmiss[k] = 1;
                    if (i == end - 1) {
                        lastmiss[k]  = 1;
                    }
                }
                else {
                 buffer [nj * k + nonmiss[k]++] = z;
                }
            }
        }

        for (k = 0; k < byvars_kstr; k++) {
            if ( (rc = SF_sdata(str_byvars[k], index[start] + in1, s)) ) return(rc);
            if ( (rc = SF_sstore(k + str_from, j + 1, s)) ) return(rc);
        }
        for (k = 0; k < byvars_knum; k++) {
            if ( (rc = SF_vdata(num_byvars[k], index[start] + in1, &z)) ) return(rc);
            if ( SF_is_missing(z) ) {
                bymiss[offset_bynum + k] = 1;
            }
            else {
                bynum[offset_bynum + k] = z;
            }
        }
        offset_bynum += byvars_knum;

        for (k = 0; k < k_targets; k++) {
            start = nj * pos_targets[k];
            end   = nonmiss[pos_targets[k]];

            if ( mf_strcmp_wrapper (stat[k], "count") ) {
                output[offset + k] = end;
            }
            else if ( mf_strcmp_wrapper (stat[k], "percent")  ) {
                output[offset + k] = 100 * end;
            }
            else if ( firstmiss[pos_targets[k]] & (mf_strcmp_wrapper (stat[k], "first") ) ) {
                outmiss[offset + k] = 1;
            }
            else if ( lastmiss[pos_targets[k]] & (mf_strcmp_wrapper (stat[k], "last") ) ) {
                outmiss[offset + k] = 1;
            }
            else if ( mf_strcmp_wrapper (stat[k], "first") | (mf_strcmp_wrapper (stat[k], "firstnm") ) ) {
                output[offset + k] = buffer[start];
            }
            else if ( mf_strcmp_wrapper (stat[k], "last") | (mf_strcmp_wrapper (stat[k], "lastnm") ) ) {
                output[offset + k] = buffer[start + end - 1];
            }
            else if ( mf_strcmp_wrapper (stat[k], "sd") &  (end < 2) ) {
                outmiss[offset + k] = 1;
            }
            else if ( end == 0 ) {
                outmiss[offset + k] = 1;
            }
            else {
                output[offset + k] = mf_switch_fun (stat[k], buffer, start, start + end);
            }
        }
        offset += k_targets;

        for (k = 0; k < k_uniq_vars; k++) {
            nmfreq[k] += nonmiss[k];
            nonmiss[k] = firstmiss[k] = lastmiss[k] = 0;
        }
    }
    if ( benchmark ) sf_running_timer (&timer, "\tPlugin step 5: Collapsed source variables");
     *************
     *  Working  *
     *************/

    free (buffer);
    free (index);
    free (info);

    // Copy output back into Stata
    // ---------------------------

    offset = offset_bynum = 0;
    for (j = 0; j < J; j++) {
        for (k = 0; k < k_targets; k++) {
            sel = offset + k;
            if ( mf_strcmp_wrapper (stat[k], "percent") ) output[sel] /= nmfreq[pos_targets[k]];
            if ( (rc = SF_vstore(k + targets_from, j + 1, outmiss[sel]? SV_missval: output[sel])) ) return (rc);
        }
        for (k = 0; k < byvars_kstr; k++) {
            if ( (rc = SF_sdata(k + str_from, j + 1, s)) ) return(rc);
            if ( (rc = SF_sstore(str_byvars[k], j + 1, s)) ) return(rc);
        }
        for (k = 0; k < byvars_knum; k++) {
            sel = offset_bynum + k;
            if ( (rc = SF_vstore(num_byvars[k], j + 1, bymiss[sel]? SV_missval: bynum[sel])) ) return(rc);
        }
        offset += k_targets;
        offset_bynum += byvars_knum;
    }
    if ( benchmark ) sf_running_timer (&timer, "\ttPlugin step 6: Copied group variables back to stata");

    free (output);
    free (bynum);
    free (bymiss);
    free (outmiss);

    SF_scal_save ("__gtools_J", J);
    return(0);
}
