#!/bin/sh

cd tf-a-stm32mp-v2.4-stm32mp-r2
export FIP_DEPLOYDIR_ROOT=$PWD/../../FIP_artifacts
make -f ../Makefile.sdk clean
make -f ../Makefile.sdk all

