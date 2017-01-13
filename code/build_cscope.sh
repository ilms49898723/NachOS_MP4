#!/bin/bash

rm -f cscope.*
find ./ -name "*.h" -o -name "*.c" -o -name "*.cc" > cscope.files
cscope -Rbq -i cscope.files

