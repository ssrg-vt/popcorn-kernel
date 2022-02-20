#include <zephyr.h>
#include <drivers/virtualization/ivshmem.h>
#include <stdio.h>

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
void main()
{
	printf("x86 kernel started\n");
	volatile struct shared_area rw_buf = {
		.write_area = (void*)0x50000000,
		.read_area  = (void*)0x5f000000
	};
//	*((uint32_t*)rw_buf.read_area) = 0x00454600 ;
	struct handshake hnsk = {
		.present = 0x1FF1F11F,
	};
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
		
	printf("x86_kernel_exiting");
	*((uint32_t*)rw_buf.read_area) = 0x00000000 ;
	*((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	while(1);
}
