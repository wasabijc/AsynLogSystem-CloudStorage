test:Test.cpp base64.cpp
	g++ -o $@ $^ -std=c++17 -lpthread -lstdc++fs -ljsoncpp -lbundle -levent 
gdb_test:Test.cpp
	g++ -g -o $@ $^ -std=c++17 -lpthread -lstdc++fs -ljsoncpp  -lbundle -levent
.PHONY:clean
clean:
	rm -rf test gdb_test ./deep_storage ./low_storage ./logfile storage.data
