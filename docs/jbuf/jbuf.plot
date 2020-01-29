#!/usr/bin/gnuplot
#
# This gnuplot script plots DEBUG_LEVEL 6 output of jbuf.c. You have to
# increment the DEBUG_LEVEL in jbuf.c if you want to get the table for
# jbuf.dat. Then call baresip like this:
#
# ./baresip 2>&1 | grep -E -o "jbuf_jitter_calc.*" > jbuf.dat
#
# Call this script. Then compare the plot legend with the variables in jbuf.c!
#
# Copyright (C) 2020 commend.com - Christian Spielberger

#set terminal x11
set terminal postscript eps size 15,10 enhanced color
set output 'jbuf.eps'
set datafile separator ","
set key outside
#set yrange [0:]
plot 'jbuf.dat' using 2:3 title 'diff', \
	'jbuf.dat' using 2:4 title 'jitter', \
	'jbuf.dat' using 2:5 title 'buf', \
	'jbuf.dat' using 2:6 title 'avbuf', \
	'jbuf.dat' using 2:7 title 'bufmin', \
	'jbuf.dat' using 2:8 title 'bufmax', \
	'jbuf.dat' using 2:($9*10) title 'G/L/H', \
	20 title "", 40 title ""
#plot 'jbuf.dat' using 2:4 title 'jitter', \
#    'jbuf.dat' using 2:6 title 'avbuf', \
#    'jbuf.dat' using 2:7 title 'bufmin', \
#    'jbuf.dat' using 2:8 title 'bufmax', \
#    'jbuf.dat' using 2:($9*10) title 'G/L/H', \
#    20 title "", 40 title ""
