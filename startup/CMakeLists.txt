cmake_minimum_required(VERSION 3.13)
project(SiI3512Startup C)

# Faceless Startup-Items auto-mount app for the SiI3512 driver (Goal-2 shipping vehicle).
# The proven v56 probe, automated: waits for the system to settle after login, then
# installs the driver (kInitialize brings up the card + AddDrive) and PBMountVol's the
# eSATA HFS volume(s). No window; auto-runs from the System Folder's "Startup Items".
# Reuses the real driver sources unchanged; embeds the driver PEF as 'sPEF' 128.
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
# NOTE: regenerate driver_pef.r whenever the driver changes:
#   ../probe/embed-pef.sh ../build/SiI3512SATA.pef driver_pef.r sPEF 128
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(SiI3512Startup
    sil_startup_app.c
    sil_startup.r            # SIZE (onlyBackground = faceless) + vers
    driver_pef.r             # embedded driver PEF ('sPEF' 128) for InstallDriverFromMemory
    ../src/sil3512_os.c      # PCI enable / BAR5 map / DMA / find-node / bring-up
    ../src/sil3512_hw.c      # reset + SATA link-up
    ../src/sil3512_ata.c     # IDENTIFY + DMA read/write
    ../src/sil3512_scsi.c    # linked so sil_os_open resolves; not exercised here
    ../src/sil3512_disk.c    # drive-queue + block I/O (kInitialize scan + AddDrive)
    TYPE "APPL" CREATOR "Si3S")

target_include_directories(SiI3512Startup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Same CFM import libs as the probe, incl. DriverLoaderLib for InstallDriverFromMemory.
target_link_libraries(SiI3512Startup
    PCILib DriverServicesLib NameRegistryLib DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(SiI3512Startup retrocrt)
endif()
