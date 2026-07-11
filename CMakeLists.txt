cmake_minimum_required(VERSION 3.13)
project(SiI3512SATA C)

# Native PCI driver for the SiI3512 (LaCie 130823). Pipeline mirrors ../usb2-ehci:
# SHARED lib -> MakePEF -> Rez wrap as file type 'ndrv'. M2 adds the bring-up/
# command-engine sources and links the OS import libs.
add_library(SiI3512SATA SHARED
    src/sil3512_driver.c   # DriverDescription + DoDriverIO dispatch
    src/sil3512_os.c       # OS 9 glue: PCI enable, BAR5 map, DMA/PRD, handlers
    src/sil3512_hw.c       # controller bring-up (register programming)
    src/sil3512_ata.c      # ATA command engine (IDENTIFY, READ/WRITE DMA)
    src/sil3512_scsi.c     # SCSI Manager 4.3 SIM + SCSI-to-ATA translation
    src/sil3512_disk.c)    # drive-queue + block I/O (resident-driver mount path)
# Export DoDriverIO + TheDriverDescription. A native driver must have DoDriverIO
# BOTH exported AND as the fragment `main` (confirmed by diffing Apple's shipping
# NVIDIA ndrv). We don't use -e (it drops the symbol from exports); instead the
# post-MakePEF patch step below points `main` at the DoDriverIO export.
set_target_properties(SiI3512SATA PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/src/sil3512_driver.exp")

# CFM import libraries the OS glue calls into (all present in the toolchain),
# same set ../usb2-ehci links.
target_link_libraries(SiI3512SATA
    PCILib DriverServicesLib NameRegistryLib DriverLoaderLib InterfaceLib)

add_custom_command(OUTPUT SiI3512SATA.pef
    COMMAND ${MAKE_PEF} libSiI3512SATA.so -o SiI3512SATA.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py
            SiI3512SATA.pef DoDriverIO
    DEPENDS SiI3512SATA ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py)

if (REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

add_custom_command(OUTPUT SiI3512SATA.ndrv
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/sil3512.r
            ${REZ_INCLUDE_FLAG}
            -o SiI3512SATA.ndrv --cc SiI3512SATA.bin
            -t "ndrv" -c "????"
            --data SiI3512SATA.pef
    DEPENDS SiI3512SATA.pef)

add_custom_target(ndrv ALL DEPENDS SiI3512SATA.ndrv)
