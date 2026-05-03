# 异步日志子系统测试 Makefile
# ====== 目标 ======
#   make              编译功能测试
#   make run          编译并运行功能测试
#   make perf         编译性能基准
#   make perf-run     编译并运行性能基准 (全量)
#   make perf-quick   编译并运行性能基准 (快速)
#   make clean        清理所有构建产物与临时日志目录
#
# 注意：JsonData 内部使用相对路径 "../../log_system/logs_code/config.conf"
# 因此运行测试时的工作目录必须是 log_system/tests/，本 Makefile 已保证这一点。

CXX       := g++
CXXFLAGS  := -std=c++17 -O2 -Wall -Wno-unused-result -pthread
LDFLAGS   := -pthread
LIBS      := -ljsoncpp

# --- 功能测试 ---
FUNC_TARGET := functional_test
FUNC_SRC    := functional_test.cpp

# --- 性能测试 ---
PERF_TARGET := performance_test
PERF_SRC    := performance_test.cpp

.PHONY: all run perf perf-run perf-quick clean

all: $(FUNC_TARGET)

$(FUNC_TARGET): $(FUNC_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

run: $(FUNC_TARGET)
	./$(FUNC_TARGET)

# 性能测试：使用 -O3 + -DNDEBUG 减少断言/assert 干扰
$(PERF_TARGET): $(PERF_SRC)
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wno-unused-result -pthread \
		-o $@ $^ $(LDFLAGS) $(LIBS)

perf: $(PERF_TARGET)

perf-run: $(PERF_TARGET)
	./$(PERF_TARGET)

perf-quick: $(PERF_TARGET)
	./$(PERF_TARGET) --quick

clean:
	-rm -f $(FUNC_TARGET) $(PERF_TARGET)
	-rm -rf ./test_logfile ./perf_logfile