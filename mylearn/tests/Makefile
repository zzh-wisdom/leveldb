CC := gcc
CXX := g++
CFLAGS := -lleveldb -lpthread -g  # -g选项用于调试

DIR_BIN := bin

SRCS := $(wildcard *.c)
DEPS := $(SRCS:.c=.d)

# 可以换成其他程序
test01: test01.o
	$(CXX) $(CFLAGS) -o $(DIR_BIN)/$@ $^

# 模式规则，产生.o文件
%.o : %.c
	$(CC) $(CFLAGS) -o $@ -c $(filter %.c, $^)

%.o : %.cpp
	$(CXX) $(CFLAGS) -o $@ -c $(filter %.cpp, $^)

include $(DEPS)

# 模式规则，产生.d文件
%.d : %.c
	@set -e;\
	$(CC) -MM $(CFLAGS) $(filter %.c,$^) | sed 's,\(.*\)\.o[ :]*,\1.o $@: ,g' > $@

%.d : %.cpp
	@set -e;\
	$(CXX) -MM $(CFLAGS) $(filter %.c,$^) | sed 's,\(.*\)\.o[ :]*,\1.o $@: ,g' > $@

format:
	@#for f in $(shell find . -name '*.c' -or -name '*.cpp' -or -name '*.h' -type f); do astyle $$f; done
	astyle --recursive *.h,*.c,*.cpp

doc:
	doxygen Doxyfile

clean:
	rm -f *.o bin/* *.d