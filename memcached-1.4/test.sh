#!/bin/bash
set j=2
while true
do
	echo "flush_all" | nc 127.0.0.1 7001
	echo "flush_all" | nc 127.0.0.1 7002
	echo "flush_all" | nc 127.0.0.1 7003
        usleep 30000        
	./testapp
done

