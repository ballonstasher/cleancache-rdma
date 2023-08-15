#!/bin/bash 

ARGS=$1

echo $ARGS
if [ -z $ARGS ]; then
  cd build && make && cd -
else
  cd build && cmake $ARGS .. && make && cd -
fi
