cmake_minimum_required(VERSION 3.16)

project(FiniteStateEntropyMinimal C)

add_subdirectory(fse)

option(BUILD_H5Z_FSE "Build HDF5 filter plugin (H5Zfse)" ON)
if(BUILD_H5Z_FSE)
    add_subdirectory(tools/H5Z-FSE)
endif()
