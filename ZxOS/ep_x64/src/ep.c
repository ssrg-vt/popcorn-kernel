//-------------------------------------------------------------------------//
//                                                                         //
//  This benchmark is a serial C version of the NPB EP code. This C        //
//  version is developed by the Center for Manycore Programming at Seoul   //
//  National University and derived from the serial Fortran versions in    //
//  "NPB3.3-SER" developed by NAS.                                         //
//                                                                         //
//  Permission to use, copy, distribute and modify this software for any   //
//  purpose with or without fee is hereby granted. This software is        //
//  provided "as is" without express or implied warranty.                  //
//                                                                         //
//  Information on NPB 3.3, including the technical report, the original   //
//  specifications, source code, results and information on how to submit  //
//  new results, is available at:                                          //
//                                                                         //
//           http://www.nas.nasa.gov/Software/NPB/                         //
//                                                                         //
//  Send comments or suggestions for this C version to cmp@aces.snu.ac.kr  //
//                                                                         //
//          Center for Manycore Programming                                //
//          School of Computer Science and Engineering                     //
//          Seoul National University                                      //
//          Seoul 151-744, Korea                                           //
//                                                                         //
//          E-mail:  cmp@aces.snu.ac.kr                                    //
//                                                                         //
//-------------------------------------------------------------------------//

//-------------------------------------------------------------------------//
// Authors: Sangmin Seo, Jungwon Kim, Jun Lee, Jeongho Nah, Gangwon Jo,    //
//          and Jaejin Lee                                                 //
//-------------------------------------------------------------------------//

//--------------------------------------------------------------------
//      program EMBAR
//--------------------------------------------------------------------
//  This is the serial version of the APP Benchmark 1,
//  the "embarassingly parallel" benchmark.
//
//
//  M is the Log_2 of the number of complex pairs of uniform (0, 1) random
//  numbers.  MK is the Log_2 of the size of each batch of uniform random
//  numbers.  MK can be set for convenience on a given system, since it does
//  not affect the results.
//--------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "type.h"
#include "npbparams.h"
#include "randdp.h"
#include "timers.h"
#include "print_results.h"
#include "device.h"
#include <zephyr/types.h>
#include <sys/time.h>

#define MAX(X,Y)  (((X) > (Y)) ? (X) : (Y))

#define MK        16
#define MM        (M - MK)
#define NN        (1 << MM)
#define NK        (1 << MK)
#define NQ        10
#define EPSILON   1.0e-8
#define A         1220703125.0
#define S         271828183.0

#define DISTRIBUTION_ENABLE 1

static double x[2*NK];
static double q[NQ]; 

enum request_type{
	COMPUTE_VRANLC,
	COMPUTE_RANDLC,
	COMPUTE_LOG,
	RESPOND_VRANLC

};
enum arg_type {
	CHAR,
	CHAR_P,
	DOUBLE,
	DOUBLE_P,
	INT
	
};
struct argument {
	uint32_t * location;
	uint32_t size;
	enum arg_type type;	
};

struct offload_struct{
	uint32_t new_request ;
	enum request_type type;
	struct argument args[8];
};

struct shared_area
{
        volatile void * write_area;
        volatile void * read_area;
};

struct handshake
{
        uint32_t  present;
        char arch[10];
};
volatile struct shared_area rw_buf; 



void vranlc_dist( int n, double *x, double a, double y[] ){

	printf("Size of double is %d\n",sizeof(double));

	struct offload_struct  ofld_vranlc ;
	ofld_vranlc.new_request = 0xF00F0FF0;
	void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);
	
	*((int*)write_pointer)  = n; 
	ofld_vranlc.args[0].location =  (uint32_t*)((char*)write_pointer + 0x40000000);
	printf("Location of the int is %p\n",ofld_vranlc.args[0].location);
	write_pointer =  (void*)((char*)write_pointer +  sizeof(int));
	
	printf("Size after incrementing is %p\n",write_pointer);	
	*((double*)write_pointer) = *x;
        ofld_vranlc.args[1].location =  (uint32_t*)((char*)write_pointer + 0x40000000) ;
	printf("Location of the float pointer is %p\n",ofld_vranlc.args[1].location);
	write_pointer  =  (void*)((char*)write_pointer +  sizeof(double));
        
	*((double*)write_pointer) = a;
	ofld_vranlc.args[2].location =  (uint32_t*)((char*)write_pointer + 0x40000000) ;
	write_pointer  =  (void*)((char*)write_pointer +  sizeof(double));
	
	*((double*)write_pointer) = *y;	
        ofld_vranlc.args[3].location = (uint32_t*)((char*)write_pointer + 0x40000000)   ;
	
		
	ofld_vranlc.type = COMPUTE_VRANLC;
	ofld_vranlc.args[0].size = 1;
	ofld_vranlc.args[0].type = INT; 
	ofld_vranlc.args[1].size = 1;
        ofld_vranlc.args[1].type = DOUBLE;
	ofld_vranlc.args[2].size = 1;
        ofld_vranlc.args[2].type = DOUBLE;
	ofld_vranlc.args[3].size = 1;
        ofld_vranlc.args[3].type = DOUBLE;
	
	memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );
	printf("Copied the sturct details\n");
	printf("Args are %d , %f , %f and %f\n", n,*x,a,*y);	
	int request_feedback = 1;
	while(1)
	{
		if((*((uint32_t*)rw_buf.write_area) == ~(0xF00F0FF0) ) && request_feedback )
		{
			printf("ARM has started calculating vranlc\n");
			request_feedback = 0;
		}
		else
		{
			struct offload_struct *  outp = (struct offload_struct *)rw_buf.read_area ;
			if(outp->new_request == 0xF00F0FF0)
			{
				*x = *(double*)outp->args[1].location;
				*y = *(double*)outp->args[3].location;
				printf("retrived values from ARM machine \n");
				break;
			}
			
		}

	}	
}

