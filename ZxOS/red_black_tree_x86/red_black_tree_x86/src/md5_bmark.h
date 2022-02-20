/*
 * Copyright (C) 2013 Michael Andersch <michael.andersch@mailbox.tu-berlin.de>
 *
 * This file is part of Starbench.
 *
 * Starbench is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Starbench is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Starbench.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#define DIGEST_SIZE 16

typedef struct md5bench {
    int input_set;
    int iterations;
    int numinputs;
    int numthreads;
    int size;
    int outflag;
    uint8_t** inputs;
    uint8_t* out;
    int pinning;
} md5bench_t;

typedef struct {
    int numbufs;
    int bufsize;
    int rseed;
} data_t;

typedef struct {
    uint8_t** in;
    uint8_t* out;
    int size;
    int numbufs;
    int tid;
    int pin_thread;
    int * next_buf;
} threadarg_t;

typedef struct {

  threadarg_t * arg_location;


} offload_md5_struct;
