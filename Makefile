.PHONY: all build doc
all: build install

build:
	mkdir -p build && cd build && \
	cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .

INSTALL_PATH := /usr/local
INCLUDE_DIR := #leveldb # 由于代码中的头文件都放在include/leveldb目录下，因此这里不需要另外指明为leveldb
LIBRARY_DIR := build
LIBRARY := libleveldb.a

install:install-static-lib

install-headers:
	install -d $(INSTALL_PATH)/lib
	install -d $(INSTALL_PATH)/include/$(INCLUDE_DIR)
	for header_dir in `find "include" -type d | sed '1d' | sed 's/^include\///g' `; do \
		install -d $(INSTALL_PATH)/include/$(INCLUDE_DIR)/$$header_dir; \
	done
	for header in `find "include/" -type f -name "*.h" | sed 's/^include\///g' `; do \
		install -C -m 644 include/$$header $(INSTALL_PATH)/include/$(INCLUDE_DIR)/$$header; \
	done

install-static-lib: install-headers 
	install -C -m 755 $(LIBRARY_DIR)/$(LIBRARY) $(INSTALL_PATH)/lib

uninstall:
	rm -rf $(INSTALL_PATH)/include/$(INCLUDE_DIR)/leveldb \
	$(INSTALL_PATH)/lib/$(LIBRARY)

format:
	@#for f in $(shell find . -name '*.c' -or -name '*.cpp' -or -name '*.h' -type f); do astyle $$f; done
	astyle --recursive *.h,*.c,*.cpp

doc:
	doxygen Doxyfile

clean:
	rm -f *.o bin/* *.d