int main() 
{
  double Mops, t1, t2, t3, t4, x1, x2;
  double sx, sy, tm, an, tt, gc;
  double sx_verify_value, sy_verify_value, sx_err, sy_err;
  int    np;
  int    i, ik, kk, l, k, nit;
  int    k_offset, j;
  logical verified, timers_enabled;

  printf("x86 kernel started\n");
  rw_buf.write_area = (void*)0x50000000;
  rw_buf.read_area  = (void*)0x5f000000;
  struct handshake hnsk = {
                .present = 0x1FF1F11F,
        };
/*	for(int li = 0 ; li < 20 ; li++)
	{
		printf("char at %p is %c\n",((char *)rw_buf.read_area + li) , *((char *)rw_buf.read_area + li));
	}
*/	
        struct handshake in_hnsk;
        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
        memcpy(hnsk.arch, "x86",4);
        memcpy((void*)&in_hnsk,rw_buf.read_area,sizeof(struct shared_area));
        if(in_hnsk.present == 0xE00E0EE0)
        {
                printf("Already other core %s present in memory.\n",in_hnsk.arch);
                memcpy(rw_buf.write_area,(void*)&hnsk,sizeof(struct shared_area));
        }
	else{
                printf("Attempting connection with other core ::\n");
                static uint64_t l = 0;
                while(1)
                {
                        volatile struct handshake * other = (struct handshake*)rw_buf.read_area;
                        if(other->present == 0xE00E0EE0)
                        {
                                printf("Other core of type %s conected \n",other->arch);
                                break;
                        }
                        else
                        {
                                if(l % 9000000 == 0)
                                {
                                        printf("waiting for other core\n");
                                        memcpy(rw_buf.write_area,(void*)&hnsk,sizeof(struct shared_area));
                                }
                                l++;
                        }
                }
        }
	*((uint32_t*)rw_buf.read_area) = 0x00000000 ;
        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	printf("x86_kernel_ shraed memory area exiting\n");
	struct timeval tv;
  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n\n",tv.tv_sec);
  double dum[3] = {1.0, 1.0, 1.0};
  char   size[16];
  FILE *fp;
  if ((fp = fopen("timer.flag", "r")) == NULL) {
    timers_enabled = false;
  } else {
    timers_enabled = true;
    fclose(fp);
  }
  
  
  //--------------------------------------------------------------------
  //  Because the size of the problem is too large to store in a 32-bit
  //  integer for some classes, we put it into a string (for printing).
  //  Have to strip off the decimal point put in there by the floating
  //  point print statement (internal file)
  //--------------------------------------------------------------------

  sprintf(size, "%15.0lf", pow(2.0, M+1));
  j = 14;
  if (size[j] == '.') j--;
  size[j+1] = '\0';
  printf("\n\n NAS Parallel Benchmarks (NPB3.3-SER-C) - EP Benchmark\n");
  printf("\n Number of random numbers generated: %15s\n", size);

  verified = false;

  //--------------------------------------------------------------------
  //  Compute the number of "batches" of random number pairs generated 
  //  per processor. Adjust if the number of processors does not evenly 
  //  divide the total number
  //--------------------------------------------------------------------

  np = NN; 

  //--------------------------------------------------------------------
  //  Call the random number generator functions and initialize
  //  the x-array to reduce the effects of paging on the timings.
  //  Also, call all mathematical functions that are used. Make
  //  sure these initializations cannot be eliminated as dead code.
  //--------------------------------------------------------------------
  if(DISTRIBUTION_ENABLE == 1)
  {
	
	vranlc_dist(10000, &dum[0], dum[1], &dum[2]);	
	
  }
  else{
	printf("before vranlc dum[2] value is %f",dum[2]);
  	vranlc(10, &dum[0], dum[1], &dum[2]);
	printf("With DISTRIBUTION_ENABLE 0 and n 10, dum[2] value is %f and dum[0] value is %f\n",dum[2],dum[0]);
  }
  dum[0] = randlc(&dum[1], dum[2]);
  for (i = 0; i < 2 * NK; i++) {
    x[i] = -1.0e99;
  }
  Mops = log(sqrt(fabs(MAX(1.0, 1.0))));   

  timer_clear(0);
  timer_clear(1);
  timer_clear(2);
  timer_start(0);


  t1 = A;
  vranlc(0, &t1, A, x);

  //--------------------------------------------------------------------
  //  Compute AN = A ^ (2 * NK) (mod 2^46).
  //--------------------------------------------------------------------

  t1 = A;

  for (i = 0; i < MK + 1; i++) {
    t2 = randlc(&t1, t1);
  }

  an = t1;
  tt = S;
  gc = 0.0;
  sx = 0.0;
  sy = 0.0;

  for (i = 0; i < NQ; i++) {
    q[i] = 0.0;
  }

  //--------------------------------------------------------------------
  //  Each instance of this loop may be performed independently. We compute
  //  the k offsets separately to take into account the fact that some nodes
  //  have more numbers to generate than others
  //--------------------------------------------------------------------

  k_offset = -1;

  for (k = 1; k <= np; k++) {
    kk = k_offset + k; 
    t1 = S;
    t2 = an;

    // Find starting seed t1 for this kk.

    for (i = 1; i <= 100; i++) {
      ik = kk / 2;
      if ((2 * ik) != kk) t3 = randlc(&t1, t2);
      if (ik == 0) break;
      t3 = randlc(&t2, t2);
      kk = ik;
    }

    //--------------------------------------------------------------------
    //  Compute uniform pseudorandom numbers.
    //--------------------------------------------------------------------
    if (timers_enabled) timer_start(2);
    vranlc(2 * NK, &t1, A, x);
    if (timers_enabled) timer_stop(2);

    //--------------------------------------------------------------------
    //  Compute Gaussian deviates by acceptance-rejection method and 
    //  tally counts in concentri//square annuli.  This loop is not 
    //  vectorizable. 
    //--------------------------------------------------------------------
    if (timers_enabled) timer_start(1);

    for (i = 0; i < NK; i++) {
      x1 = 2.0 * x[2*i] - 1.0;
      x2 = 2.0 * x[2*i+1] - 1.0;
      t1 = x1 * x1 + x2 * x2;
      if (t1 <= 1.0) {
        t2   = sqrt(-2.0 * log(t1) / t1);
        t3   = (x1 * t2);
        t4   = (x2 * t2);
        l    = MAX(fabs(t3), fabs(t4));
        q[l] = q[l] + 1.0;
        sx   = sx + t3;
        sy   = sy + t4;
      }
    }

    if (timers_enabled) timer_stop(1);
  }

  for (i = 0; i < NQ; i++) {
    gc = gc + q[i];
  }


  timer_stop(0);
  tm =timer_read(0);

  nit = 0;
  verified = true;
  if (M == 24) {
    printf("M value is 24\n");
    sx_verify_value = -3.247834652034740e+3;
    sy_verify_value = -6.958407078382297e+3;
  } else if (M == 25) {
    sx_verify_value = -2.863319731645753e+3;
    sy_verify_value = -6.320053679109499e+3;
  } else if (M == 28) {
    sx_verify_value = -4.295875165629892e+3;
    sy_verify_value = -1.580732573678431e+4;
  } else if (M == 30) {
    sx_verify_value =  4.033815542441498e+4;
    sy_verify_value = -2.660669192809235e+4;
  } else if (M == 32) {
    sx_verify_value =  4.764367927995374e+4;
    sy_verify_value = -8.084072988043731e+4;
  } else if (M == 36) {
    sx_verify_value =  1.982481200946593e+5;
    sy_verify_value = -1.020596636361769e+5;
  } else if (M == 40) {
    sx_verify_value = -5.319717441530e+05;
    sy_verify_value = -3.688834557731e+05;
  } else {
    verified = false;
  }

  if (verified) {
    sx_err = fabs((sx - sx_verify_value) / sx_verify_value);
    sy_err = fabs((sy - sy_verify_value) / sy_verify_value);
    verified = ((sx_err <= EPSILON) && (sy_err <= EPSILON));
  }

  Mops = pow(2.0, M+1) / tm / 1000000.0;

  printf("\nEP Benchmark Results:\n\n");
  printf("CPU Time =%10.4lf\n", tm);
  printf("N = 2^%5d\n", M);
  printf("No. Gaussian Pairs = %15.0lf\n", gc);
  printf("Sums = %25.15lE %25.15lE\n", sx, sy);
  printf("Counts: \n");
  for (i = 0; i < NQ; i++) {
    printf("%3d%15.0lf\n", i, q[i]);
  }

  print_results("EP", CLASS, M+1, 0, 0, nit,
      tm, Mops, 
      "Random numbers generated",
      verified, NPBVERSION, COMPILETIME, CS1,
      CS2, CS3, CS4, CS5, CS6, CS7);

  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n",tv.tv_sec);
  if (timers_enabled) {
    if (tm <= 0.0) tm = 1.0;
    tt = timer_read(0);
    printf("\nTotal time:     %9.3lf (%6.2lf)\n", tt, tt*100.0/tm);
    tt = timer_read(1);
    printf("Gaussian pairs: %9.3lf (%6.2lf)\n", tt, tt*100.0/tm);
    tt = timer_read(2);
    printf("Random numbers: %9.3lf (%6.2lf)\n", tt, tt*100.0/tm);
  }

  return 0;
}
