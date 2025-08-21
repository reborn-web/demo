win_test.exe: win_test.cpp
	g++ -std=c++11 -O2 -o $@ $<
