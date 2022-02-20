#include <zephyr.h>
#include <drivers/virtualization/ivshmem.h>
#include <stdio.h>
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
struct handshake
{
        uint32_t present;
        char arch[10];
};
struct shared_area
{
        volatile void * write_area;
        volatile void * read_area;
};
  typedef struct {
      unsigned int mantissa_low:32;     
      unsigned int mantissa_high:20;
      unsigned int exponent:11;        
      unsigned int sign:1;
    } tDoubleStruct;


void vranlc( int n, double *x, double a, double y[] )
{
  const double r23 = 1.1920928955078125e-07;
  const double r46 = r23 * r23;
  const double t23 = 8.388608e+06;
  const double t46 = t23 * t23;

  double t1, t2, t3, t4, a1, a2, x1, x2, z;

  int i;

  //--------------------------------------------------------------------
  //  Break A into two parts such that A = 2^23 * A1 + A2.
  //--------------------------------------------------------------------
  t1 = r23 * a;
  a1 = (int) t1;
  a2 = a - t23 * a1;

  //--------------------------------------------------------------------
  //  Generate N results.   This loop is not vectorizable.
  //--------------------------------------------------------------------
  for ( i = 0; i < n; i++ ) {
    //--------------------------------------------------------------------
    //  Break X into two parts such that X = 2^23 * X1 + X2, compute
    //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
    //  X = 2^23 * Z + A2 * X2  (mod 2^46).
    //--------------------------------------------------------------------
    t1 = r23 * (*x);
    x1 = (int) t1;
    x2 = *x - t23 * x1;
    t1 = a1 * x2 + a2 * x1;
    t2 = (int) (r23 * t1);
    z = t1 - t23 * t2;
    t3 = t23 * z + a2 * x2;
    t4 = (int) (r46 * t3) ;
    *x = t3 - t46 * t4;
    y[1] = r46 * (*x);
  }

  return;
}


void main()
{
	printf("arm64 app started\n");
	volatile struct shared_area rw_buf = {
                .write_area = (void*)0x9f000000,
                .read_area  = (void*)0x90000000
        };
	*((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	struct handshake hnsk = {
                .present = 0xE00E0EE0,
        };
	struct handshake in_hnsk;
	memcpy(hnsk.arch, "ARM",4);
	memcpy((void*)&in_hnsk,rw_buf.read_area,sizeof(struct shared_area)); 	
	if(in_hnsk.present  == 0x1FF1F11F)
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
			if(other->present == 0x1FF1F11F)
			{
                                printf("Other core of type x86 now conected \n");
                        	break;
			}
			else
			{
				if(l % 9000000 == 0){
                                        printf("waiting for other core\n");
                        		memcpy((void*)rw_buf.write_area,(void*)&hnsk,sizeof(struct shared_area));
				}
                                l++;
			}
                }
        }
	blackscholes_main();
	while(1)
	{
		struct offload_struct *  inp = (struct offload_struct *)rw_buf.read_area ;
		if(inp->new_request == 0xF00F0FF0)
		{
		if(COMPUTE_VRANLC == inp->type){
			printf("A request of type %d came\n",COMPUTE_VRANLC);
			printf("Args are %d , %f , %f and %f\n", *inp->args[0].location,*(double*)inp->args[1].location,*(double*)inp->args[2].location,*(double*)inp->args[3].location);
			vranlc(*(int*)inp->args[0].location,(double*)inp->args[1].location,*(double*)inp->args[2].location , (double*)inp->args[3].location);
			printf("Post computation value of y is %f and value of x is %f\n",*((double*)inp->args[3].location), *((double*)inp->args[1].location));			
			
			//indicate that you have consumed the message
			*(uint32_t*)inp = ~(0xF00F0FF0);

			//Start replying
			struct offload_struct ofld_vranlc ;

			ofld_vranlc.new_request = 0xF00F0FF0;
			void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);
			
			*((int*)write_pointer)  = *(int*)inp->args[0].location;
		        ofld_vranlc.args[0].location =  (uint32_t*)((char*)write_pointer - 0x40000000);
        		printf("Location of the int is %p\n",ofld_vranlc.args[0].location);
      			write_pointer =  (void*)((char*)write_pointer +  sizeof(int));

        		*((double*)write_pointer) = *(int*)inp->args[1].location;
        		ofld_vranlc.args[1].location =  (uint32_t*)((char*)write_pointer - 0x40000000) ;
        		printf("Location of the float pointer is %p\n",ofld_vranlc.args[1].location);
        		write_pointer  =  (void*)((char*)write_pointer +  sizeof(double));

        		*((double*)write_pointer) = *(int*)inp->args[2].location;
        		ofld_vranlc.args[2].location =  (uint32_t*)((char*)write_pointer - 0x40000000) ;
        		write_pointer  =  (void*)((char*)write_pointer +  sizeof(double));

        		*((double*)write_pointer) = *(int*)inp->args[3].location;
        		ofld_vranlc.args[3].location = (uint32_t*)((char*)write_pointer - 0x40000000)   ;


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
			}
		}
	}
	printf("ARM_kernel_exiting\n");
     	*((uint32_t*)rw_buf.read_area) = 0x00000000 ;
     	*((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	while(1);
}
