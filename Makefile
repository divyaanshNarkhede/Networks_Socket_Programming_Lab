# Makefile for the link-state emulator.
#
#   make                         build the virtual node (./vn)
#   make oracle [IP=<my-ip>]     run the oracle with config_file.txt
#   make clean                   remove build files

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
CONFIG   = config_file.txt
IP       =

.PHONY: all oracle clean

all: vn

vn: vn.cpp
	$(CXX) $(CXXFLAGS) -o vn vn.cpp

oracle:
	python3 oracle.py $(CONFIG) $(IP)

clean:
	rm -f vn
	rm -rf __pycache__
