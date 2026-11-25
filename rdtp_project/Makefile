CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Wpedantic

all: rdt_sender rdt_receiver

rdt_sender: rdt_sender.cpp rdtp_common.h
	$(CXX) $(CXXFLAGS) rdt_sender.cpp -o rdt_sender

rdt_receiver: rdt_receiver.cpp rdtp_common.h
	$(CXX) $(CXXFLAGS) rdt_receiver.cpp -o rdt_receiver

clean:
	rm -f rdt_sender rdt_receiver rdtp_metrics.csv

.PHONY: all clean
