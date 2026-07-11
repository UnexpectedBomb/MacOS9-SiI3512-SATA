cmake_minimum_required(VERSION 3.13)
project(SiI3512Probe C)

# On-hardware probe/loader for the SiI3512 driver, as a double-clickable PPC app
# (Retro68 can't build an auto-loading PPC INIT; an app is the validation path).
# Reuses the real driver sources unchanged.
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_application(SiI3512Probe
    sil_probe_app.c
    sil_probe.r             # SIZE partition + vers
    driver_pef.r            # embedded driver PEF ('sPEF' 128) for InstallDriverFromMemory
    ../src/sil3512_os.c      # PCI enable / BAR5 map / DMA / find-node / bring-up
    ../src/sil3512_hw.c      # reset + SATA link-up
    ../src/sil3512_ata.c     # IDENTIFY + DMA read
    ../src/sil3512_scsi.c    # linked so sil_os_open resolves; not exercised here
    ../src/sil3512_disk.c    # linked so sil_os_initialize's disk-scan ref resolves
    TYPE "APPL" CREATOR "Si35")

target_include_directories(SiI3512Probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Same CFM import libs the driver uses, plus DriverLoaderLib for InstallDriverFromMemory.
target_link_libraries(SiI3512Probe
    PCILib DriverServicesLib NameRegistryLib DriverLoaderLib InterfaceLib)

if(TARGET retrocrt)
    add_dependencies(SiI3512Probe retrocrt)
endif()

# Wrap the HFS .dsk into an APM .img so OS 9 will mount it (raw HFS won't).
set(WRAP_APM ${CMAKE_CURRENT_SOURCE_DIR}/../../client/scripts/wrap-apm.py)
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/SiI3512Probe.img
    COMMAND /usr/bin/env python3 ${WRAP_APM}
            ${CMAKE_CURRENT_BINARY_DIR}/SiI3512Probe.dsk
            ${CMAKE_CURRENT_BINARY_DIR}/SiI3512Probe.img SiI3512Probe
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/SiI3512Probe.dsk ${WRAP_APM}
    COMMENT "Wrapping SiI3512Probe.dsk -> SiI3512Probe.img" VERBATIM)
add_custom_target(SiI3512Probe_disk ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/SiI3512Probe.img)
add_dependencies(SiI3512Probe_disk SiI3512Probe_APPL)
