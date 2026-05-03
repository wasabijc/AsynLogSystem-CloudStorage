# 异步日志子系统功能测试 Makefile
# 用法：
#   make                  编译
#   make run              编译并运行
#   make clean            清理产物
#
# 注意：JsonData 内部使用相对路径 "../../log_system/logs_code/config.conf"
# 因此运行测试时的工作目录必须是 log_system/tests/，本 Makefile 已保证这一点。

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-result -pthread
LDFLAGS  := -pthread
LIBS     := -ljsoncpp

TARGET   := functional_test
SRC      := functional_test.cpp \
            ../logs_code/backlog/ServerBackupLog.cpp

# 仅链接需要的源；ServerBackupLog.cpp 含有 main()，会与测试 main 冲突，
# 因此默认不参与链接，只编译 functional_test.cpp。
OBJ_SRC  := functional_test.cpp

all: $(TARGET)

$(TARGET): $(OBJ_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	-rm -f $(TARGET)
	-rm -rf ./test_logfile

.PHONY: all run clean