cp corundum_10g/fpga/mqnic/250S0C/fpga_pcie/fpga/fpga.runs/impl_1/fpga.bit Pcie_Arm/images/linux/system.bit 
echo "Copying the XSA..."
echo "Copied the XSA"
cd ***Add Petalinux project folder here***
echo "Cleaning Petalinux project"
petalinux-build -x mrproper -f
echo "Clean completed!"
petalinux-config --get-hw-description=. --silentconfig
echo "Initiating build"
petalinux-build
echo "Build completed!"
echo "Creating Boot image.."
./build_binfile.sh
cd ../xsct_flash_prog
echo "Flashing image..."
python program_flash.py -b ../***Add Petalinux project folder here***/images/linux/BOOT.BIN