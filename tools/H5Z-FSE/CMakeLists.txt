cmake_minimum_required(VERSION 3.16)

project(H5Z_FSE C)

find_package(HDF5 COMPONENTS C REQUIRED)

# If HDF5 is parallel, resolve MPI via CMake instead of hard-coded OpenMPI paths.
if(HDF5_IS_PARALLEL)
    find_package(MPI REQUIRED COMPONENTS C)
    if(NOT MPI_C_FOUND)
        message(FATAL_ERROR "Parallel HDF5 detected, but MPI C was not found.")
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
)

if(HDF5_IS_PARALLEL)
    target_include_directories(H5Zfse PRIVATE ${MPI_C_INCLUDE_DIRS})
endif()

target_link_libraries(H5Zfse PRIVATE
    fse_core
    ${HDF5_LIBRARIES}
)

if(HDF5_IS_PARALLEL)
    target_link_libraries(H5Zfse PRIVATE MPI::MPI_C)
endif()

set_target_properties(H5Zfse PROPERTIES
    OUTPUT_NAME H5Zfse
)
