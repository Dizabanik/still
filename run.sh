#!/bin/bash

make
rm output.bc
./kawac test.kawa

lli output.bc
