
	SECTION_PROLOGUE(_APP_SMEM_SECTION_NAME,,)
	{
		APP_SHARED_ALIGN;
		_app_smem_start = .;

		/* Auto generated code do not modify */
		SMEM_PARTITION_ALIGN(z_data_smem_z_malloc_partition_bss_end - z_data_smem_z_malloc_partition_part_start);
		z_data_smem_z_malloc_partition_part_start = .;
		KEEP(*(data_smem_z_malloc_partition_data*))

		z_data_smem_z_malloc_partition_bss_start = .;
		KEEP(*(data_smem_z_malloc_partition_bss*))

		z_data_smem_z_malloc_partition_bss_end = .;
		SMEM_PARTITION_ALIGN(z_data_smem_z_malloc_partition_bss_end - z_data_smem_z_malloc_partition_part_start);
		z_data_smem_z_malloc_partition_part_end = .;

		APP_SHARED_ALIGN;
		_app_smem_end = .;
	} GROUP_DATA_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)

	z_data_smem_z_malloc_partition_part_size = z_data_smem_z_malloc_partition_part_end - z_data_smem_z_malloc_partition_part_start;
	z_data_smem_z_malloc_partition_bss_size = z_data_smem_z_malloc_partition_bss_end - z_data_smem_z_malloc_partition_bss_start;
