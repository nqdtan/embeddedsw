#!/bin/bash

make clean
make -f Makefile.Linux CFLAGS+=-DPL
