# Chapter 10 examples: remote debugging. meter is built with debug info
# here; device/meter is what goes on the device: no debug info, only a
# link to meter.debug (and its build ID).
CFLAGS := -std=c11 -Wall -Wextra -g -O2
PROGS  := meter poller

all: $(PROGS) device/meter debug

# objcopy --only-keep-debug: the debug info alone; then everything but
# the code out, and a .gnu_debuglink section (name + CRC of meter.debug).
device/meter: meter
	objcopy --only-keep-debug meter meter.debug
	objcopy --strip-all --add-gnu-debuglink=meter.debug meter $@

# The debug info where gdb looks for it by build ID, as a distribution
# installs it: DIR/.build-id/7a/476c...debug for build ID 7a476c...
debug: device/meter
	id=$$(readelf -n device/meter | sed -n 's/.*Build ID: //p'); \
	mkdir -p debug/.build-id/$$(echo $$id | cut -c1-2) && \
	cp meter.debug debug/.build-id/$$(echo $$id | cut -c1-2)/$$(echo $$id | cut -c3-).debug

clean:
	rm -rf $(PROGS) meter.debug device/meter device/ready.flag debug core core.*

.PHONY: all clean debug
