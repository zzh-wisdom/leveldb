
all: build install

build:
	mkdir -p build && cd build
	cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .

INSTALL_PATH := /usr/local
INCLUDE_DIR := #leveldb
LIBRARY_DIR := build
LIBRARY := libleveldb.a

install-headers:
	install -d $(INSTALL_PATH)/lib
	install -d $(INSTALL_PATH)/include/$(INCLUDE_DIR)
	for header_dir in `find "include" -type d | sed '1d' | sed 's/^include\///g' `; do \
		install -d $(INSTALL_PATH)/include/$(INCLUDE_DIR)/$$header_dir; \
	done
	for header in `find "include/" -type f -name "*.h" | sed 's/^include\///g' `; do \
		install -C -m 644 include/$$header $(INSTALL_PATH)/include/$(INCLUDE_DIR)/$$header; \
	done

install-static: install-headers 
	install -C -m 755 $(LIBRARY_DIR)/$(LIBRARY) $(INSTALL_PATH)/lib

install:install-static

uninstall:
	rm -rf $(INSTALL_PATH)/include/$(INCLUDE_DIR) \
	$(INSTALL_PATH)/lib/$(LIBRARY)