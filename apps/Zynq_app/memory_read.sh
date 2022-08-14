for i in {1..20}
do 
	./a.out /dev/uio0 0x0 -a 10000 -n 512 -w 8
done
echo "Data Read Completed!"
