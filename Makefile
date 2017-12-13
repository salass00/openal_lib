OPENALDIR = openal-soft-1.18.2

.PHONY: all
all: build-openal

.PHONY: build-openal
build-openal:
	$(MAKE) -C $(OPENALDIR) -f makefile.amigaos4

.PHONY: clean
clean:
	$(MAKE) -C $(OPENALDIR) -f makefile.amigaos4 clean

.PHONY: install
install:
	$(MAKE) -C $(OPENALDIR) -f makefile.amigaos4 install

