cmake_minimum_required(VERSION 3.16)

project(H5Z_FSE C)

find_package(HDF5 COMPONENTS C REQUIRED)
if(HDF5_IS_PARALLEL)
    find_path(H5Z_FSE_MPI_INCLUDE_DIR mpi.h
        PATHS
            /usr/lib/x86_64-linux-gnu/openmpi/include
            /usr/include/openmpi
    )
    find_library(H5Z_FSE_MPI_LIB mpi
        PATHS
            /usr/lib/x86_64-linux-gnu/openmpi/lib
            /usr/lib/x86_64-linux-gnu
    )
    if(NOT H5Z_FSE_MPI_INCLUDE_DIR OR NOT H5Z_FSE_MPI_LIB)
        message(FATAL_ERROR "Parallel HDF5 detected, but mpi.h/libmpi were not found.")
    endif()
endif()

add_library(H5Zfse SHARED
    src/H5Z-FSE.c
)

set_property(TARGET H5Zfse PROPERTY POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(H5Zfse PRIVATE -Wl,-Bsymbolic)
endif()

target_include_directories(H5Zfse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/fse/include
    ${HDF5_INCLUDE_DIRS}
    $<$<BOOL:${HDF5_IS_PARALLEL}>:${H5Z_FSE_MPI_INCLUDE_DIR}>
)

target_link_libraries(H5Zfse PRIVATE
    fse_core
    ${HDF5_LIBRARIES}
    $<$<BOOL:${HDF5_IS_PARALLEL}>:${H5Z_FSE_MPI_LIB}>
)

set_target_properties(H5Zfse PROPERTIES
    OUTPUT_NAME H5Zfse
)
