win_test.exe: win_test.cpp
	g++ -std=c++11 -O2 -o $@ $<

sem_test: sem_test.cpp
	g++ -std=c++11 -O2 -pthread -o $@ $<

test: test.cpp
	g++ -std=c++20 -O2 -pthread -o $@ $<
